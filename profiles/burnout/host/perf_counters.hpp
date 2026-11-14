#pragma once
// Per-frame performance counters (BRIEF_PERF_60FPS.md, step 0). Built only
// with -DPERF_COUNTERS (CMake: -DBURNOUT_PERF_COUNTERS=ON); otherwise every
// PERF_ONLY(...) site compiles to nothing.
//
// Each series belongs to one thread and gets one value per frame of that
// thread. Every 600 values a line goes to the log (ux0:data/burnout-perf.log
// on the Vita, burnout_perf.log in the working directory on the PC): average,
// maximum and the worst 1% (the 99th percentile: the 6th largest of 600).
//
// Series (ms unless noted), by thread:
//   emulation (core 0)  t_cpu_game     flip to flip, minus the time the
//                                      emulation thread was blocked (waits for
//                                      the GE front end / the SAS worker, real-
//                                      time pacing): recompiled code + HLE
//                       t_audio        SAS mixing of the frame (on the mixer
//                                      core when the SAS worker runs, which the
//                                      line says)
//   decode (core 1)     t_ge_translate display lists walked and turned into GL
//                                      ops (decode + record), per presented frame
//                       vertices       vertices converted (count)
//                       tex_uploads    texture uploads recorded (count)
//   GL (core 2)         t_gl_submit    frame replay (vitaGL calls), minus present
//                       t_swap         present: last quad, vglSwapBuffers and its
//                                      wait for the GPU / vsync
//                       draws          draw calls (count)
//                       state_changes  GL state ops replayed (enable/disable,
//                                      blend, depth, stencil, masks, scissor,
//                                      viewport, program, framebuffer, texture
//                                      binds and parameters, uniforms) (count)
#include <cstdint>

#if defined(PERF_COUNTERS)
#define PERF_ONLY(...) __VA_ARGS__

namespace burnout::perf {

enum class Series : unsigned {
    CpuGame, Audio, GeTranslate, Vertices, TextureUploads, GlSubmit, Swap, Draws, StateChanges, Count
};

// One value of a series' frame (from that series' thread only).
void push(Series series, double value) noexcept;

// Emulation thread: time spent blocked (not running guest code or HLE).
void emulation_blocked(double ms) noexcept;
// Emulation thread, at each sceDisplaySetFrameBuf: closes its frame.
// audio_ms: SAS mixing since the previous flip; audio_other_core: on the SAS worker.
void emulation_flip(std::uint64_t vblank, double audio_ms, bool audio_other_core) noexcept;

// SAS mixing time, accumulated by whoever mixes (any thread).
void add_audio_ms(double ms) noexcept;

// A free line in the log (the run's settings).
void note(const char *text) noexcept;

}  // namespace burnout::perf

#else
#define PERF_ONLY(...)
#endif
