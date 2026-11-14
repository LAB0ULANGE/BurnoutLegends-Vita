#pragma once

// The Vita's fourth core (core 3) is the system's: user threads may not run
// there, unless a plugin such as CapUnlocker opens it. Burnout's busiest core
// in 60 fps races is core 2 (GL thread, GE front end, sound); the sound
// threads go to core 3 when the system lets them (2026-11-14, from the
// Saints Row Vita port's advice), and stay where they were otherwise: players
// without the plugin see no change. ux0:data/burnout/no_system_core.txt
// keeps them off it.

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>

#include <cstdio>

namespace burnout {

constexpr int kSystemCoreMask = 0x80000;  // SCE_KERNEL_CPU_MASK_USER_0 << 3

// Moves the calling thread to core 3 if it then really runs there; else puts
// it on `fallback_mask` (its usual core). True on core 3.
inline bool move_to_system_core(int fallback_mask) {
    static const bool allowed = [] {
        std::FILE *f = std::fopen("ux0:data/burnout/no_system_core.txt", "rb");
        if (f != nullptr) std::fclose(f);
        return f == nullptr;
    }();
    const SceUID me = sceKernelGetThreadId();
    if (allowed && sceKernelChangeThreadCpuAffinityMask(me, kSystemCoreMask) >= 0) {
        sceKernelDelayThread(1000);  // let the scheduler move it
        if (sceKernelGetCpuId() == 3) return true;
    }
    sceKernelChangeThreadCpuAffinityMask(me, fallback_mask);
    return false;
}

} // namespace burnout
#endif
