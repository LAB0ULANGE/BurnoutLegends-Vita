// vitaGL context for the PS Vita GE renderer: 960x544, shaders compiled at
// run time by vitaShaRK (libshacccg.suprx must be installed).
#include "gl_api.hpp"

#include "ge_gl.hpp"

#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace burnout::gl {

int g_param_buffer_mib = 0;  // GXM parameter buffer, 0: vitaGL default (16 MiB)
void set_param_buffer_mib(int mib) { g_param_buffer_mib = mib; }
int g_fbo_scenes = 8;  // GXM scenes per frame of each FBO render target
void set_fbo_scenes(int scenes) { g_fbo_scenes = scenes; }

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
    trace(detail);
    if (cached_memory()) {
        // CPU writes to the pools (vertex and index uploads) go through the cache.
        vglUseCachedMem(GL_TRUE);
        trace("vitaGL pools in cached memory");
    }
    // vitaGL creates each render target for one scene a frame. The game goes
    // back to its frame buffer 5-6 times a frame (effect buffers between), so
    // every new scene on it waited for the GPU to finish the previous one.
    vglSetupRenderTargetScenesNum(1, static_cast<std::uint8_t>(g_fbo_scenes));
    {
        char line[64];
        std::snprintf(line, sizeof(line), "FBO render targets: %d scenes per frame", g_fbo_scenes);
        trace(line);
    }
    // gl-89q: a full parameter buffer splits scenes into partial renders
    // (every tile stored and reloaded), more often the larger the target.
    if (g_param_buffer_mib > 0) {
        vglSetParamBufferSize(static_cast<std::uint32_t>(g_param_buffer_mib) * kMiB);
        char line[64];
        std::snprintf(line, sizeof(line), "GXM parameter buffer %d MiB", g_param_buffer_mib);
        trace(line);
    }
    // The result only says whether the resolution had to fall back (GL_TRUE);
    // vitaGL has no failure return here.
    if (vglInitWithCustomSizes(0, 960, 544, ram, cdram, phycont, 0, SCE_GXM_MULTISAMPLE_NONE))
        trace("vitaGL initialised (resolution fell back)");
    else
        trace("vitaGL initialised");
    // The emulation paces itself on PSP vblanks; waiting for the Vita's vblank
    // in the swap only stalls the worker.
    vglWaitVblankStart(GL_FALSE);
    return true;
}

void swap_buffers() { vglSwapBuffers(GL_FALSE); }

std::string memory_report() {
    char text[160];
    std::snprintf(text, sizeof(text), "vitaGL free: cdram %u KiB, ram %u KiB, phycont %u KiB",
                  static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024u), static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024u),
                  static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024u));
    return text;
}

} // namespace burnout::gl
