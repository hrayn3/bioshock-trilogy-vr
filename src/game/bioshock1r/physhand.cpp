#include "game/bioshock1r/physhand.h"

#include "core/hooks/heap_scan.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hands.h"
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
constexpr uint64_t kSettleMs = 2000; // the equip animation moves the reference

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
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_forceOff{false};
bool g_nearPending = false;
bool g_nearLooseOnly = false;
bool g_parked = false; // test control: the body stays live but far below the level

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

// `vrphys near`: Havok's own body lists, so props are found whatever actor class
// owns them. hkpSimulationIsland carries no reflection data, so its entity array
// is found by shape: a pointer and count whose first element is a rigid body.
struct Tracked {
    void* body;
    float pos[3];
};
constexpr int kMaxTracked = 32;
constexpr float kNearHk = 16.0f; // 8 m
constexpr uint32_t kWorldActiveIslandsOffset = 0x28;
constexpr uint32_t kWorldInactiveIslandsOffset = 0x34;
constexpr uint32_t kBodyVtableRva = 0xBFDD64;
constexpr uint32_t kBodyFilterOffset = 0x2C;
constexpr uint32_t kBodyMotionTypeOffset = 0xE8;
constexpr uint32_t kLayerProjectile = 11;
uint64_t g_watchUntilMs = 0;
uint64_t g_lastWatchMs = 0;
Tracked g_tracked[kMaxTracked];
int g_trackedCount = 0;
uint64_t g_lastTrackMs = 0;

struct Sweep {
    int index = -1;
    uint64_t startMs = 0;
} g_sweep;
constexpr uint64_t kSweepMs = 1200;
constexpr float kSweepHalfHk = 1.0f; // 50 cm either side

bool read_pos(void* body, float pos[3]) {
    __try {
        memcpy(pos, static_cast<uint8_t*>(body) + patterns::kHkBodyPosOffset, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int walk_island(void* island, const float hand[3]) {
    int added = 0;
    __try {
        const uint8_t* o = static_cast<const uint8_t*>(island);
        for (uint32_t off = 0x08; off < 0x80; off += 4) {
            void** data = *reinterpret_cast<void** const*>(o + off);
            int count = *reinterpret_cast<const int*>(o + off + 4);
            if (!data || count <= 0 || count > 4096 ||
                reinterpret_cast<uintptr_t>(data) < 0x10000)
                continue;
            if (!vtable_is(data[0], kBodyVtableRva)) continue;
            for (int i = 0; i < count && g_trackedCount < kMaxTracked; ++i) {
                void* b = data[i];
                if (!vtable_is(b, kBodyVtableRva) || b == g_body) continue;
                float p[3];
                if (!read_pos(b, p)) continue;
                float d[3] = {p[0] - hand[0], p[1] - hand[1], p[2] - hand[2]};
                float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                uint32_t filter = *reinterpret_cast<const uint32_t*>(
                    static_cast<uint8_t*>(b) + kBodyFilterOffset);
                uint8_t motion =
                    *(static_cast<uint8_t*>(b) + kBodyMotionTypeOffset);
                // Loose props anywhere on the map, everything else only nearby.
                bool loose = motion != 4 && motion != 5;
                if (g_watchUntilMs) {
                    // `vrphys watch`: projectiles, to prove they clear the hand.
                    if ((filter & 31) == kLayerProjectile)
                        BVR_LOG("[phys] projectile %p at %.2f m from the hand", b, dist * 0.5f);
                    continue;
                }
                if (g_nearLooseOnly ? !loose : dist > kNearHk) continue;
                g_tracked[g_trackedCount].body = b;
                memcpy(g_tracked[g_trackedCount].pos, p, 12);
                BVR_LOG("[phys] near #%d body %p layer=%u group=%u motion=%u at %.2f m "
                        "(hk %.2f %.2f %.2f)",
                        g_trackedCount, b, filter & 31, filter >> 16, motion, dist * 0.5f, p[0],
                        p[1], p[2]);
                ++g_trackedCount;
                ++added;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return added;
}

int walk_islands(uint32_t arrayOffset, const float hand[3]) {
    int added = 0;
    __try {
        const uint8_t* w = static_cast<const uint8_t*>(g_world);
        void** islands = *reinterpret_cast<void** const*>(w + arrayOffset);
        int n = *reinterpret_cast<const int*>(w + arrayOffset + 4);
        for (int i = 0; i < n && i < 512; ++i) added += walk_island(islands[i], hand);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return added;
}

void watch_tick(const float handHk[3]) {
    uint64_t now = GetTickCount64();
    if (now >= g_watchUntilMs) {
        g_watchUntilMs = 0;
        BVR_LOG("[phys] watch ended");
        return;
    }
    if (now - g_lastWatchMs < 100) return;
    g_lastWatchMs = now;
    walk_islands(kWorldActiveIslandsOffset, handHk);
}

void near_scan(const float handHk[3]) {
    g_trackedCount = 0;
    int active = walk_islands(kWorldActiveIslandsOffset, handHk);
    int asleep = walk_islands(kWorldInactiveIslandsOffset, handHk);
    BVR_LOG("[phys] near: %d awake, %d asleep%s", active, asleep,
            g_nearLooseOnly ? " (loose props, whole map)" : " within 8 m");
}

void track_tick() {
    uint64_t now = GetTickCount64();
    if (!g_trackedCount || now - g_lastTrackMs < 250) return;
    g_lastTrackMs = now;
    for (int i = 0; i < g_trackedCount; ++i) {
        float p[3];
        if (!read_pos(g_tracked[i].body, p)) continue;
        float d[3] = {p[0] - g_tracked[i].pos[0], p[1] - g_tracked[i].pos[1],
                      p[2] - g_tracked[i].pos[2]};
        float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (moved < 0.02f) continue;
        BVR_LOG("[phys] MOVED #%d by %.1f cm", i, moved * 50.0f);
        memcpy(g_tracked[i].pos, p, 12);
    }
}

// Debug override: drive the body straight through a tracked prop.
bool sweep_target(float pos[4]) {
    if (g_sweep.index < 0 || g_sweep.index >= g_trackedCount) return false;
    uint64_t t = GetTickCount64() - g_sweep.startMs;
    if (t >= kSweepMs) {
        BVR_LOG("[phys] sweep #%d done", g_sweep.index);
        g_sweep.index = -1;
        return false;
    }
    float p[3];
    if (!read_pos(g_tracked[g_sweep.index].body, p)) return false;
    float f = static_cast<float>(t) / kSweepMs * 2.0f - 1.0f;
    pos[0] = p[0] + f * kSweepHalfHk;
    pos[1] = p[1];
    pos[2] = p[2];
    return true;
}

// ---- minor telekinesis ------------------------------------------------------
constexpr float kTkReachHk = 3.0f;  // 1.5 m
float g_tkReachHk = kTkReachHk;     // `vrphys tk reach <m>`, a test lever
uint64_t g_tkLogMs = 0;
constexpr float kTkHoldHk = 0.7f;   // 35 cm in front of the hand
constexpr float kTkGain = 14.0f;    // per second, of the gap to the hold point
constexpr float kTkMaxSpeedHk = 30.0f;
constexpr float kTkThrowScale = 1.5f;
constexpr float kTkMaxKg = 80.0f;
constexpr float kTkSpinKeep = 0.9f;
constexpr uint64_t kTkStepMs = 8;
using VecFn = void(__fastcall*)(void* motion, void*, const float* v);
using MassFn = float(__fastcall*)(void* motion, void*);

void* g_pawn = nullptr;
void* g_tkBody = nullptr;
bool g_tkGripWas = false;
uint64_t g_tkLastMs = 0;
float g_tkHandLast[3] = {};
float g_tkHandVel[3] = {};
bool g_tkHaveHand = false;
std::atomic<bool> g_tkEnabled{true};
uint32_t g_tkGrabs = 0;

void* body_actor(void* body) {
    __try {
        uint8_t* b = static_cast<uint8_t*>(body);
        uint8_t* props = *reinterpret_cast<uint8_t**>(b + patterns::kHkBodyPropsOffset);
        int count = *reinterpret_cast<int*>(b + patterns::kHkBodyPropsOffset + 4);
        for (int i = 0; props && i < count && i < 16; ++i) {
            uint8_t* e = props + i * 16;
            if (*reinterpret_cast<uint32_t*>(e) == 0) return *reinterpret_cast<void**>(e + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

enum TkReject { kTkOk, kTkSelf, kTkFixed, kTkNoWorld, kTkNoActor, kTkPlayer, kTkRagdoll, kTkHeavy, kTkCount };
uint32_t g_tkRejects[kTkCount] = {};

TkReject grab_verdict(void* body) {
    if (!vtable_is(body, kBodyVtableRva) || body == g_body) return kTkSelf;
    uint8_t* b = static_cast<uint8_t*>(body);
    __try {
        uint8_t motion = *(b + kBodyMotionTypeOffset);
        if (motion == 4 || motion == 5) return kTkFixed;
        if (!*reinterpret_cast<void**>(b + patterns::kHkBodyWorldOffset)) return kTkNoWorld;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kTkNoWorld;
    }
    void* actor = body_actor(body);
    if (!actor) return kTkNoActor;
    if (actor == g_pawn) return kTkPlayer;
    void* rag = nullptr;
    void* live = nullptr;
    if (read_ptr(static_cast<uint8_t*>(actor) + patterns::kActorRagdollOffset, &rag) &&
        read_ptr(static_cast<uint8_t*>(rag) + 0x40, &live))
        return kTkRagdoll;
    __try {
        float kg = fn<MassFn>(patterns::kHkGetMassRva)(b + patterns::kHkBodyMotionOffset, nullptr);
        if (kg > kTkMaxKg) return kTkHeavy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kTkHeavy;
    }
    return kTkOk;
}

bool grabbable(void* body) {
    TkReject v = grab_verdict(body);
    ++g_tkRejects[v];
    return v == kTkOk;
}

void tk_scan_island(void* island, const float hand[3], void** best, float* bestDist) {
    __try {
        const uint8_t* o = static_cast<const uint8_t*>(island);
        for (uint32_t off = 0x08; off < 0x80; off += 4) {
            void** data = *reinterpret_cast<void** const*>(o + off);
            int count = *reinterpret_cast<const int*>(o + off + 4);
            if (!data || count <= 0 || count > 4096 ||
                reinterpret_cast<uintptr_t>(data) < 0x10000)
                continue;
            if (!vtable_is(data[0], kBodyVtableRva)) continue;
            for (int i = 0; i < count; ++i) {
                float p[3];
                if (!read_pos(data[i], p)) continue;
                float d[3] = {p[0] - hand[0], p[1] - hand[1], p[2] - hand[2]};
                float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist < *bestDist && grabbable(data[i])) {
                    *bestDist = dist;
                    *best = data[i];
                }
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* tk_find(const float hand[3]) {
    void* best = nullptr;
    float bestDist = g_tkReachHk;
    __try {
        const uint8_t* w = static_cast<const uint8_t*>(g_world);
        const uint32_t arrays[2] = {kWorldActiveIslandsOffset, kWorldInactiveIslandsOffset};
        for (uint32_t arr : arrays) {
            void** islands = *reinterpret_cast<void** const*>(w + arr);
            int n = *reinterpret_cast<const int*>(w + arr + 4);
            for (int i = 0; i < n && i < 512; ++i) tk_scan_island(islands[i], hand, &best, &bestDist);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return best;
}

bool tk_drive(void* body, const float vel[4], const float spin[4]) {
    __try {
        fn<ThisFn>(patterns::kHkActivateRva)(body, nullptr);
        uint8_t* motion = static_cast<uint8_t*>(body) + patterns::kHkBodyMotionOffset;
        void** vt = *reinterpret_cast<void***>(motion);
        reinterpret_cast<VecFn>(vt[patterns::kHkMotionSetLinearVt / 4])(motion, nullptr, vel);
        reinterpret_cast<VecFn>(vt[patterns::kHkMotionSetAngularVt / 4])(motion, nullptr, spin);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void clamp_speed(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > kTkMaxSpeedHk)
        for (int i = 0; i < 3; ++i) v[i] *= kTkMaxSpeedHk / len;
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
    g_tkBody = nullptr;
    g_builtKey.clear();
    g_trackedCount = 0;
    g_sweep.index = -1;
    g_keyChangeMs = GetTickCount64(); // a load plays the equip animation
}

void telekinesis_tick(const float handUu[3], const float dir[3], bool grip) {
    if (!patterns::kHkActivateRva || !g_tkEnabled.load(std::memory_order_relaxed) || !g_world ||
        g_forceOff.load(std::memory_order_relaxed))
        return;
    uint64_t now = GetTickCount64();
    uint64_t stepMs = now - g_tkLastMs;
    if (stepMs < kTkStepMs) return;
    g_tkLastMs = now;
    uint32_t critical = 0;
    if (!read_u32(static_cast<uint8_t*>(g_world) + patterns::kHkWorldCriticalLockOffset, &critical) ||
        critical != 0)
        return;

    const float hand[3] = {handUu[0] * kHkPerUu, handUu[1] * kHkPerUu, handUu[2] * kHkPerUu};
    if (g_tkHaveHand && stepMs < 100) {
        float dt = static_cast<float>(stepMs) * 0.001f;
        for (int i = 0; i < 3; ++i)
            g_tkHandVel[i] += ((hand[i] - g_tkHandLast[i]) / dt - g_tkHandVel[i]) * 0.5f;
    }
    memcpy(g_tkHandLast, hand, sizeof g_tkHandLast);
    g_tkHaveHand = true;

    if (!g_tkBody) {
        if (grip && !g_tkGripWas) {
            g_tkBody = tk_find(hand);
            if (g_tkBody) ++g_tkGrabs;
            BVR_LOG("[tk] grip: %s (ok %u self %u fixed %u noworld %u noactor %u player %u ragdoll "
                    "%u heavy %u)",
                    g_tkBody ? "got one" : "nothing in reach", g_tkRejects[kTkOk],
                    g_tkRejects[kTkSelf], g_tkRejects[kTkFixed], g_tkRejects[kTkNoWorld],
                    g_tkRejects[kTkNoActor], g_tkRejects[kTkPlayer], g_tkRejects[kTkRagdoll],
                    g_tkRejects[kTkHeavy]);
            memset(g_tkRejects, 0, sizeof g_tkRejects);
        }
    } else if (!vtable_is(g_tkBody, kBodyVtableRva)) {
        g_tkBody = nullptr;
    } else if (!grip) {
        alignas(16) float vel[4] = {g_tkHandVel[0] * kTkThrowScale, g_tkHandVel[1] * kTkThrowScale,
                                    g_tkHandVel[2] * kTkThrowScale, 0.0f};
        clamp_speed(vel);
        alignas(16) float spin[4] = {};
        tk_drive(g_tkBody, vel, spin);
        BVR_LOG("[tk] released at %.1f m/s",
                sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) * 0.5f);
        g_tkBody = nullptr;
    } else {
        float p[3];
        if (!read_pos(g_tkBody, p)) {
            g_tkBody = nullptr;
        } else {
            alignas(16) float vel[4] = {};
            for (int i = 0; i < 3; ++i) vel[i] = (hand[i] + dir[i] * kTkHoldHk - p[i]) * kTkGain;
            clamp_speed(vel);
            alignas(16) float spin[4] = {};
            __try {
                memcpy(spin, static_cast<uint8_t*>(g_tkBody) + patterns::kHkBodyAngularOffset, 16);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            for (int i = 0; i < 3; ++i) spin[i] *= kTkSpinKeep;
            if (now - g_tkLogMs >= 1000) {
                g_tkLogMs = now;
                float gap[3] = {hand[0] - p[0], hand[1] - p[1], hand[2] - p[2]};
                BVR_LOG("[tk] holding, %.2f m from the hand",
                        sqrtf(gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2]) * 0.5f);
            }
            if (!tk_drive(g_tkBody, vel, spin)) {
                BVR_LOG("[tk] drive faulted, letting go");
                g_tkBody = nullptr;
            }
        }
    }
    g_tkGripWas = grip;
}

void on_frame(const GamePose& gp, int hand) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_forceOff.load(std::memory_order_relaxed))
        return;
    if (!g_base || !patterns::kHkQueueKeyframeRva) return;

    // The rig has no owner field we trust; the holdable's is the fire path's own.
    void* hold = nullptr;
    void* pawn = nullptr;
    if (!hands::current_holdable(&hold) || !hold ||
        !read_ptr(static_cast<uint8_t*>(hold) + patterns::kWeaponOwnerOffset, &pawn) ||
        !vtable_is(pawn, patterns::kShockPlayerVtableRva))
        return;
    g_pawn = pawn;
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

    track_tick();

    alignas(16) float pos[4] = {gp.loc.x * kHkPerUu, gp.loc.y * kHkPerUu, gp.loc.z * kHkPerUu, 0};
    alignas(16) float quat[4];
    ue_rot_to_quat(gp.rot, quat);
    if (g_nearPending) {
        g_nearPending = false;
        near_scan(pos);
    }
    if (g_watchUntilMs) watch_tick(pos);
    bool sweeping = sweep_target(pos);
    if (g_parked) pos[2] -= 200.0f;
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
    } else if (len > kMaxStepHk && !sweeping) {
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
    } else if (strncmp(args, "tk", 2) == 0) {
        float reachM = 0.0f;
        if (const char* r = strstr(args, "reach")) sscanf_s(r + 5, "%f", &reachM);
        if (reachM > 0.0f) g_tkReachHk = reachM * kHkPerMetre;
        g_tkEnabled.store(strstr(args, "off") == nullptr, std::memory_order_relaxed);
        BVR_LOG("[tk] %s, %u grabs, holding %p",
                g_tkEnabled.load(std::memory_order_relaxed) ? "on" : "off", g_tkGrabs, g_tkBody);
    } else if (strncmp(args, "near", 4) == 0) {
        g_nearPending = true;
        g_nearLooseOnly = strstr(args, "loose") != nullptr;
    } else if (strncmp(args, "park", 4) == 0) {
        g_parked = strstr(args, "on") != nullptr;
        BVR_LOG("[phys] parked %s", g_parked ? "on" : "off");
    } else if (strncmp(args, "watch", 5) == 0) {
        g_watchUntilMs = GetTickCount64() + 6000;
        BVR_LOG("[phys] watching projectiles for 6 s");
    } else if (strncmp(args, "sweep", 5) == 0) {
        int index = 0;
        sscanf_s(args + 5, "%d", &index);
        g_sweep.index = index;
        g_sweep.startMs = GetTickCount64();
        BVR_LOG("[phys] sweeping through #%d", index);
    } else {
        log_status();
    }
}

} // namespace bvr::b1r::physhand
