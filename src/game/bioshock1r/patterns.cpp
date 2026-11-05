// eventPlayerCalcView derivation: FName-chain scan, technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT), src/memory.rs - the
// generic implementation lives in core/hooks/pattern_scan.cpp. The logged RVA
// is the cross-check value against the Rust mod on the same exe build
// (docs/bioshock1/ENGINE_NOTES.md).

#include "game/bioshock1r/patterns.h"

#include "core/util/log.h"
#include "core/util/module_id.h"

#include <windows.h>

#include <cstring>

namespace bvr::b1r::patterns {
namespace {

// Captured by resolve() so the lazy hfov_option_ptr() can form the vtable
// address for this session's ASLR base.
const uint8_t* g_imageBase = nullptr;

// Set once by resolve(): does the running exe match the build every RVA in
// patterns.h came from? See the header's build-identity block.
bool g_rvaTrusted = false;
// Test override for the stand-down path - see handle_buildgate_command.
bool g_buildGateForcedOff = false;

// ---- live-object search (session 27 rewrite) --------------------------------
// The generic machinery - thread-stack/TEB exclusion, the live-heap fast path,
// the budgeted sliced fallback - lives in core/hooks/heap_scan.h. This file
// supplies only the per-class accept filters and the dormancy policy, because
// those are the parts that know what a BioShock object looks like.
//
// What this replaced swept the whole 4 GB address space at a 4-byte stride in
// one blocking call, accepting any MEM_PRIVATE + PAGE_READWRITE region - which
// is also the signature of a thread stack. Field consequences: multi-second
// game-thread stalls, and a candidate that could be a stack slot or a freed
// block while the FOV writer poked it every frame. Full post-mortem in
// heap_scan.h and ENGINE_NOTES session 27.

// Cached UShockUserSettings instance (revalidated every call). There is no
// static pointer to it, so we find it by its fixed-RVA vtable and cache the
// address for the session.
void* g_userSettings = nullptr;
uint64_t g_lastSettingsAttemptMs = 0;
int g_settingsMisses = 0;
bool g_settingsDormant = false;
ObjectScan g_settingsScan;

// Probe slots for the candidate log line: [0] HorizontalFOV, [1] 1 if the
// UObject class chain resolved, [2] the class name's FName index.
bool accept_user_settings(void* obj, void*, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    int32_t fov = *reinterpret_cast<const int32_t*>(p + kUserSettingsHfovOffset);
    probe[0] = fov;

    // CORROBORATION (session 27). The vtable dword is one predicate, and it is
    // satisfied by any 4 bytes that happen to hold that value: a stack slot
    // spilling the pointer, or a freed object whose header the allocator has
    // not reused yet. The consumer WRITES to whatever comes back, so one
    // predicate is not enough. object_class_name walks obj -> +0x30 UClass* ->
    // UClass vtable identity -> +0x28 FName index -> GNames with every step
    // validated, so a non-null result is strong evidence of a live UObject.
    //
    // Deliberately NOT compared against an expected name: the live instance
    // could be a subclass and rejecting it would silently disable FOV control.
    // The resolved name is logged instead, so tightening this to an exact match
    // later is a one-line change backed by real data.
    const wchar_t* cls = object_class_name(obj);
    probe[1] = cls ? 1 : 0;
    if (!cls) return false;
    const uint8_t* clsObj = *reinterpret_cast<const uint8_t* const*>(p + kUObjectClassOffset);
    probe[2] = *reinterpret_cast<const int32_t*>(clsObj + kUObjectNameIndexOffset);

    // The engine's own options UI clamps to 75..130. The 40..170 window this
    // used to accept let far more debris through.
    return fov >= kUserSettingsHfovMin && fov <= kUserSettingsHfovMax;
}

// ---- Unrecognised builds ---------------------------------------------------
// Every class-table address below is re-derived from the running image; a
// value that cannot be derived stays unusable rather than falling back to the
// Steam number, and every code target must pass a shape check before hooking.

bool g_gnamesBound = true; // only an unrecognised build has to look for it

bool rebind_vtable(const bvr::pattern_scan::ProcessImage& image, const char* mangled,
                   uint32_t& rva) {
    const uint8_t* vtable = bvr::pattern_scan::find_rtti_vtable(image, mangled);
    if (!vtable) {
        BVR_LOG("[b1r] rebind %s: NOT found - dependent features stay off", mangled);
        rva = 0;
        return false;
    }
    uint32_t found = static_cast<uint32_t>(vtable - image.base);
    BVR_LOG("[b1r] rebind %s: RVA 0x%X (Steam build 0x%X)", mangled, found, rva);
    rva = found;
    return true;
}

void rebind_vtables(const bvr::pattern_scan::ProcessImage& image) {
    rebind_vtable(image, ".?AVUShockUserSettings@@", kUserSettingsVtableRva);
    rebind_vtable(image, ".?AVUAttackAbility@@", kAttackAbilityVtableRva);
    rebind_vtable(image, ".?AVAShockPlayer@@", kShockPlayerVtableRva);
    rebind_vtable(image, ".?AVAPlayerWeapon@@", kPlayerWeaponVtableRva);
    rebind_vtable(image, ".?AVAHands@@", kHandsVtableRva);
    rebind_vtable(image, ".?AVUWindowsClient@@", kClientVtableRva);
    rebind_vtable(image, ".?AVUWindowsViewport@@", kViewportVtableRva);
    rebind_vtable(image, ".?AVUGameEngine@@", kEngineVtableRva);
    rebind_vtable(image, ".?AVUClass@@", kUClassVtableRva);
}

// A fire-start implementation ends in `ret imm16` and reads the documented
// pawn fields. Matching all of that by accident is what this refuses to assume.
bool has_disp32(const uint8_t* begin, const uint8_t* end, uint32_t value) {
    for (const uint8_t* p = begin; p + sizeof(value) <= end; ++p) {
        if (memcmp(p, &value, sizeof(value)) == 0) return true;
    }
    return false;
}

bool fire_start_shape(const uint8_t* fn, uint16_t retBytes) {
    using bvr::pattern_scan::is_memory_valid;
    constexpr size_t kMaxBody = 0x600;
    if (!is_memory_valid(fn, kMaxBody)) return false;
    const uint8_t* end = nullptr;
    for (const uint8_t* p = fn; p + 3 <= fn + kMaxBody; ++p) {
        if (p[0] == 0xC2 && memcmp(p + 1, &retBytes, sizeof(retBytes)) == 0) {
            end = p;
            break;
        }
    }
    return end && has_disp32(fn, end, kFireStartLocationOffset) &&
           has_disp32(fn, end, kFireStartViewOffset);
}

// UAttackAbility::GetPerfectFireStart has no vtable slot of its own: it is a
// direct call out of an ability virtual. Exactly one shape match, or nothing.
const uint8_t* find_ability_fire_start(const bvr::pattern_scan::ProcessImage& image,
                                       const uint8_t** callerOut) {
    using bvr::pattern_scan::is_memory_valid;
    constexpr uint32_t kMaxVtableBytes = 0x340;
    constexpr size_t kCallerWindow = 0x500;
    if (!kAttackAbilityVtableRva) return nullptr;
    const uint8_t* vtable = image.base + kAttackAbilityVtableRva;
    const uint8_t* imageEnd = image.base + image.size;
    const uint8_t* match = nullptr;
    for (uint32_t off = 0; off < kMaxVtableBytes; off += sizeof(void*)) {
        if (!is_memory_valid(vtable + off, sizeof(void*))) break;
        const uint8_t* impl = *reinterpret_cast<const uint8_t* const*>(vtable + off);
        if (impl < image.base || impl >= imageEnd) break; // end of the table
        if (!is_memory_valid(impl, kCallerWindow)) continue;
        for (const uint8_t* p = impl; p + 5 <= impl + kCallerWindow; ++p) {
            if (*p != 0xE8) continue;
            int32_t rel = 0;
            memcpy(&rel, p + 1, sizeof(rel));
            const uint8_t* target = p + 5 + rel;
            if (target < image.base || target + sizeof(kAbilityFireStartPrologue) >= imageEnd)
                continue;
            if (!is_memory_valid(target, sizeof(kAbilityFireStartPrologue))) continue;
            if (memcmp(target, kAbilityFireStartPrologue,
                       sizeof(kAbilityFireStartPrologue)) != 0)
                continue;
            if (!fire_start_shape(target, kAbilityFireStartRetBytes)) continue;
            if (match && match != target) return nullptr; // ambiguous: refuse
            match = target;
            if (callerOut) *callerOut = impl;
        }
    }
    return match;
}

// The name table is filled during engine init, so this runs on demand: three
// dwords { Data, Count, Max } in the image's writable data whose first entries
// each carry their own index at +0.
bool bind_gnames() {
    using bvr::pattern_scan::is_memory_valid;
    constexpr int32_t kMinNames = 1000, kMaxNames = 2000000, kProbe = 32;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_imageBase + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    uint32_t found = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* begin = g_imageBase + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        for (size_t off = 0; off + 12 <= size; off += 4) {
            const uint8_t* p = begin + off;
            auto data = *reinterpret_cast<const uint8_t* const* const*>(p);
            int32_t count = *reinterpret_cast<const int32_t*>(p + 4);
            int32_t max = *reinterpret_cast<const int32_t*>(p + 8);
            if (count < kMinNames || count > kMaxNames || max < count || !data) continue;
            if (!is_memory_valid(data, kProbe * sizeof(void*))) continue;
            bool ok = true;
            for (int32_t n = 0; ok && n < kProbe; ++n) {
                const uint8_t* entry = data[n];
                ok = entry && is_memory_valid(entry, kFNameEntryTextOffset + 2) &&
                     *reinterpret_cast<const int32_t*>(entry) == n;
            }
            if (!ok) continue;
            if (found) { // two tables would mean this shape test is wrong
                BVR_LOG("[b1r] name table ambiguous - class names stay off");
                return false;
            }
            found = static_cast<uint32_t>(p - g_imageBase);
        }
    }
    if (!found) return false;
    kGNamesDataRva = found;
    BVR_LOG("[b1r] rebind name table: RVA 0x%X", found);
    return true;
}

// Reads a pointer target without faulting. VirtualQuery results are cached per
// region because a data-section scan feeds this millions of arbitrary values.
class ReadableCache {
public:
    bool readable(const void* p) {
        auto a = reinterpret_cast<uintptr_t>(p);
        if (a < lo_ || a + sizeof(void*) > hi_) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
            lo_ = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            hi_ = lo_ + mbi.RegionSize;
            good_ = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        }
        return good_;
    }
private:
    uintptr_t lo_ = 0, hi_ = 0;
    bool good_ = false;
};

// The engine global: a slot in the image's writable data holding the one object
// whose vtable is UGameEngine's. Identity comes from the vtable, not a guess.
bool bind_engine_ptr() {
    if (!kEngineVtableRva) return false;
    const void* want = g_imageBase + kEngineVtableRva;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_imageBase + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    ReadableCache cache;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* begin = g_imageBase + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        for (size_t off = 0; off + sizeof(void*) <= size; off += sizeof(void*)) {
            const void* object = *reinterpret_cast<const void* const*>(begin + off);
            if ((reinterpret_cast<uintptr_t>(object) & 3) != 0 || !object) continue;
            if (!cache.readable(object)) continue;
            if (*reinterpret_cast<const void* const*>(object) != want) continue;
            kGameEnginePtrRva = static_cast<uint32_t>(begin + off - g_imageBase);
            BVR_LOG("[b1r] rebind engine global: RVA 0x%X", kGameEnginePtrRva);
            return true;
        }
    }
    return false;
}

bool g_enginePtrBound = true; // only an unrecognised build has to look for it

bool gnames_ready() {
    if (g_gnamesBound) return true;
    static DWORD nextTry = 0;
    DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - nextTry) < 0) return false;
    nextTry = now + 2000; // the scan walks all of .data - never per call
    g_gnamesBound = bind_gnames();
    return g_gnamesBound;
}

} // namespace

void log_scan_result(const char* what, const ObjectScan& scan, const ScanResult& r,
                     bool candidates) {
    // A refusal already said so once, in run_object_scan. Repeating a summary for
    // a search that never ran turns the retry cadence into log spam - which the
    // wrong-build test produced, one line every 2 s for the whole session.
    if (r.disabled) return;
    const bvr::heap_scan::Sweep& s = scan.sweep;
    if (candidates) {
        for (int i = 0; i < s.recorded; ++i) {
            const wchar_t* cls = object_class_name(s.addr[i]);
            BVR_LOG("[b1r] %s candidate @ %p probe=%d,%d,%d,%d class='%ls' -> %s", what, s.addr[i],
                    s.probe[i][0], s.probe[i][1], s.probe[i][2], s.probe[i][3],
                    cls ? cls : L"<unresolved>", s.ok[i] ? "ACCEPTED" : "rejected");
        }
        if (s.matches > s.recorded)
            BVR_LOG("[b1r] %s: %d further match(es) not recorded (cap %d)", what,
                    s.matches - s.recorded, bvr::heap_scan::kMaxRecorded);
    }
    // s62f: dead/faulted heap counts ride the same line. Before v0.8.2 either
    // one was a process-killing access violation (a heap destroyed by a level
    // load while this scan held its handle); now they are survivable skips, so
    // seeing them here is the guard working.
    char guarded[96] = "";
    if (s.deadHeaps || s.faultedHeaps)
        sprintf_s(guarded, " - SKIPPED %u dead + %u faulted heap(s), survived",
                  s.deadHeaps, s.faultedHeaps);
    BVR_LOG("[b1r] %s scan via %s: %d match(es), %d accepted, chosen=%p "
            "(%.1f ms, %u slice(s), %u heap(s)/%u block(s), %d exclude span(s)%s%s)",
            what, r.path, r.matches, r.accepted, r.object,
            static_cast<double>(s.elapsedUs) / 1000.0, s.slices, s.heaps, s.blocks,
            s.excludeSpans, guarded,
            s.excludes.truncated  ? " - EXCLUSION TABLE FULL, SOME STACKS NOT EXCLUDED"
            : s.excludeMissed > 0 ? " - SOME THREAD STACKS NOT EXCLUDED"
            : s.excludeMissed < 0 ? " - THREAD ENUMERATION FAILED"
                                  : "");
}

bool run_object_scan(ObjectScan& scan, uint32_t vtableRva, uint32_t needBytes,
                     ObjectAccept accept, void* user, ScanResult& out) {
    out = ScanResult{};
    if (!g_imageBase || !accept) return true; // nothing to search: complete and empty
    // On an unrecognised build the vtable RVA is an arbitrary constant, and the
    // callers of this function WRITE to what it returns. Searching memory for a
    // meaningless value and then poking the winner is the one failure mode worth
    // refusing outright (session 27).
    if (!g_rvaTrusted) {
        static bool once = false;
        if (!once) {
            once = true;
            BVR_LOG("[b1r] object scans DISABLED - the host build is not the one these vtable "
                    "addresses came from (see the build block above)");
        }
        out.path = "disabled (unverified build)";
        out.disabled = true;
        return true;
    }
    const void* wantVtable = g_imageBase + vtableRva;

    // Fast path first: only live, allocated heap blocks, tens of milliseconds,
    // and it cannot hand back a freed block. When it succeeds the sliced sweep
    // never runs at all.
    if (!scan.sweeping) {
        if (bvr::heap_scan::heap_blocks(scan.sweep, wantVtable, needBytes, accept, user)) {
            out.object = scan.sweep.first;
            out.matches = scan.sweep.matches;
            out.accepted = scan.sweep.accepted;
            out.path = "heap";
            return true;
        }
        // Not in any Win32 heap block: either the object does not exist yet, or
        // this engine allocates it from its own pool. Fall back to the region
        // sweep, sliced so it can never stall a frame.
        scan.sweeping = true;
        scan.sweep.cursor = 0;
    }

    auto outcome = bvr::heap_scan::sweep(scan.sweep, wantVtable, needBytes, accept, user,
                                         kScanSliceBudgetUs);
    out.path = "sweep (heap path found nothing)";
    if (outcome == bvr::heap_scan::Outcome::Working) return false;
    scan.sweeping = false;
    out.object = scan.sweep.first;
    out.matches = scan.sweep.matches;
    out.accepted = scan.sweep.accepted;
    return true;
}

int32_t* hfov_option_ptr() {
    using namespace bvr::pattern_scan;
    if (!g_imageBase) return nullptr;

    const void* wantVtable = g_imageBase + kUserSettingsVtableRva;

    // Revalidate the cache on TWO predicates, not one: the vtable dword AND the
    // UObject class chain, so a freed-and-not-yet-reused block cannot keep
    // passing as the settings object while the FOV writer pokes it every frame.
    if (g_userSettings) {
        if (is_memory_valid(g_userSettings, kUserSettingsHfovOffset + sizeof(int32_t)) &&
            *reinterpret_cast<const void* const*>(g_userSettings) == wantVtable &&
            object_class_name(g_userSettings) != nullptr) {
            return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                              kUserSettingsHfovOffset);
        }
        g_userSettings = nullptr;
        hfov_scan_rearm("cached settings object went stale");
    }

    // DORMANCY (backported from bioshock2r; the session-22 weapon-resolver
    // lesson). A rate limit alone means a save where the object never appears
    // rescans forever, which reads as "the game freezes every couple of
    // seconds". Only an explicit re-arm clears this.
    if (g_settingsDormant) return nullptr;

    uint64_t now = GetTickCount64();
    if (!g_settingsScan.sweeping && now - g_lastSettingsAttemptMs < kScanRetryMs) return nullptr;
    g_lastSettingsAttemptMs = now;

    ScanResult r{};
    if (!run_object_scan(g_settingsScan, kUserSettingsVtableRva,
                         kUserSettingsHfovOffset + sizeof(int32_t), &accept_user_settings, nullptr,
                         r))
        return nullptr; // sweep still in progress - no stall, try again next frame

    log_scan_result("UShockUserSettings", g_settingsScan, r, true);

    // FAIL CLOSED on ambiguity: more than one object passed the filter, so we
    // cannot say which one the renderer reads. Writing to the wrong one is
    // worse than not writing at all.
    if (r.accepted > 1) {
        BVR_LOG("[b1r] UShockUserSettings AMBIGUOUS (%d accepted) - refusing to bind, "
                "FOV control stays off",
                r.accepted);
        g_settingsDormant = true;
        return nullptr;
    }

    g_userSettings = r.object;
    if (!g_userSettings) {
        if (++g_settingsMisses >= kScanMissesBeforeDormant) {
            g_settingsDormant = true;
            BVR_LOG("[b1r] UShockUserSettings scan DORMANT after %d miss(es) - a view-state "
                    "change re-arms it",
                    g_settingsMisses);
        }
        return nullptr;
    }
    g_settingsMisses = 0;
    return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                      kUserSettingsHfovOffset);
}

void hfov_scan_rearm(const char* why) {
    g_settingsMisses = 0;
    if (g_settingsDormant) {
        g_settingsDormant = false;
        BVR_LOG("[b1r] UShockUserSettings scan re-armed (%s)", why);
    }
}

bool settings_bound() { return g_userSettings != nullptr; }

bool rva_trusted() { return g_rvaTrusted && !g_buildGateForcedOff; }

void handle_buildgate_command(const char* args) {
    if (args && strncmp(args, "off", 3) == 0) {
        g_buildGateForcedOff = true;
        BVR_LOG("[b1r] buildgate FORCED OFF - address-dependent features now behave exactly as "
                "they would on an unrecognised build (object scans refuse, so no FOV control "
                "and no viewmodel drive). 'buildgate on' restores.");
    } else if (args && strncmp(args, "on", 2) == 0) {
        g_buildGateForcedOff = false;
        // The scanners latched dormant while refusing; wake them.
        hfov_scan_rearm("buildgate restored");
        BVR_LOG("[b1r] buildgate restored (host build itself is %s)",
                g_rvaTrusted ? "VERIFIED" : "STILL UNRECOGNISED");
    } else {
        BVR_LOG("[b1r] buildgate: host build %s, forced-off %s -> rva_trusted=%s",
                g_rvaTrusted ? "VERIFIED" : "NOT RECOGNISED",
                g_buildGateForcedOff ? "yes" : "no", rva_trusted() ? "yes" : "no");
    }
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    g_imageBase = image.base;
    BVR_LOG("[b1r] scanning main module: base %p size 0x%zX", image.base, image.size);

    // Build gate FIRST, so everything logged below is read in the right light.
    const bvr::module_id::Fingerprint host = bvr::module_id::host_exe();
    g_rvaTrusted =
        bvr::module_id::matches(host, kHostTimeDateStamp, kHostSizeOfImage, kHostFileBytes);
    if (g_rvaTrusted) {
        BVR_LOG("[b1r] host build VERIFIED (pe-timestamp 0x%08X) - hardcoded addresses trusted",
                host.timeDateStamp);
    } else {
        BVR_LOG("[b1r] ============================================================");
        BVR_LOG("[b1r] HOST BUILD NOT RECOGNISED - address-dependent features OFF");
        BVR_LOG("[b1r]   running:  pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                host.timeDateStamp, host.sizeOfImage,
                static_cast<unsigned long long>(host.fileBytes));
        BVR_LOG("[b1r]   expected: pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                kHostTimeDateStamp, kHostSizeOfImage,
                static_cast<unsigned long long>(kHostFileBytes));
        BVR_LOG("[b1r] Every offset this mod hardcodes was derived from the 2022-04-13 Steam");
        BVR_LOG("[b1r] build. Applying them to a different binary is how a mod corrupts a");
        BVR_LOG("[b1r] game rather than failing to work, so they stand down and the game");
        BVR_LOG("[b1r] stays fully playable. Head tracking still uses a pattern scan and is");
        BVR_LOG("[b1r] unaffected. If you are on Epic, GOG or a newer patch, please send this");
        BVR_LOG("[b1r] log - the three numbers above are what a per-build offset table needs.");
        BVR_LOG("[b1r] ============================================================");
        g_gnamesBound = false;
        g_enginePtrBound = false;
        rebind_vtables(image);
    }

    EventScanResult scan{};
    bool ok = find_event_function(image, "PlayerCalcView", scan);

    BVR_LOG("[b1r] \"PlayerCalcView\": %zu wide-string match(es), %zu string xref(s)",
            scan.stringMatches, scan.stringXrefs);
    if (scan.fnameIndexGlobal) {
        BVR_LOG("[b1r] fname index global: %p (%zu xref(s), %zu candidate(s) after init-site filter)",
                scan.fnameIndexGlobal, scan.globalXrefs, scan.candidates);
    }
    if (scan.fnameCtor) {
        // Session 20: the FName constructor, kept for the name-system work
        // (GNames derivation in ENGINE_NOTES; resolver below).
        BVR_LOG("[b1r] fname ctor = %p (RVA 0x%X)", scan.fnameCtor,
                static_cast<unsigned>(scan.fnameCtor - image.base));
    }

    if (!ok) {
        const char* stage = scan.stringMatches == 0     ? "wide string not found"
                            : scan.stringXrefs == 0     ? "no string xrefs"
                            : !scan.fnameIndexGlobal    ? "fname index global not found"
                            : scan.candidates == 0      ? "no candidates past init-site filter"
                                                        : "no valid prologue found";
        BVR_LOG("[b1r] scan FAILED (%s) - camera features disabled, game runs flat", stage);
        return false;
    }

    out.eventPlayerCalcView = scan.function;
    BVR_LOG("[b1r] eventPlayerCalcView = %p (RVA 0x%X)", scan.function,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(scan.function) -
                                  reinterpret_cast<uintptr_t>(image.base)));

    resolve_aim_natives(image, out); // fail-soft, logs each
    return true;
}

void resolve_aim_natives(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    // Weapon side: read the implementation straight out of the class vtable
    // (RTTI-derived vtable RVA + the slot the InitiateDamage call site uses).
    // A vtable read survives a rebuild that moves the function; the expected
    // RVA is only a cross-check.
    const void** weaponVtbl =
        reinterpret_cast<const void**>(const_cast<uint8_t*>(image.base) + kPlayerWeaponVtableRva);
    if (kPlayerWeaponVtableRva &&
        is_memory_valid(weaponVtbl, kWeaponFireStartVtblOffset + sizeof(void*))) {
        const uint8_t* impl = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(weaponVtbl) + kWeaponFireStartVtblOffset);
        if (impl >= image.base && impl < image.base + image.size) {
            uint32_t rva = static_cast<uint32_t>(impl - image.base);
            if (rva == kExpectedWeaponFireStartImplRva ||
                fire_start_shape(impl, kWeaponFireStartRetBytes)) {
                out.weaponFireStart = const_cast<uint8_t*>(impl);
                BVR_LOG("[b1r] AWeapon::GetPerfectFireStart impl = %p (RVA 0x%X%s)", impl, rva,
                        rva == kExpectedWeaponFireStartImplRva
                            ? ""
                            : " - shape verified on this build");
            } else {
                BVR_LOG("[b1r] AWeapon::GetPerfectFireStart candidate at RVA 0x%X failed the "
                        "shape check - REFUSING the hook", rva);
            }
        }
    }
    if (!out.weaponFireStart)
        BVR_LOG("[b1r] AWeapon::GetPerfectFireStart impl NOT resolved (vtable read failed)");

    // Same two functions one level up, for the probe's "a shot happened" line.
    if (kPlayerWeaponVtableRva &&
        is_memory_valid(weaponVtbl, kWeaponInitDamageVtblOffset + sizeof(void*))) {
        const uint8_t* impl = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(weaponVtbl) + kWeaponInitDamageVtblOffset);
        if (impl >= image.base && impl < image.base + image.size) {
            out.weaponInitDamage = const_cast<uint8_t*>(impl);
            uint32_t rva = static_cast<uint32_t>(impl - image.base);
            BVR_LOG("[b1r] AWeapon::InitiateDamage impl = %p (RVA 0x%X%s)", impl, rva,
                    rva == kExpectedWeaponInitDamageRva ? "" : " - DIFFERS from the documented build");
        }
    }
    const uint8_t* abilityInit = image.base + kAbilityInitDamageRva;
    if (g_rvaTrusted && is_memory_valid(abilityInit, sizeof kAbilityInitDamagePrologue) &&
        memcmp(abilityInit, kAbilityInitDamagePrologue, sizeof kAbilityInitDamagePrologue) == 0) {
        out.abilityInitDamage = const_cast<uint8_t*>(abilityInit);
        BVR_LOG("[b1r] UAttackAbility::InitiateDamage impl = %p (RVA 0x%X)", abilityInit,
                kAbilityInitDamageRva);
    }

    // Ability side: the InitiateDamage call site calls it directly, so there is
    // no slot to read - use the documented RVA, gated on a prologue match so a
    // patched build refuses rather than hooking a stranger.
    if (!g_rvaTrusted) {
        const uint8_t* caller = nullptr;
        const uint8_t* found = find_ability_fire_start(image, &caller);
        if (found) {
            out.abilityFireStart = const_cast<uint8_t*>(found);
            out.abilityInitDamage = const_cast<uint8_t*>(caller);
            BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart impl = %p (RVA 0x%X - shape "
                    "verified on this build)", found, static_cast<uint32_t>(found - image.base));
        } else {
            BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart not uniquely identified - "
                    "plasmid aim unavailable");
        }
        return;
    }
    const uint8_t* ability = image.base + kAbilityFireStartImplRva;
    if (is_memory_valid(ability, sizeof kAbilityFireStartPrologue) &&
        memcmp(ability, kAbilityFireStartPrologue, sizeof kAbilityFireStartPrologue) == 0) {
        out.abilityFireStart = const_cast<uint8_t*>(ability);
        BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart impl = %p (RVA 0x%X)", ability,
                kAbilityFireStartImplRva);
    } else {
        BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart impl prologue mismatch at RVA 0x%X"
                " - plasmid aim unavailable", kAbilityFireStartImplRva);
    }
}

bool engine_ptr_ready() {
    if (g_enginePtrBound) return true;
    if (!g_imageBase) return false;
    static DWORD nextTry = 0;
    DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - nextTry) < 0) return false;
    nextTry = now + 2000; // the scan walks all of .data - never per call
    g_enginePtrBound = bind_engine_ptr();
    return g_enginePtrBound;
}

const wchar_t* fname_text(int32_t index) {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase || index < 0 || !gnames_ready()) return nullptr;
    const uint8_t* arrayBase = g_imageBase + kGNamesDataRva;
    if (!is_memory_valid(arrayBase, 8)) return nullptr;
    const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(arrayBase);
    int32_t count = *reinterpret_cast<const int32_t*>(arrayBase + 4);
    if (!data || index >= count) return nullptr;
    if (!is_memory_valid(data + index, sizeof(void*))) return nullptr;
    const uint8_t* entry = data[index];
    if (!entry || !is_memory_valid(entry, kFNameEntryTextOffset + 2)) return nullptr;
    // Entries self-identify: +0 is the entry's own index. A mismatch means
    // the layout assumption broke (or the slot is recycled garbage) - refuse.
    if (*reinterpret_cast<const int32_t*>(entry) != index) return nullptr;
    return reinterpret_cast<const wchar_t*>(entry + kFNameEntryTextOffset);
}

int32_t fname_count() {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase || !gnames_ready()) return 0;
    const uint8_t* arrayBase = g_imageBase + kGNamesDataRva;
    if (!is_memory_valid(arrayBase, 8)) return 0;
    return *reinterpret_cast<const int32_t*>(arrayBase + 4);
}

const wchar_t* object_class_name(const void* obj) {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase || !obj || !kUClassVtableRva) return nullptr;
    const uint8_t* o = static_cast<const uint8_t*>(obj);
    if (!is_memory_valid(o, kUObjectClassOffset + sizeof(void*))) return nullptr;
    const uint8_t* cls =
        *reinterpret_cast<const uint8_t* const*>(o + kUObjectClassOffset);
    if (!cls || !is_memory_valid(cls, kUObjectNameIndexOffset + sizeof(int32_t)))
        return nullptr;
    // The +0x30 target must BE a UClass - vtable-gated so a stranger layout
    // (or a freed object) can never produce a bogus profile key.
    if (*reinterpret_cast<const void* const*>(cls) != g_imageBase + kUClassVtableRva)
        return nullptr;
    return fname_text(
        *reinterpret_cast<const int32_t*>(cls + kUObjectNameIndexOffset));
}

} // namespace bvr::b1r::patterns
