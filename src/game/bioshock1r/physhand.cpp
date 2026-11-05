#include "game/bioshock1r/physhand.h"

#include "core/hooks/heap_scan.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::b1r::physhand {
namespace {

constexpr float kHkPerUu = 0.02f;    // the game's own world scale
constexpr float kHkPerMetre = 2.0f;  // 100 UU per metre
constexpr float kMaxStepHk = 0.5f;   // 25 cm a frame, about 11 m/s at 45 fps
constexpr float kWarpHk = 3.0f;      // 1.5 m: a load or recenter, not a hand
constexpr uint32_t kFilterInfo = (1u << 16) | 5u; // group 1, layer 5
constexpr uint64_t kSettleMs = 800;  // the equip animation moves the reference

// Thiscall targets, called as fastcall: ecx is `self`, edx is unused, the
// callee pops the rest either way.
using AllocFn = void*(__fastcall*)(void* heap, void*, int size);
using ThisFn = void(__fastcall*)(void* self, void*);
using CapsuleFn = void(__fastcall*)(void* self, void*, const float* a, const float* b, float r);
using BodyFn = void(__fastcall*)(void* self, void*, const void* cinfo);
using AddPropertyFn = void(__fastcall*)(void* self, void*, uint32_t key, void* lo, uint32_t hi);
using AddEntityFn = void*(__fastcall*)(void* world, void*, void* entity, int activation);
using RemoveEntityFn = void*(__fastcall*)(void* world, void*, uint8_t* result, void* entity);
using WarpFn = void(__fastcall*)(void* body, void*, const float* pos, const float* quat);
using QueueFn = void(__fastcall*)(void* havok, void*, const float* pos, const float* quat,
                                  void* body);

struct Collider {
    const char* key;
    float fromM, toM, radiusM; // along the weapon axis
};
// Lengths are from the viewmodels. The headset decides the final numbers.
const Collider kColliders[] = {
    {"Wrench", 0.00f, 0.55f, 0.045f},
    {"Pistol", -0.05f, 0.24f, 0.04f},
    {"MachineGun", -0.10f, 0.55f, 0.05f},
    {"Shotgun", -0.10f, 0.60f, 0.05f},
    {"Crossbow", 0.00f, 0.60f, 0.07f},
    {"GrenadeLauncher", 0.00f, 0.55f, 0.07f},
    {"ChemicalThrower", 0.00f, 0.55f, 0.07f},
    {"ResearchCamera", 0.00f, 0.18f, 0.08f},
};
const Collider kDefaultCollider = {"default", 0.00f, 0.30f, 0.05f};
const Collider kHandCollider = {"hand", 0.00f, 0.12f, 0.05f};
const float kFwd[3] = {1.0f, 0.0f, 0.0f};

const uint8_t* g_base = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_forceOff{false};

void* g_havok = nullptr; // the level's Havok wrapper, re-read each frame
void* g_world = nullptr;
void* g_body = nullptr;
int16_t g_baseRefs = 0;  // body refcount with nothing queued
std::string g_builtKey;
std::string g_wantKey;
uint64_t g_keyChangeMs = 0;
uint32_t g_builds = 0, g_queued = 0, g_skipped = 0, g_warps = 0, g_clamps = 0, g_faults = 0;
float g_lastTarget[3] = {};
float g_lastBody[3] = {};

template <typename T>
T fn(uint32_t rva) {
    return reinterpret_cast<T>(const_cast<uint8_t*>(g_base) + rva);
}

bool read_u32(const void* p, uint32_t* out) {
    __try {
        *out = *static_cast<const uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* p, void** out) {
    uint32_t v = 0;
    if (!read_u32(p, &v)) return false;
    *out = reinterpret_cast<void*>(v);
    return v != 0;
}

bool vtable_is(void* obj, uint32_t rva) {
    void* vt = nullptr;
    return obj && read_ptr(obj, &vt) && vt == g_base + rva;
}

void* hk_alloc(int size) {
    void* router = TlsGetValue(*reinterpret_cast<const DWORD*>(g_base + patterns::kHkTlsSlotRva));
    if (!router) return nullptr;
    void* heap = *reinterpret_cast<void**>(static_cast<uint8_t*>(router) + 0x2C);
    void** vt = *reinterpret_cast<void***>(heap);
    return reinterpret_cast<AllocFn>(vt[1])(heap, nullptr, size);
}

// Same order as the game's own body creation: shape, cinfo, body, drop the shape
// reference (the body holds its own), actor property, add to world.
void* build_body_raw(void* world, void* pawn, const float a[4], const float b[4], float radius,
                     const float pos[4], const float quat[4]) {
    void* capsule = hk_alloc(patterns::kHkCapsuleBytes);
    if (!capsule) return nullptr;
    fn<CapsuleFn>(patterns::kHkCapsuleCtorRva)(capsule, nullptr, a, b, radius);

    alignas(16) uint8_t cinfo[0xD0];
    fn<ThisFn>(patterns::kHkCinfoCtorRva)(cinfo, nullptr);
    *reinterpret_cast<uint32_t*>(cinfo + 0x00) = kFilterInfo;
    *reinterpret_cast<void**>(cinfo + 0x04) = capsule;
    memcpy(cinfo + 0x10, pos, 16);
    memcpy(cinfo + 0x20, quat, 16);
    *reinterpret_cast<float*>(cinfo + 0xA8) = 0.0f;   // restitution: a hand does not bounce
    *reinterpret_cast<float*>(cinfo + 0xAC) = 50.0f;  // the engine's own velocity caps
    *reinterpret_cast<float*>(cinfo + 0xB0) = 7.854f;
    cinfo[0xB4] = 4; // keyframed

    void* body = hk_alloc(patterns::kHkBodyBytes);
    if (!body) return nullptr;
    fn<BodyFn>(patterns::kHkBodyCtorRva)(body, nullptr, cinfo);
    fn<ThisFn>(patterns::kHkRemoveRefRva)(capsule, nullptr);
    fn<AddPropertyFn>(patterns::kHkAddPropertyRva)(body, nullptr, 0, pawn, 0);
    fn<AddEntityFn>(patterns::kHkAddEntityRva)(world, nullptr, body, 1);
    return body;
}

void* build_body(void* world, void* pawn, const float a[4], const float b[4], float radius,
                 const float pos[4], const float quat[4]) {
    __try {
        return build_body_raw(world, pawn, a, b, radius, pos, quat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool remove_body(void* world, void* body) {
    __try {
        uint8_t result = 0;
        fn<RemoveEntityFn>(patterns::kHkRemoveEntityRva)(world, nullptr, &result, body);
        fn<ThisFn>(patterns::kHkRemoveRefRva)(body, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool move_body(void* havok, void* body, const float pos[4], const float quat[4], bool warp) {
    __try {
        if (warp)
            fn<WarpFn>(patterns::kHkWarpRva)(body, nullptr, pos, quat);
        else
            fn<QueueFn>(patterns::kHkQueueKeyframeRva)(havok, nullptr, pos, quat, body);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_body_state(void* body, float pos[3], int16_t* refs) {
    __try {
        memcpy(pos, static_cast<uint8_t*>(body) + patterns::kHkBodyPosOffset, 12);
        *refs = *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(body) +
                                            patterns::kHkRefCountOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void note_fault(const char* what) {
    ++g_faults;
    g_body = nullptr;
    g_forceOff.store(true, std::memory_order_relaxed);
    BVR_LOG("[phys] %s faulted - physics hand off until `vrphys on`", what);
}

const Collider& collider_for(const std::string& key) {
    if (key == kHandCollider.key) return kHandCollider;
    for (const Collider& c : kColliders)
        if (key == c.key) return c;
    return kDefaultCollider;
}

// `vrphys near`: actors close to the hand, then any movement of them, so "the
// hand moved a prop" is a log line rather than a screenshot comparison.
struct Tracked {
    void* obj;
    float loc[3];
};
constexpr int kMaxTracked = 24;
Tracked g_tracked[kMaxTracked];
int g_trackedCount = 0;
bool g_nearScan = false;
patterns::ObjectScan g_scan;
uint64_t g_lastTrackMs = 0;

bool accept_near(void* obj, void* user, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    const float* hand = static_cast<const float*>(user);
    float loc[3];
    memcpy(loc, static_cast<uint8_t*>(obj) + patterns::kActorLocOffset, sizeof loc);
    float d[3] = {loc[0] - hand[0], loc[1] - hand[1], loc[2] - hand[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    probe[0] = static_cast<int32_t>(dist);
    if (dist < 1500.0f && g_trackedCount < kMaxTracked) {
        g_tracked[g_trackedCount].obj = obj;
        memcpy(g_tracked[g_trackedCount].loc, loc, sizeof loc);
        ++g_trackedCount;
        const wchar_t* name = patterns::object_class_name(obj);
        BVR_LOG("[phys] near %p %ls at (%.0f %.0f %.0f), %.0f UU from the hand", obj,
                name ? name : L"?", loc[0], loc[1], loc[2], dist);
    }
    return false; // keep sweeping
}

bool read_loc(void* obj, float loc[3]) {
    __try {
        memcpy(loc, static_cast<uint8_t*>(obj) + patterns::kActorLocOffset, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void track_tick(const float hand[3]) {
    if (g_nearScan) {
        patterns::ScanResult r{};
        if (patterns::run_object_scan(g_scan, patterns::kHavokActorVtableRva,
                                      patterns::kActorLocOffset + 12, &accept_near,
                                      const_cast<float*>(hand), r)) {
            g_nearScan = false;
            BVR_LOG("[phys] near scan done: %d tracked", g_trackedCount);
        }
        return;
    }
    uint64_t now = GetTickCount64();
    if (!g_trackedCount || now - g_lastTrackMs < 500) return;
    g_lastTrackMs = now;
    for (int i = 0; i < g_trackedCount; ++i) {
        float loc[3];
        if (!read_loc(g_tracked[i].obj, loc)) continue;
        float d[3] = {loc[0] - g_tracked[i].loc[0], loc[1] - g_tracked[i].loc[1],
                      loc[2] - g_tracked[i].loc[2]};
        float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (moved < 0.5f) continue;
        BVR_LOG("[phys] MOVED %p by %.1f UU to (%.0f %.0f %.0f)", g_tracked[i].obj, moved, loc[0],
                loc[1], loc[2]);
        memcpy(g_tracked[i].loc, loc, sizeof loc);
    }
}

void log_status() {
    int16_t refs = 0;
    float pos[3] = {};
    if (g_body) read_body_state(g_body, pos, &refs);
    BVR_LOG("[phys] %s body=%p world=%p collider='%s' refs=%d base=%d builds=%u queued=%u "
            "skipped=%u warps=%u clamps=%u faults=%u",
            g_enabled.load(std::memory_order_relaxed)
                ? (g_forceOff.load(std::memory_order_relaxed) ? "faulted" : "ON")
                : "off",
            g_body, g_world, g_builtKey.c_str(), refs, g_baseRefs, g_builds, g_queued, g_skipped,
            g_warps, g_clamps, g_faults);
    BVR_LOG("[phys] target hk (%.3f %.3f %.3f) body hk (%.3f %.3f %.3f)", g_lastTarget[0],
            g_lastTarget[1], g_lastTarget[2], pos[0], pos[1], pos[2]);
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) { g_base = image.base; }

void on_world_change() {
    // The old world owns teardown, and the Havok heap may be gone with it, so no
    // Havok call. One small body stays referenced for the life of the process.
    g_body = nullptr;
    g_world = nullptr;
    g_havok = nullptr;
    g_builtKey.clear();
    g_trackedCount = 0;
    g_nearScan = false;
}

void on_frame(void* handsActor, const GamePose& gp, int hand) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_forceOff.load(std::memory_order_relaxed))
        return;
    if (!g_base || !patterns::kHkQueueKeyframeRva) return;

    void* pawn = nullptr;
    if (!read_ptr(static_cast<uint8_t*>(handsActor) + patterns::kWeaponOwnerOffset, &pawn) ||
        !vtable_is(pawn, patterns::kShockPlayerVtableRva))
        return;
    void* level = nullptr;
    void* havok = nullptr;
    void* world = nullptr;
    if (!read_ptr(static_cast<uint8_t*>(pawn) + patterns::kActorLevelOffset, &level) ||
        !read_ptr(static_cast<uint8_t*>(level) + patterns::kLevelHavokOffset, &havok) ||
        !read_ptr(static_cast<uint8_t*>(havok) + patterns::kHavokWorldOffset, &world))
        return;
    if (world != g_world) {
        if (g_world) BVR_LOG("[phys] world changed - body forgotten");
        on_world_change();
        g_world = world;
    }
    g_havok = havok;

    uint32_t critical = 0;
    if (!read_u32(static_cast<uint8_t*>(world) + patterns::kHkWorldCriticalLockOffset, &critical) ||
        critical != 0)
        return; // Havok would defer our call

    float cloc[3] = {gp.loc.x, gp.loc.y, gp.loc.z};
    track_tick(cloc);

    alignas(16) float pos[4] = {gp.loc.x * kHkPerUu, gp.loc.y * kHkPerUu, gp.loc.z * kHkPerUu, 0};
    alignas(16) float quat[4];
    ue_rot_to_quat(gp.rot, quat);
    memcpy(g_lastTarget, pos, sizeof g_lastTarget);

    std::string key = hand == 0 ? kHandCollider.key : aim::weapon_key();
    uint64_t now = GetTickCount64();
    if (key != g_wantKey) {
        g_wantKey = key;
        g_keyChangeMs = now;
    }
    if (g_body && key != g_builtKey && now - g_keyChangeMs >= kSettleMs) {
        void* old = g_body;
        g_body = nullptr;
        if (!remove_body(world, old)) {
            note_fault("remove");
            return;
        }
    }

    if (!g_body) {
        if (now - g_keyChangeMs < kSettleMs) return;
        const Collider& c = collider_for(key);
        float origin[3] = {0, 0, 0};
        float axis[3] = {kFwd[0], kFwd[1], kFwd[2]};
        if (hand != 0 && !bones::weapon_frame(origin, axis)) return;
        alignas(16) float a[4], b[4];
        for (int i = 0; i < 3; ++i) {
            a[i] = origin[i] * kHkPerUu + axis[i] * c.fromM * kHkPerMetre;
            b[i] = origin[i] * kHkPerUu + axis[i] * c.toM * kHkPerMetre;
        }
        a[3] = b[3] = 0.0f;
        void* body = build_body(world, pawn, a, b, c.radiusM * kHkPerMetre, pos, quat);
        if (!body) {
            note_fault("build");
            return;
        }
        float p[3];
        read_body_state(body, p, &g_baseRefs);
        g_body = body;
        g_builtKey = key;
        ++g_builds;
        BVR_LOG("[phys] body %p built for '%s': capsule %.2f..%.2f m r=%.3f, origin (%.1f %.1f "
                "%.1f) UU, axis (%.2f %.2f %.2f)",
                body, key.c_str(), c.fromM, c.toM, c.radiusM, origin[0], origin[1], origin[2],
                axis[0], axis[1], axis[2]);
        return;
    }

    float cur[3] = {};
    int16_t refs = 0;
    if (!read_body_state(g_body, cur, &refs)) {
        note_fault("read");
        return;
    }
    memcpy(g_lastBody, cur, sizeof g_lastBody);
    if (refs > g_baseRefs) {
        ++g_skipped; // no physics step since the last push (paused)
        return;
    }
    float d[3] = {pos[0] - cur[0], pos[1] - cur[1], pos[2] - cur[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    bool warp = len > kWarpHk;
    if (warp) {
        ++g_warps;
    } else if (len > kMaxStepHk) {
        ++g_clamps;
        float k = kMaxStepHk / len;
        for (int i = 0; i < 3; ++i) pos[i] = cur[i] + d[i] * k;
    }
    if (!move_body(g_havok, g_body, pos, quat, warp)) {
        note_fault("move");
        return;
    }
    if (!warp) ++g_queued;
}

void handle_command(const char* args) {
    if (strncmp(args, "on", 2) == 0) {
        g_forceOff.store(false, std::memory_order_relaxed);
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[phys] ON%s", patterns::kHkQueueKeyframeRva ? "" : " but this build has no Havok map");
    } else if (strncmp(args, "off", 3) == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        if (g_body && g_world) {
            if (!remove_body(g_world, g_body)) note_fault("remove");
            g_body = nullptr;
        }
        g_builtKey.clear();
        BVR_LOG("[phys] off");
    } else if (strncmp(args, "near", 4) == 0) {
        g_trackedCount = 0;
        g_nearScan = true;
        BVR_LOG("[phys] near scan started");
    } else {
        log_status();
    }
}

} // namespace bvr::b1r::physhand
