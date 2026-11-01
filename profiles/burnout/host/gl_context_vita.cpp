// vitaGL context for the PS Vita GE renderer: 960x544, shaders compiled at
// run time by vitaShaRK (libshacccg.suprx must be installed).
#include "gl_api.hpp"

#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cstdio>

namespace burnout::gl {

bool create_context(std::string &error) {
    // The guest heap is reserved at startup, so size each vitaGL pool from
    // what is actually left instead of letting vglInitExtended guess.
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    constexpr int kMiB = 1024 * 1024;
    const int ram = std::clamp(info.size_user - 8 * kMiB, 4 * kMiB, 32 * kMiB);
    const int cdram = std::max(info.size_cdram - 4 * kMiB, 16 * kMiB);
    const int phycont = std::clamp(info.size_phycont - 2 * kMiB, 0, 16 * kMiB);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "free user %d KiB, cdram %d KiB, phycont %d KiB; pools ram %d cdram %d phycont %d KiB",
                  info.size_user / 1024, info.size_cdram / 1024, info.size_phycont / 1024, ram / 1024, cdram / 1024,
                  phycont / 1024);
    if (!vglInitWithCustomSizes(0, 960, 544, ram, cdram, phycont, 0, SCE_GXM_MULTISAMPLE_NONE)) {
        error = std::string("vglInitWithCustomSizes failed (") + detail + ")";
        return false;
    }
    std::fprintf(stderr, "[gl] %s\n", detail);
    return true;
}

void swap_buffers() { vglSwapBuffers(GL_FALSE); }

} // namespace burnout::gl
