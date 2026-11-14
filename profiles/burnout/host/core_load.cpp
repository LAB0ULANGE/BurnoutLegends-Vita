#include "core_load.hpp"

#if defined(_WIN32) && !defined(__vita__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace burnout::core_load {

#if defined(__vita__)
namespace {

struct Entry {
    SceUID id{-1};
    char name[24]{};
    std::uint64_t last_clocks{};
};

constexpr int kMax = 24;
std::array<Entry, kMax> g_entries;
std::atomic<int> g_count{0};
std::mutex g_report_mutex;
std::uint64_t g_last_wall{};

std::uint64_t run_clocks(SceUID id, int &core) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(id, &info) != 0) {
        core = -1;
        return 0;
    }
    // Pinned to one core: that core. Free to move: the last one it ran on.
    const unsigned mask = static_cast<unsigned>(info.currentCpuAffinityMask) >> 16;
    if (mask == 1u) core = 0;
    else if (mask == 2u) core = 1;
    else if (mask == 4u) core = 2;
    else core = info.lastExecutedCpuId >= 0 && info.lastExecutedCpuId <= 2 ? info.lastExecutedCpuId : 3;
    return info.runClocks;
}

} // namespace

void register_current_thread(const char *name) {
    const int slot = g_count.fetch_add(1);
    if (slot >= kMax) return;
    Entry &entry = g_entries[static_cast<std::size_t>(slot)];
    std::snprintf(entry.name, sizeof(entry.name), "%s", name);
    int core = 0;
    entry.last_clocks = run_clocks(sceKernelGetThreadId(), core);
    entry.id = sceKernelGetThreadId();
}

std::string report() {
    std::lock_guard lock(g_report_mutex);
    const std::uint64_t wall = sceKernelGetProcessTimeWide();
    const std::uint64_t span = wall - g_last_wall;
    const bool first = g_last_wall == 0u;
    g_last_wall = wall;
    const int count = std::min(g_count.load(), kMax);
    double per_core[4]{};
    std::string threads;
    for (int i = 0; i < count; ++i) {
        Entry &entry = g_entries[static_cast<std::size_t>(i)];
        if (entry.id < 0) continue;
        int core = 0;
        const std::uint64_t clocks = run_clocks(entry.id, core);
        if (core < 0) continue;  // gone
        const std::uint64_t used = clocks - entry.last_clocks;
        entry.last_clocks = clocks;
        if (first || span == 0u) continue;
        const double percent = 100.0 * static_cast<double>(used) / static_cast<double>(span);
        per_core[core] += percent;
        char part[64];
        std::snprintf(part, sizeof(part), " %s(c%d) %.1f%%", entry.name, core, percent);
        threads += part;
    }
    if (first || span == 0u) return {};
    char head[160];
    std::snprintf(head, sizeof(head), "core load over %.1f s: core 0 %.0f%%, core 1 %.0f%%, core 2 %.0f%%%s; threads:",
                  static_cast<double>(span) / 1e6, per_core[0], per_core[1], per_core[2],
                  per_core[3] > 0.0 ? " (unpinned elsewhere)" : "");
    return head + threads;
}

#else

#if defined(_WIN32)
// PC: PSPRECOMP_PROFILE_THREAD=<name> makes the sampling profiler (headless
// --profile) watch that thread (decode, gl, ge_front...) instead of the
// emulation thread.
void *g_profile_target{};
void register_current_thread(const char *name) {
    const char *wanted = std::getenv("PSPRECOMP_PROFILE_THREAD");
    if (wanted == nullptr || std::strcmp(wanted, name) != 0) return;
    HANDLE handle{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0u);
    g_profile_target = handle;
}
void *profile_target() { return g_profile_target; }
#else
void register_current_thread(const char *) {}
void *profile_target() { return nullptr; }
#endif
std::string report() { return {}; }

#endif

} // namespace burnout::core_load
