#include "core/debug/thread_prof.h"

#include "core/util/log.h"

#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace bvr::debug {
namespace {

constexpr size_t kMaxSamples = 60000;
std::atomic<bool> g_running{false};

struct Job {
    uint32_t tid;
    uint32_t ms;
};

// Nothing here may allocate between Suspend and Resume: the target can be
// holding the heap lock, and this thread would then deadlock against it.
DWORD WINAPI sample_thread(void* arg) {
    Job job = *static_cast<Job*>(arg);
    delete static_cast<Job*>(arg);

    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, job.tid);
    if (!t) {
        BVR_LOG("[prof] cannot open thread %u (err %lu)", job.tid, GetLastError());
        g_running = false;
        return 0;
    }
    std::vector<uint32_t> eips(kMaxSamples);
    // A wait and a busy loop both land in ntdll; the top of the stack tells them
    // apart by naming who called in.
    constexpr size_t kStackDwords = 48;
    std::vector<uint32_t> stacks(kMaxSamples * kStackDwords);
    size_t n = 0;
    timeBeginPeriod(1);
    ULONGLONG end = GetTickCount64() + job.ms;
    while (GetTickCount64() < end && n < kMaxSamples) {
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            bool ok = GetThreadContext(t, &c) != 0;
            SIZE_T got = 0;
            if (ok)
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Esp),
                                  &stacks[n * kStackDwords], kStackDwords * 4, &got);
            ResumeThread(t);
            if (ok) eips[n++] = c.Eip;
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    CloseHandle(t);

    HMODULE mods[512];
    DWORD got = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &got);
    struct Mod {
        uintptr_t base, size;
        std::string name;
        size_t hits;
    };
    std::vector<Mod> list;
    for (DWORD i = 0; i < got / sizeof(HMODULE); ++i) {
        MODULEINFO mi{};
        char path[MAX_PATH] = {};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        list.push_back({reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), mi.SizeOfImage,
                        slash ? slash + 1 : path, 0});
    }
    uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::map<uint32_t, size_t> bins;
    size_t unknown = 0;
    for (size_t i = 0; i < n; ++i) {
        uintptr_t a = eips[i];
        bool found = false;
        for (Mod& m : list) {
            if (a < m.base || a >= m.base + m.size) continue;
            ++m.hits;
            found = true;
            if (m.base == exeBase) ++bins[static_cast<uint32_t>(a - m.base) & ~0x3Fu];
            break;
        }
        if (!found) ++unknown;
    }
    std::sort(list.begin(), list.end(), [](const Mod& a, const Mod& b) { return a.hits > b.hits; });
    BVR_LOG("[prof] %zu samples of thread %u over %u ms", n, job.tid, job.ms);
    for (size_t i = 0; i < list.size() && i < 12 && list[i].hits; ++i)
        BVR_LOG("[prof] %5.1f%% %s", 100.0 * list[i].hits / (n ? n : 1), list[i].name.c_str());
    BVR_LOG("[prof] %5.1f%% outside any module", 100.0 * unknown / (n ? n : 1));

    // System code by symbol, with the nearest caller inside the game or the mod.
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    const uintptr_t selfBase = [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&thread_prof_start), &m);
        return reinterpret_cast<uintptr_t>(m);
    }();
    auto mod_of = [&](uintptr_t a) -> const Mod* {
        for (const Mod& m : list)
            if (a >= m.base && a < m.base + m.size) return &m;
        return nullptr;
    };
    std::map<std::string, size_t> sys;
    for (size_t i = 0; i < n; ++i) {
        const Mod* m = mod_of(eips[i]);
        if (!m || m->base == exeBase || m->base == selfBase) continue;
        char buf[sizeof(SYMBOL_INFO) + 128] = {};
        SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 127;
        DWORD64 disp = 0;
        std::string name = SymFromAddr(proc, eips[i], &disp, si) ? si->Name : "?";
        std::string caller = "-";
        for (size_t k = 0; k < kStackDwords; ++k) {
            uintptr_t v = stacks[i * kStackDwords + k];
            const Mod* c = mod_of(v);
            if (!c || (c->base != exeBase && c->base != selfBase)) continue;
            char hex[32];
            sprintf_s(hex, "%s+%X", c->base == exeBase ? "exe" : "mod",
                      static_cast<unsigned>(v - c->base));
            caller = hex;
            break;
        }
        ++sys[m->name + "!" + name + " < " + caller];
    }
    SymCleanup(proc);
    std::vector<std::pair<size_t, std::string>> sysTop;
    for (auto& kv : sys) sysTop.push_back({kv.second, kv.first});
    std::sort(sysTop.rbegin(), sysTop.rend());
    for (size_t i = 0; i < sysTop.size() && i < 30; ++i)
        BVR_LOG("[prof] sys %5.2f%% %s", 100.0 * sysTop[i].first / (n ? n : 1),
                sysTop[i].second.c_str());
    std::vector<std::pair<size_t, uint32_t>> top;
    for (auto& kv : bins) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 60; ++i)
        BVR_LOG("[prof] bin %08X %5.2f%%", top[i].second, 100.0 * top[i].first / (n ? n : 1));
    g_running = false;
    return 0;
}

} // namespace

void thread_prof_start(uint32_t tid, uint32_t ms) {
    if (g_running.exchange(true)) {
        BVR_LOG("[prof] already sampling");
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, sample_thread, new Job{tid, ms}, 0, nullptr);
    if (h) {
        CloseHandle(h);
        BVR_LOG("[prof] sampling thread %u for %u ms", tid, ms);
    } else {
        g_running = false;
    }
}

} // namespace bvr::debug
