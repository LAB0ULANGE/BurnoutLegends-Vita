// Headless Burnout Legends benchmark for PS Vita.
//
// Expects the user's own game files under ux0:data/burnout/:
//   EBOOT_DECRYPTED.ELF, PARAM.SFO, USRDIR/, ms0/PSP/SAVEDATA/ULES00125SAVE000/
// Runs the scripted race once at 444 MHz, the highest ARM clock a retail Vita
// grants an app without plugins (clock plugins with per-title profiles, such as
// PSV-VSH-Menu, override the request; the report logs the clock obtained), and
// writes the per-frame statistics to ux0:data/burnout-bench.txt. The screen only shows a
// solid status colour: blue running, green done, red error.
#include "burnout_hle.hpp"
#include <filesystem>
#include "core_load.hpp"
#include "audio_out.hpp"
#include "music.hpp"
#include "sas_worker.hpp"
#include "burnout_run.hpp"
#if defined(BURNOUT_VITA_GL)
#include "boot_menu_vita.hpp"
#include "ge_gl.hpp"
#include "ge_render.hpp"

#include <psp2/ctrl.h>
#endif

#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
// Runtime state is heap-allocated (32 MiB guest RAM plus dispatch tables); the
// generated units chain natively up to 48 frames deep.
int _newlib_heap_size_user = 160 * 1024 * 1024;
// vita-elf-create (Windows build) writes the SCE module tables past the end of
// its copy of the text segment, so it corrupts its heap (exit 0xC0000374)
// unless the segment ends early in a page. When it crashes again, grow this
// pad until `readelf -l` shows the first LOAD memsz ending in 0x000-0x400.
__attribute__((used)) const unsigned char burnout_text_pad[0xebfc] = {1};
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
}

#if defined(BURNOUT_VITA_PROFILE)
extern "C" volatile std::uint32_t psprecomp_profile_label;
#endif

namespace {

constexpr const char *kRoot = "ux0:data/burnout/";
#if defined(BURNOUT_VITA_GL)
// Rendering build: the game on screen with the real pad (the scripted race
// inputs are still OR-ed in), paced to real time; START+SELECT quits.
constexpr const char *kBuild = "gl-103";
constexpr bool kAllProbe = false;  // gl-90/91 tests: GPU probe on every draw, 600 frames per mode
constexpr int kProbeSet = 1;  // 1: pixels (gl-90), 2: draws (gl-92), 3: geometry (gl-93)
constexpr bool kGpuProbe = false;  // diagnostic: GPU probe modes, 600 frames each
constexpr bool kNoTrees = false;
constexpr bool kMediump = false;  // test C: variant fragment shaders at medium precision
constexpr bool kReorder = false;  // E: alpha-tested opaque draws after the opaque scenery
constexpr bool kMergeCutouts = false;  // cut-out PRIMs in world space, merged into few draws
constexpr bool kMergeAll = false;  // ... and every unlit unblended PRIM
constexpr bool kCutoutPrepass = false;  // gl-72: cut-outs depth-only with discard, then colour at EQUAL depth
constexpr unsigned kDrawBudget = 0;  // gl-74 test: draws past this per frame dropped (0: none)
constexpr int kForceScale = 0;  // test builds: 4 = 840x476, 3 = 720x408, 1 = 480x272 without the menu (0: settings)
constexpr bool kSharpen = true;  // sharpen the enlarged picture (render scale below 2)
constexpr bool kDynamicRes = true;  // gl-98d: 960x544 down to 720x408 only when frames run late (valid region + tight regions)
constexpr int kFboScenes = 8;  // gl-96 tests: GXM scenes per frame of FBO render targets
constexpr int kParamBufferMiB = 0;  // gl-89q test: GXM parameter buffer (0: 16 MiB default)
constexpr bool kValidRegion = true;  // one target per screen buffer, patched vitaGL processes only the drawn tiles
//constexpr bool kDynamicRes = false;  // gl-88: off. Any dynamic target set (76-87d) made present wait 12-35 ms; one level fixed at creation (87e, static) 0.4 ms
constexpr bool kOnlyLevel = false;  // test: dynamic targets with the current level alone
constexpr bool kTightRegions = true;  // gl-97: scenes limited to the columns drawn (bloom buffer: a quarter)
constexpr bool kEffect1x = false;  // gl-95 test: full-width effect buffer 110000 at 1x
constexpr bool kLazyLevels = true;  // dynamic levels not cleared at creation (no GXM render target until drawn)
constexpr float kDynamicPin = 0.0f;  // test: that scale always (1.5 = 720x408), targets never recreated
constexpr bool kThinFoliage = false;  // gl-77: far foliage partly left out
constexpr float kThinNear = 256.0f, kThinFar = 512.0f;
constexpr bool kBlendDeferred = false;  // F: those draws blend a 0/1 alpha instead of discarding
constexpr bool kSoundCore2 = true;  // test B: sound on core 2, the decode worker alone on core 1
constexpr bool kStencilAB = false;  // diagnostic: stencil full/minimal/off, 600 frames each
constexpr bool kDiagnosticCaptures = false;  // true: frames to ux0:data/burnout-frames during play
constexpr const char *kReport = "ux0:data/" BURNOUT_VITA_FILES ".txt";
constexpr const char *kLog = "ux0:data/" BURNOUT_VITA_FILES ".log";
burnout::boot_menu::Settings g_settings;
#else
constexpr const char *kReport = "ux0:data/burnout-bench.txt";
constexpr const char *kLog = "ux0:data/burnout-bench.log";
#endif

// Same sequence as profiles/burnout/scripts/race_world_tour_compact.txt.
constexpr const char *kDefaultScript =
    "hold   120   126  START\n"
    "hold   420   426  START\n"
    "hold   600   606  CROSS\n"
    "hold   800   806  CIRCLE\n"
    "pulse  950  1500   90  6  CROSS\n"
#if defined(BURNOUT_VITA_GL)
    // From the race start the player drives.
    "";
#else
    "hold  1500 99999  CROSS\n"
    "pulse 1500 99999  240 30 R\n";
#endif

std::uint32_t *g_framebuffer = nullptr;

#if defined(BURNOUT_VITA_PROFILE)
#if defined(BURNOUT_VITA_GL)
constexpr const char *kProfile = "ux0:data/" BURNOUT_VITA_FILES "-profile.txt";
#else
constexpr const char *kProfile = "ux0:data/burnout-profile.txt";
#endif
std::atomic<bool> g_measuring{false};
std::atomic<bool> g_profiler_stop{false};
std::unordered_map<std::uint32_t, std::uint32_t> g_samples;

void write_profile();

// Runs on another core and samples the last block the emulation thread entered.
// The report is written as soon as the window closes (a player may leave with
// the PS button rather than START+SELECT).
int profiler_thread(SceSize, void *) {
    g_samples.reserve(1u << 16u);
    bool was_measuring = false;
    while (!g_profiler_stop.load(std::memory_order_relaxed)) {
        const bool measuring = g_measuring.load(std::memory_order_relaxed);
        if (measuring) ++g_samples[static_cast<std::uint32_t>(psprecomp_profile_label)];
        else if (was_measuring) write_profile();
        was_measuring = measuring;
        sceKernelDelayThread(100);
    }
    return 0;
}

void write_profile() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranked(g_samples.begin(), g_samples.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::uint64_t total = 0u;
    for (const auto &entry : ranked) total += entry.second;
    std::FILE *out = std::fopen(kProfile, "w");
    if (out == nullptr) return;
    std::fprintf(out, "samples %llu\n", static_cast<unsigned long long>(total));
    for (const auto &[label, count] : ranked) std::fprintf(out, "%08lx %lu\n", static_cast<unsigned long>(label),
                                                           static_cast<unsigned long>(count));
    std::fclose(out);
}
#endif

void show_status(std::uint32_t abgr) {
#if defined(BURNOUT_VITA_GL)
    (void)abgr;  // vitaGL owns the display
    return;
#endif
    constexpr int kWidth = 960, kHeight = 544;
    if (g_framebuffer == nullptr) {
        const SceUID block = sceKernelAllocMemBlock("status_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                    (kWidth * kHeight * 4 + 0x3FFFF) & ~0x3FFFF, nullptr);
        if (block < 0 || sceKernelGetMemBlockBase(block, reinterpret_cast<void **>(&g_framebuffer)) < 0) return;
    }
    for (int i = 0; i < kWidth * kHeight; ++i) g_framebuffer[i] = abgr;
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    frame.base = g_framebuffer;
    frame.pitch = kWidth;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = kWidth;
    frame.height = kHeight;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

int keep_awake(SceSize, void *) {
    for (;;) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelDelayThread(5 * 1000 * 1000);
    }
    return 0;
}

void append_report(const std::string &text) {
    std::ofstream out(kReport, std::ios::app);
    out << text;
}

std::string run_pass(int arm_mhz, const burnout::InputScript &script, bool timed) {
    const int arm_set = scePowerSetArmClockFrequency(arm_mhz);
    scePowerSetBusClockFrequency(222);
    // The GPU's maximum for applications, and its memory crossbar with it
    // (the GPU side is what limits the heavier tracks at 960x544).
    const int gpu_set = scePowerSetGpuClockFrequency(222);
    const int xbar_set = scePowerSetGpuXbarClockFrequency(166);

    burnout::BenchOptions options;
    const std::string root = kRoot;
    options.elf = root + "EBOOT_DECRYPTED.ELF";
    options.usrdir = root + "USRDIR";
    options.disc_root = root;
    // Saves are Adrenaline's: the PSP memory stick of the Vita, in the PSP
    // format (shared with the PSP game). Before anything is written there,
    // the game's save folders are copied once to backup_adrenaline; plain
    // saves of builds before gl-103 (ms0/) are then moved there, encrypted.
    options.memstick = "ux0:pspemu";
    options.save_backup = root + "backup_adrenaline";
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path saves = "ux0:pspemu/PSP/SAVEDATA", backup = options.save_backup;
        const fs::path old_saves = root + "ms0/PSP/SAVEDATA";
        if (fs::is_directory(old_saves, ec) && !fs::exists(backup, ec)) {
            fs::create_directories(backup, ec);
            for (const auto &entry : fs::directory_iterator(saves, ec))
                if (entry.path().filename().string().rfind("ULES00125", 0) == 0)
                    fs::copy(entry.path(), backup / entry.path().filename(), fs::copy_options::recursive, ec);
        }
        burnout::migrate_plain_saves(old_saves, saves, "ULES00125");
    }
    options.save_slot = static_cast<std::uint32_t>(g_settings.save_slot);
    options.script = script;
    options.max_vblanks = 9000u;
#if defined(BURNOUT_VITA_GL)
    // The scripted test race (an A/B measurement) stops by itself after 150 s
    // of PSP time, about 2 minutes of race; free play never does.
    options.max_vblanks = timed ? 9000u : UINT64_MAX;
    options.realtime = true;
    // Walks the display lists on another core (no faster: the game waits for
    // the GE right after submitting).
    options.async_ge = g_settings.async_ge;
    options.live_buttons = [] {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        // L + R + DOWN: a screenshot in ux0:data/burnout-frames and the next
        // frame's draws in the log (one per press; L + R + SELECT opens the VSH menu).
        static bool held = false;
        const bool combo = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_DOWN)) ==
                           (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_DOWN);
        if (combo && !held) burnout::render::request_capture();
        held = combo;
        return static_cast<std::uint32_t>(pad.buttons) & 0xF3F9u;  // PSP-compatible bits
    };
    options.live_analog = [] {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        return static_cast<std::uint32_t>(pad.lx) | (static_cast<std::uint32_t>(pad.ly) << 8u);  // left stick
    };
    options.quit_requested = [] {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        return (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT);
    };
#else
    (void)timed;
    options.hash_trace = "ux0:data/burnout-hashes.txt";
#endif
#if defined(BURNOUT_VITA_PROFILE)
    options.measuring = &g_measuring;
#endif
    std::ostringstream title;
    title << "burnout-bench (PS Vita headless) ARM " << scePowerGetArmClockFrequency() << " MHz (requested " << arm_mhz
          << ", ret 0x" << std::hex << arm_set << std::dec << "), bus "
          << scePowerGetBusClockFrequency() << " MHz, GPU " << scePowerGetGpuClockFrequency() << " MHz (ret 0x" << std::hex
          << gpu_set << std::dec << "), GPU crossbar " << scePowerGetGpuXbarClockFrequency() << " MHz (ret 0x" << std::hex
          << xbar_set << std::dec << ")";
    options.title = title.str();
    std::fprintf(stderr, "[vita] %s\n", options.title.c_str());

    const burnout::BenchResult result = burnout::run_benchmark(options);
    std::ostringstream text;
    text << result.report << "host time for the whole pass: " << result.host_seconds << " s\n\n";
#if defined(BURNOUT_VITA_GL)
    text << burnout::render::report();
#endif
    return text.str();
}

} // namespace

int main() {
#if defined(BURNOUT_VITA_GL)
    // One log per build (ux0:data/burnout-gl-71m.log...), so several test
    // builds run in a row keep their logs; the previous session's is kept.
    const std::string log = std::string("ux0:data/burnout-") + kBuild + ".log";
    std::remove((log + ".prev").c_str());
    std::rename(log.c_str(), (log + ".prev").c_str());
    std::freopen(log.c_str(), "w", stderr);
#else
    std::freopen(kLog, "w", stderr);
#endif
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    show_status(0xFF804000u);  // blue: running
#if defined(BURNOUT_VITA_GL)
    const std::string label = std::string(BURNOUT_VITA_LABEL).empty() ? std::string(kBuild)
                                                                      : std::string(kBuild) + " " + BURNOUT_VITA_LABEL;
    g_settings = burnout::boot_menu::run(kRoot, label);
    if (kForceScale != 0) g_settings.render_scale = kForceScale;
    if (kSoundCore2) g_settings.sound_core1 = false;  // test: SAS mixer and audio beside the GL thread
    std::fprintf(stderr, "[vita] %s: %s, scale %d, sound %d, frame skip %d, shader cache %d\n", kBuild,
                 g_settings.auto_race ? "scripted test race" : "free play", g_settings.render_scale, g_settings.sound ? 1 : 0,
                 g_settings.frame_skip ? 1 : 0, g_settings.shader_cache ? 1 : 0);
    std::fprintf(stderr, "[vita] options: skip when behind %d, offscreen 1x %d, cached sub-lists %d, sound on core %d, SAS thread %d\n",
                 g_settings.skip_when_behind ? 1 : 0, g_settings.offscreen_1x ? 1 : 0, g_settings.cache_sublists ? 1 : 0,
                 g_settings.sound_core1 ? 1 : 2, g_settings.sas_thread ? 1 : 0);
    const bool scripted = g_settings.auto_race;
#else
    const bool scripted = true;
#endif
    const SceUID awake = sceKernelCreateThread("keep_awake", keep_awake, 0x10000100, 0x1000, 0, 0, nullptr);
    if (awake >= 0) sceKernelStartThread(awake, 0, nullptr);

#if defined(BURNOUT_VITA_PROFILE) || defined(BURNOUT_VITA_GL)
    // Keep the emulation thread on core 0, off the sampler's or GL worker's core.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    burnout::core_load::register_current_thread("emulation");
#endif
#if defined(BURNOUT_VITA_PROFILE)
    const SceUID profiler = sceKernelCreateThread("profiler", profiler_thread, 0x10000100, 0x10000, 0,
                                                  SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (profiler >= 0) sceKernelStartThread(profiler, 0, nullptr);
#endif

    bool ok = true;
    {
        std::ofstream(kReport, std::ios::trunc)
            << "Burnout Legends ULES00125 - headless recomp benchmark\n"
            << "build: arm-vita-eabi-g++ " << __VERSION__ << ", generated code " BURNOUT_GENERATED_OPT " -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard\n"
            << "window: 60 s of race (PSP time), starting 10 s after the race data loads\n\n";
    }
    try {
        burnout::InputScript script;
        if (!scripted) {
            // Free play: the player goes through the menus. The intro movie
            // (not decoded: a black screen) is skipped as the script does.
            script.parse_line("hold   120   126  START");
        } else if (std::FILE *custom = std::fopen((std::string(kRoot) + "race.txt").c_str(), "r")) {
            std::fclose(custom);
            script.load(std::string(kRoot) + "race.txt");
        } else {
            std::istringstream lines(kDefaultScript);
            for (std::string line; std::getline(lines, line);) script.parse_line(line);
        }
#if defined(BURNOUT_VITA_GL)
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        burnout::render::Options render;
        render.enabled = true;
        render.backend = burnout::render::Backend::OpenGL;
        render.capture_dir = "ux0:data/burnout-frames";  // L + R + SELECT screenshots too
        if (scripted) {
            render.capture_vblanks = {2101u, 3001u};
            render.capture_dir = "ux0:data/burnout-frames";
        } else if (kDiagnosticCaptures) {
            // Diagnostic build: a frame every 5 s of play, to see on the PC
            // what the Vita shows (gl-55: the bluish lower half on Lakeside).
            for (std::uint64_t vblank = 1500u; vblank < 40000u; vblank += 300u) render.capture_vblanks.insert(vblank);
            render.capture_dir = "ux0:data/burnout-frames";
        }
        render.frame_skip = g_settings.frame_skip;
        render.skip_when_behind = g_settings.skip_when_behind;
        render.offscreen_1x = g_settings.offscreen_1x;
        render.cache_sublists = g_settings.cache_sublists;
        render.render_scale = g_settings.render_scale == 3 ? 1.5f : g_settings.render_scale == 4 ? 1.75f : static_cast<float>(g_settings.render_scale);  // 2 = 960x544, 4 = 840x476, 3 = 720x408, 1 = 480x272
        if (g_settings.shader_cache) {
            const std::string cache = std::string(kRoot) + "shader_cache";
            sceIoMkdir(cache.c_str(), 0777);
            render.shader_cache_dir = cache;
        }
        render.cached_gpu_memory = g_settings.cached_gpu_memory;
        render.variant_list = std::string(kRoot) + "shader_variants.txt";
        std::fprintf(stderr, "[vita] render scale %g\n", static_cast<double>(render.render_scale));
        std::fprintf(stderr, "[vita] starting the renderer\n");
        burnout::gl::set_lazy_levels(kLazyLevels);
        burnout::gl::set_effect_1x(kEffect1x);
        burnout::gl::set_tight_regions(kTightRegions);
        burnout::gl::set_valid_region(kValidRegion);
        burnout::gl::set_param_buffer_mib(kParamBufferMiB);
        burnout::gl::set_fbo_scenes(kFboScenes);
        burnout::gl::set_only_current_level(kOnlyLevel);
        burnout::gl::set_present_sharpen(kSharpen);
        burnout::gl::set_dynamic_resolution(kDynamicRes, 1.5f, kDynamicPin);
        burnout::gl::set_thin_foliage(kThinFoliage, kThinNear, kThinFar);
        burnout::render::configure(render);
        burnout::gl::set_stencil_ab(kStencilAB);
        burnout::gl::set_gpu_probe(kGpuProbe);
        burnout::gl::set_all_probe(kAllProbe);
        burnout::gl::set_probe_set(kProbeSet);
        burnout::gl::set_no_trees(kNoTrees);
        burnout::gl::set_fragment_mediump(kMediump);
        burnout::gl::set_reorder_cutouts(kReorder);
        burnout::gl::set_merge_cutouts(kMergeCutouts || kMergeAll, kMergeAll);
        burnout::gl::set_blend_deferred_cutouts(kBlendDeferred);
        burnout::gl::set_cutout_prepass(kCutoutPrepass);
        if (kDrawBudget != 0u) burnout::gl::set_draw_budget(kDrawBudget);
        std::fprintf(stderr, "[vita] renderer ready, running the game\n");
        if (!burnout::render::active()) throw std::runtime_error("OpenGL initialisation failed (see the log)");
        // Sound: the SAS voices and audio channels to a BGM port, mixed on
        // core 1 (beside the decode worker, which often waits) or core 2
        // (beside the GL thread, the busiest on the heavier tracks).
        if (!g_settings.sound) {
            std::fprintf(stderr, "[vita] sound off\n");
        } else {
            std::string sound_error;
            const int sound_core = g_settings.sound_core1 ? 1 : 2;
            if (burnout::audio::start(sound_core, {}, sound_error)) {
                // Music: the tracks converted by tools/convert_music.py.
                const int tracks = burnout::music::set_dir(std::string(kRoot) + "music_ima");
                // SAS mixing: on the emulation thread by default. The game
                // reads the voices' state right after each grain, so a mixer
                // thread never overlapped it and its queue and wake-ups cost
                // core 0 more than the mixing itself (PC: SND 1.0 vs 0.6 ms).
                if (g_settings.sas_thread) {
                    const bool mixer = burnout::sas_worker::start(sound_core);
                    std::fprintf(stderr, "[vita] SAS mixer thread %s %d\n", mixer ? "on core" : "unavailable, core", sound_core);
                } else {
                    std::fprintf(stderr, "[vita] SAS mixing on the emulation thread\n");
                }
                std::fprintf(stderr, "[vita] sound on, %d music tracks\n", tracks);
            } else std::fprintf(stderr, "[vita] sound unavailable: %s\n", sound_error.c_str());
        }
#endif
        const std::string report = run_pass(444, script, scripted);
        append_report(report);
#if defined(BURNOUT_VITA_GL)
        if (burnout::audio::enabled()) {
            const auto sound = burnout::audio::counters();
            std::fprintf(stderr, "[vita] sound: %llu frames played, %llu underrun, %llu dropped\n",
                         static_cast<unsigned long long>(sound.played_frames),
                         static_cast<unsigned long long>(sound.underrun_frames),
                         static_cast<unsigned long long>(sound.dropped_frames));
            burnout::audio::stop();
        }
#endif
#if !defined(BURNOUT_VITA_GL)
        if (report.find("stop: benchmark window complete") == std::string::npos) ok = false;
#endif
    } catch (const std::exception &e) {
        append_report(std::string("ERROR: ") + e.what() + "\n");
        std::fprintf(stderr, "[vita] error: %s\n", e.what());
        ok = false;
    }
#if defined(BURNOUT_VITA_PROFILE)
    g_profiler_stop = true;
    if (profiler >= 0) sceKernelWaitThreadEnd(profiler, nullptr, nullptr);
    write_profile();
#endif
    std::fflush(stderr);
    show_status(ok ? 0xFF00C000u : 0xFF0000C0u);  // green done / red error
    sceKernelDelayThread(10 * 1000 * 1000);
    sceKernelExitProcess(0);
    return 0;
}
