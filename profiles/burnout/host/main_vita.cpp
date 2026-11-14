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
#include "texture_pack.hpp"
#include "movie.hpp"
#include "bonus_menu.hpp"
#include "paints.hpp"
#include "overlay.hpp"
#include "bonus.hpp"
#include "filters.hpp"
#include <filesystem>
#include "core_load.hpp"
#include "audio_out.hpp"
#include "trax.hpp"
#include "music.hpp"
#include "sas_worker.hpp"
#include "burnout_run.hpp"
#include "perf_counters.hpp"
#if defined(BURNOUT_VITA_GL)
#include "boot_menu_vita.hpp"
#include "ge_gl.hpp"
#include "ge_render.hpp"

#include <psp2/ctrl.h>
#endif

#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
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
// 0.0.48 ran out of it in a second race with a texture pack (125-140 MiB in
// use in races): 208 MiB, ~110 MiB of user memory stay free (vitaGL's RAM
// pool takes 32 of them).
int _newlib_heap_size_user = 208 * 1024 * 1024;
// vita-elf-create (Windows build) writes the SCE module tables past the end of
// its copy of the text segment, so it corrupts its heap (exit 0xC0000374)
// unless the segment ends early in a page. When it crashes again, grow this
// pad until `readelf -l` shows the first LOAD memsz ending in 0x000-0x400.
#if defined(BURNOUT_VITA_PROFILE)
// Measuring build (block labels): its own size (0x1b3fe4c without the extra 0x2b4).
__attribute__((used)) const unsigned char burnout_text_pad[0x36ff4 + 0x2b4] = {1};
#elif !defined(BURNOUT_VITA_GL)
// Headless bench (burnout-bench.vpk): its own size (0x187f890 without the extra 0x870).
__attribute__((used)) const unsigned char burnout_text_pad[0x36ff4 + 0x870] = {1};
#elif defined(PERF_COUNTERS)
__attribute__((used)) const unsigned char burnout_text_pad[0x36ff4] = {1};
#else
// Players' build (no counters, out/vita-rel): its own size.
constexpr unsigned kReleaseTextPad = 0x2134;  // adjusted until vita-elf-create succeeds
__attribute__((used)) const unsigned char burnout_text_pad[0x36ff4 + kReleaseTextPad] = {1};
#endif
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
// Public release: no launch menu (default settings), no Bonus filters, no
// debug pad combos, no texture packs (not finished yet).
constexpr bool kRelease = true;
// Picture quality: off. Measured on the Vita (Silver Lake): trilinear alone
// made the GPU 10-16 ms late, FXAA alone 20-26 ms (full-screen pass at
// 960x544, whatever the render resolution): 24-26 fps instead of 30.
// BURNOUT_FXAA 2 = the light 5-tap version (no dependent reads), 0.0.5.
#ifndef BURNOUT_FXAA
#define BURNOUT_FXAA 2
#endif
#ifndef BURNOUT_TRILINEAR
#define BURNOUT_TRILINEAR 0
#endif
constexpr bool kFxaa = BURNOUT_FXAA != 0;
constexpr bool kTrilinear = BURNOUT_TRILINEAR != 0;
bool g_perf_no_fxaa{};  // PERF builds: ux0:data/burnout/no_fxaa.txt (GPU load test)
// Optimisation test builds (2026-11-11 night, from the THUG Vita port, one
// letter each, its own log): 'a' larger GXM rings; 'b' + compiled shaders
// kept on the memory card; 'c' + static geometry hashed once in 4 frames.
constexpr char kOptimisations = 'b';  // (c tried 2026-11-13: no gain at 60, scenery flickers: rejected)
#if defined(BURNOUT_VITA_PROFILE)
// Measuring build (own bubble): samples the game code's blocks during 60 s of race.
constexpr const char *kBuild = "0.0.81-mesure";
#elif defined(PERF_COUNTERS)
// Per-frame counters (BRIEF_PERF_60FPS.md): ux0:data/burnout-perf.log.
// kPerfForce480: the 480x272 preset without holding L (the player asked for a build that starts in it).
constexpr bool kPerfForce480 = false;
constexpr const char *kBuild = kPerfForce480 ? "0.0.83-perfo480" : "0.0.83-perfz";
#else
constexpr const char *kBuild = kRelease ? "0.0.3" : "gl-109";
#endif

// Bonus filters (bonus.hpp): state from unlocks.bin, applied to the renderer.
burnout::bonus::State g_bonus;
bool g_psp_picture{};  // PAUSED > OPTIONS > DISPLAY: PSP (480x272 enlarged), else HD
#if defined(PERF_COUNTERS)
bool g_perf_480{};  // reference preset 480x272 (L held at launch)
#endif
void apply_bonus() {
    static burnout::bonus::State saved = g_bonus;
    burnout::gl::set_filters(burnout::bonus::applied(g_bonus));
    burnout::gl::set_antialias_lite(BURNOUT_FXAA == 2);
    burnout::gl::set_antialias(kFxaa && !g_perf_no_fxaa);  // FXAA in races (2: the light version)
    burnout::gl::set_trilinear(kTrilinear);
    std::fprintf(stderr, "[bonus] %s, filters: %s\n", g_bonus.enabled ? "on" : "off",
                 burnout::filters::describe(burnout::bonus::applied(g_bonus)).c_str());
    if (!(saved == g_bonus)) {
        burnout::bonus::save(std::string(kRoot) + "unlocks.bin", g_bonus);
        saved = g_bonus;
    }
}
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
constexpr bool kEffect1x = true;  // the full-width effect buffer 110000 at 1x (gl-95: ~10 ms of GPU a frame; its HUD bug was a stale viewport, fixed)
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
#if !defined(BURNOUT_BENCH_SUFFIX)
#define BURNOUT_BENCH_SUFFIX ""  // A/B bench builds: their own report files
#endif
constexpr const char *kReport = "ux0:data/burnout-bench" BURNOUT_BENCH_SUFFIX ".txt";
constexpr const char *kLog = "ux0:data/burnout-bench" BURNOUT_BENCH_SUFFIX ".log";
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
#if defined(PERF_COUNTERS)
        // Test builds are driven from the PC (vitacompanion), whose synthetic
        // buttons do not count as activity: keep the screen on as well.
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
#else
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
#endif
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
    // Adrenaline's memory stick can also be on these (its "memory stick location" option).
    options.extra_memsticks = {"uma0:pspemu", "imc0:pspemu", "ur0:pspemu", "xmc0:pspemu"};
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
#if defined(BURNOUT_VITA_GL)
    options.save_slot = static_cast<std::uint32_t>(g_settings.save_slot);
#endif  // the headless bench: the first save
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
        if (combo && !held && !kRelease) burnout::render::request_capture();
        held = combo;
#if defined(PERF_COUNTERS)
        // Remote screenshot (test builds, 2026-11-13): the PC drops
        // ux0:data/burnout/shot.txt over FTP; checked twice a second.
        static unsigned shot_poll = 0u;
        if (++shot_poll % 30u == 0u) {
            static const std::string shot = std::string(kRoot) + "shot.txt";
            if (std::FILE *f = std::fopen(shot.c_str(), "rb")) {
                std::fclose(f);
                sceIoRemove(shot.c_str());
                burnout::render::request_capture(false);
                std::fprintf(stderr, "[vita] remote screenshot requested\n");
            }
        }
#endif
        // Bonus filters, live: L + R + RIGHT / LEFT = next / previous filter
        // in slot 1, L + R + UP = all off (saved to unlocks.bin on change).
        const std::uint32_t lr = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
        static std::uint32_t filter_held = 0u;
        const std::uint32_t arrows = (pad.buttons & lr) == lr ? pad.buttons & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_UP) : 0u;
        const std::uint32_t pressed = arrows & ~filter_held;
        filter_held = arrows;
        if (pressed != 0u && !kRelease) {
            if (pressed & SCE_CTRL_RIGHT) burnout::bonus::cycle(g_bonus, 0, 1);
            else if (pressed & SCE_CTRL_LEFT) burnout::bonus::cycle(g_bonus, 0, -1);
            else g_bonus.active = {};
            g_bonus.enabled = true;
            apply_bonus();
        }
        // While L + R are held the arrows drive the filters, not the game.
        std::uint32_t buttons = static_cast<std::uint32_t>(pad.buttons);
        if ((buttons & lr) == lr && !kRelease) buttons &= ~static_cast<std::uint32_t>(SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_UP | SCE_CTRL_DOWN);
        return buttons & 0xF3F9u;  // PSP-compatible bits
    };
    // PAUSED > OPTIONS > BONUS in the game's own pause menu (bonus_menu.cpp);
    // the postcard's caption is written with our font.
    std::fprintf(stderr, "[bonus] postcard font %s\n", burnout::overlay::init("app0:fonts/Oswald-Bold.ttf") ? "loaded" : "MISSING");
    burnout::bonus_menu::configure({
        [] { return g_bonus.enabled; },
        [] { return static_cast<int>(burnout::filters::all().size()); },
        [](int i) { return burnout::filters::all()[static_cast<std::size_t>(i)].name; },
        [](int i) { return burnout::bonus::available(g_bonus, i); },
        [](int i) {
            if (!g_bonus.enabled) return false;
            for (const auto &id : g_bonus.active)
                if (burnout::filters::find(id) == i) return true;
            return false;
        },
        [](int i) {
            if (!g_bonus.enabled) g_bonus.active = {};  // Bonus was off: start from none
            burnout::bonus::toggle(g_bonus, i);
            apply_bonus();
        },
        [](int i) {
            const int signature = burnout::bonus::filter_signature(i);
            return std::string("LOCKED: ") + burnout::bonus::signature_name(signature);
        },
        [](int signature) {
            const int filter = burnout::bonus::signature_filter(signature);
            if (!burnout::bonus::unlock(g_bonus, filter)) return false;
            std::fprintf(stderr, "[bonus] Signature Takedown %s: %s unlocked\n", burnout::bonus::signature_name(signature),
                         burnout::filters::all()[static_cast<std::size_t>(filter)].name);
            apply_bonus();  // saves unlocks.bin
            return true;
        },
        [](int i) {
            const int signature = burnout::bonus::filter_signature(i);
            return signature < 0 ? std::string() : std::string(burnout::bonus::signature_name(signature));
        },
        [](int i, float x, float y, float w, float h, float angle) {
            burnout::gl::set_postcard(i >= 0 ? std::vector<int>{i} : std::vector<int>{}, x, y, w, h, angle);
        },
        [] { burnout::gl::postcard_photo(); },
        [](int i) {
            const burnout::bonus::Reward reward = burnout::bonus::filter_reward(i);
            return reward.source == burnout::bonus::Source::Free ? -1 : static_cast<int>(reward.source) * 100 + reward.index;
        },
        [](float mph) { burnout::gl::set_car_speed(mph); },
        [](int postcards, int headlines, int percent) {
            const int unlocked = burnout::bonus::unlock_progress(g_bonus, postcards, headlines, percent);
            if (unlocked == 0) return 0;
            std::fprintf(stderr, "[bonus] progress %d postcards, %d headlines, %d %%: %d filters unlocked\n", postcards, headlines,
                         percent, unlocked);
            apply_bonus();  // saves unlocks.bin
            return unlocked;
        },
        // No BONUS VITA / DISPLAY rows (2026-11-09: "Burnout Legends 2.0",
        // the original game is Adrenaline's): the Bonus always on, the
        // picture always HD. The PSP picture's code stays (set_picture_scale).
        {},
        {},
        {},
        [](bool up) { burnout::gl::set_filters_suspended(up); },
        // BONUS 2.0's pictures (app0:bonus/, app0:signatures/) and the
        // Signature award without the game's 3D car.
        [](int slot, const std::string &path, float x, float y, float w, float h, float angle) {
            burnout::gl::set_picture(slot, path.empty() ? path : "app0:" + path, x, y, w, h, angle);
        },
        [](bool hide) { burnout::gl::set_hide_3d(hide); },
    });
    // The Signature Takedown photos on the awards screen (bonus_menu.cpp).
    // Tests (2026-11-11): ux0:data/burnout/toutes_les_recompenses.txt arms
    // "every reward at the end of the next race"; when it fires, the saves
    // are copied aside first and the file renamed (once only).
    {
        namespace fs = std::filesystem;
        const fs::path flag = std::string(kRoot) + "toutes_les_recompenses.txt";
        const fs::path done = std::string(kRoot) + "toutes_les_recompenses_fait_0079.txt";
        std::error_code ec;
        // (Test builds 0.0.69-0.0.76 armed it by themselves; 0.0.77: only
        // with toutes_les_recompenses.txt, the user's save having it all.)
        constexpr bool kEveryRewardOnce = false;
        // Test builds 0.0.79-0.0.80: BONUS 2.0 showed everything as won and
        // every colour offered. Off from 0.0.81 (2026-11-12, the user: filters
        // shown unlocked on a save that had not won them; it also wrote all
        // ten into unlocks.bin, which every save shares).
        constexpr bool kShowEverything = false;
        if (kShowEverything) burnout::bonus_menu::set_everything_won(true);
        if (fs::exists(flag, ec) || (kEveryRewardOnce && !fs::exists(done, ec))) {
            // The saves copied aside now, at the launch (2026-11-11: done when
            // it fired, the copying held the race itself).
            {
                std::error_code e;
                const fs::path saves = "ux0:pspemu/PSP/SAVEDATA", backup = std::string(kRoot) + "sauvegardes_avant_toutes_recompenses";
                fs::create_directories(backup, e);
                // (Byte by byte: fs::copy on the Vita left empty files, 0.0.69.)
                const auto copy = [](const fs::path &from, const fs::path &to) {
                    std::FILE *in = std::fopen(from.string().c_str(), "rb");
                    if (in == nullptr) return;
                    std::FILE *out = std::fopen(to.string().c_str(), "wb");
                    std::size_t bytes = 0;
                    if (out != nullptr) {
                        char buffer[16384];
                        for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, in)) > 0;) bytes += std::fwrite(buffer, 1, n, out);
                        std::fclose(out);
                    }
                    std::fclose(in);
                    std::fprintf(stderr, "[test] copied %s: %u bytes\n", to.string().c_str(), static_cast<unsigned>(bytes));
                };
                for (const auto &entry : fs::directory_iterator(saves, e)) {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("ULES00125", 0) != 0) continue;
                    fs::create_directories(backup / name, e);
                    for (const auto &file : fs::directory_iterator(entry.path(), e)) copy(file.path(), backup / name / file.path().filename());
                }
                copy(std::string(kRoot) + "unlocks.bin", backup / "unlocks.bin");
            }
            burnout::bonus_menu::force_all_rewards([flag, done] {
                std::error_code e;
                fs::remove(flag, e);
                // (The file the check above looks for: until 0.0.76 another
                // name was written, and the test came back at every launch.)
                if (std::FILE *f = std::fopen(done.string().c_str(), "w")) {
                    std::fputs("Toutes les recompenses ont ete forcees une fois (0.0.79).\n", f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "[test] every reward fired\n");
                burnout::bonus_menu::set_everything_won(true);
            });
        } else if (kEveryRewardOnce && fs::exists(done, ec)) {
            // Test build, the test done: BONUS 2.0 shows everything as won.
            burnout::bonus_menu::set_everything_won(true);
        }
    }
    burnout::bonus_menu::set_picture_ready([](int slot) { return burnout::gl::picture_ready(slot); });
    burnout::bonus_menu::set_signature_photos("app0:signatures", [](const std::string &path, float x, float y, float w, float h, float angle) {
        burnout::gl::set_photo(path, x, y, w, h, angle);
    });
    burnout::paints::set_texels_written([](std::uint32_t address, std::uint32_t bytes) { burnout::render::cpu_wrote_vram(address, bytes); });
    options.per_vblank = [](psprecomp::GuestMemory &memory) {
        burnout::bonus_menu::update(memory);
        if (burnout::paints::chosen() != g_bonus.paint) {
            g_bonus.paint = burnout::paints::chosen();
            apply_bonus();  // saves unlocks.bin
        }
    };
    options.button_filter = [](std::uint32_t buttons) { return burnout::bonus_menu::filter_buttons(buttons); };
    options.guest_call = [](std::uint32_t &f, std::uint32_t &a0, std::uint32_t &a1) { return burnout::bonus_menu::take_guest_call(f, a0, a1); };
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

    // The player's own music in EA Trax (trax.cpp): MP3s in ux0:data/burnout/musique_perso.
    burnout::trax::configure(std::string(kRoot) + "musique_perso", std::string(kRoot) + "musique_perso.txt");
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
    // First launch: the game files are made from the player's own ISO / CSO.
    if (!burnout::boot_menu::install(kRoot)) {
        std::fflush(stderr);
        sceKernelExitProcess(0);
        return 0;
    }
    g_settings = kRelease ? burnout::boot_menu::Settings{} : burnout::boot_menu::run(kRoot, label);
    // Every launch starts in HD: the PSP picture (PAUSED > OPTIONS > DISPLAY)
    // lasts one session (0.0.48-0.0.49 kept it, and played at 480x272
    // without the player knowing).
    g_psp_picture = false;
    if (kForceScale != 0) g_settings.render_scale = kForceScale;
#if defined(PERF_COUNTERS)
    // Reference presets (BRIEF_PERF_60FPS.md): 960x544, or 480x272 with L held
    // at launch; fixed (no dynamic resolution).
    {
        // Polled for a second: a single read at launch often sees nothing
        // (0.0.82-perf missed L; boot_menu_vita.cpp polls 2 s for the same reason).
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
        g_perf_480 = kPerfForce480;
        for (int frame = 0; frame < 60 && !g_perf_480; ++frame) {
            SceCtrlData pad{};
            sceCtrlPeekBufferPositive(0, &pad, 1);
            g_perf_480 = (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) != 0u;
            sceDisplayWaitVblankStart();
        }
        g_settings.render_scale = g_perf_480 ? 1 : 2;
        burnout::perf::note(g_perf_480 ? (kPerfForce480 ? "preset 480x272 (this build), dynamic resolution off" : "preset 480x272 (L held at launch), dynamic resolution off")
                                       : "preset 960x544, dynamic resolution off");
        // ux0:data/burnout/auto_race.txt: the bench's scripted race (menus
        // driven, World Tour compact race) with the picture, to compare the
        // core-0 time with the headless bench on the same scene.
        if (std::FILE *f = std::fopen((std::string(kRoot) + "auto_race.txt").c_str(), "rb")) {
            std::fclose(f);
            g_settings.auto_race = true;
            burnout::perf::note("auto race on (ux0:data/burnout/auto_race.txt): the bench's scripted race");
        }
        // Draws on / skipped in turn, one thread time report each (the log's
        // "draws on|skipped"): core 0's load without the GPU's traffic.
        // GPU load tests (2026-11-13, the 60 fps mode stops at ~40 i/s with
        // the GL thread waiting in vitaGL): FXAA off, effect buffers at 1x.
        if (std::FILE *f = std::fopen((std::string(kRoot) + "no_fxaa.txt").c_str(), "rb")) {
            std::fclose(f);
            g_perf_no_fxaa = true;
            burnout::perf::note("FXAA off (ux0:data/burnout/no_fxaa.txt)");
        }
        if (std::FILE *f = std::fopen((std::string(kRoot) + "offscreen_1x.txt").c_str(), "rb")) {
            std::fclose(f);
            g_settings.offscreen_1x = true;
            burnout::perf::note("effect buffers at 1x (ux0:data/burnout/offscreen_1x.txt)");
        }
        // (0.0.83-perff: burnout::ge_front_draw_skip_alternate(), draws on /
        // skipped in turn; measured, see PERF_LOG.)
        // 30-fps limiter off in races (tools/patch_frame_rate.py, PC setting
        // fps60) when ux0:data/burnout/fps60.txt exists: the brief's "frame
        // once the limiter is removed".
        if (std::FILE *f = std::fopen((std::string(kRoot) + "fps60.txt").c_str(), "rb")) {
            std::fclose(f);
            burnout::set_sixty_fps(true);
            burnout::perf::note("fps60 on (ux0:data/burnout/fps60.txt): one vblank a frame in races");
            if (std::FILE *g = std::fopen((std::string(kRoot) + "single_step.txt").c_str(), "rb")) {
                std::fclose(g);
                burnout::set_single_step(true);
                burnout::perf::note("single step on (ux0:data/burnout/single_step.txt): one 1/60 s step a frame, the race slows down");
            }
        }
    }
#endif
    if (kSoundCore2) g_settings.sound_core1 = false;  // test: SAS mixer and audio beside the GL thread
    std::fprintf(stderr, "[vita] %s: %s, scale %d, sound %d, frame skip %d, shader cache %d\n", kBuild,
                 g_settings.auto_race ? "scripted test race" : "free play", g_settings.render_scale, g_settings.sound ? 1 : 0,
                 g_settings.frame_skip ? 1 : 0, g_settings.shader_cache ? 1 : 0);
    std::fprintf(stderr, "[vita] options: skip when behind %d, offscreen 1x %d, cached sub-lists %d, sound on core %d, SAS thread %d, GE parallel %d\n",
                 g_settings.skip_when_behind ? 1 : 0, g_settings.offscreen_1x ? 1 : 0, g_settings.cache_sublists ? 1 : 0,
                 g_settings.sound_core1 ? 1 : 2, g_settings.sas_thread ? 1 : 0, g_settings.async_ge ? 1 : 0);
    const bool scripted = g_settings.auto_race;
#else
    const bool scripted = true;
#endif
    const SceUID awake = sceKernelCreateThread("keep_awake", keep_awake, 0x10000100, 0x1000, 0, 0, nullptr);
    if (awake >= 0) sceKernelStartThread(awake, 0, nullptr);
#if !defined(BURNOUT_VITA_PROFILE) && !defined(BURNOUT_VITA_GL)
    // Bench only, ux0:data/burnout/stress.txt: cores 1 and 2 kept busy while
    // the emulation runs alone on core 0 ("mem": 4 MiB copies, the bus and
    // the shared cache; "cpu": register-only arithmetic). Answers whether the
    // game's ~30 ms on core 0 next to the GPU threads, against ~9 ms here,
    // is contention or work (PERF_LOG 2026-11-13).
    const char stress_mode = [] {
        char mode = 0;
        if (std::FILE *f = std::fopen((std::string(kRoot) + "stress.txt").c_str(), "rb")) {
            const int c = std::fgetc(f);
            std::fclose(f);
            mode = c == 'c' ? 'c' : 'm';
        }
        return mode;
    }();
    if (stress_mode != 0) {
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        static char g_stress_mode;
        g_stress_mode = stress_mode;
        const auto stress = [](SceSize, void *) -> int {
            if (g_stress_mode == 'c') {
                volatile std::uint32_t sink = 0;
                std::uint32_t a = 1u, b = 2u;
                for (;;) {
                    for (int i = 0; i < 1 << 20; ++i) { a = a * 1664525u + 1013904223u; b ^= a >> 3; }
                    sink = a + b;
                }
            }
            static thread_local std::vector<std::uint8_t> from(4u << 20u, 1u), to(4u << 20u);
            for (;;) sceClibMemcpy(to.data(), from.data(), from.size());
            return 0;
        };
        for (const int mask : {SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2}) {
            const SceUID t = sceKernelCreateThread("stress", stress, 0x10000100, 0x10000, 0, mask, nullptr);
            if (t >= 0) sceKernelStartThread(t, 0, nullptr);
        }
    }
#endif

#if defined(BURNOUT_VITA_PROFILE) || defined(BURNOUT_VITA_GL)
    // Keep the emulation thread on core 0, off the sampler's or GL worker's core.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    burnout::core_load::register_current_thread("emulation");
    std::fprintf(stderr, "[vita] emulation thread priority %d\n", sceKernelGetThreadCurrentPriority());
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
            << "window: 60 s of race (PSP time), starting 10 s after the race data loads\n"
#if !defined(BURNOUT_VITA_PROFILE) && !defined(BURNOUT_VITA_GL)
            << "stress on cores 1-2: " << (stress_mode == 'c' ? "cpu (registers only)" : stress_mode == 'm' ? "mem (4 MiB copies)" : "none") << "\n"
#endif
            << "\n";
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
        // The Bonus postcards: ready-made pictures. (0.0.10-0.0.16 also kept
        // the filters' programs on the memory card: loading one back with
        // glProgramBinary crashed the game, C2-12828-1.)
        burnout::gl::set_postcard_pictures("app0:postcards");
        burnout::gl::set_sun_glare(0.5f);  // the game's sun glare at half strength (the player found it too dazzling)
#if defined(PERF_COUNTERS)
        // Test (2026-11-14, the cut sky): ux0:data/burnout/sun_glare.txt holds the glare's strength.
        if (std::FILE *f = std::fopen((std::string(kRoot) + "sun_glare.txt").c_str(), "rb")) {
            float glare = 0.5f;
            if (std::fscanf(f, "%f", &glare) == 1) burnout::gl::set_sun_glare(glare);
            std::fclose(f);
            std::fprintf(stderr, "[vita] test switch sun_glare.txt: %g\n", static_cast<double>(glare));
        }
#endif
        render.cached_gpu_memory = g_settings.cached_gpu_memory;
        render.variant_list = std::string(kRoot) + "shader_variants.txt";
        std::fprintf(stderr, "[vita] render scale %g\n", static_cast<double>(render.render_scale));
        std::fprintf(stderr, "[vita] starting the renderer\n");
        burnout::gl::set_lazy_levels(kLazyLevels);
        burnout::gl::set_effect_1x(kEffect1x);
#if defined(PERF_COUNTERS)
        // Test switches (2026-11-14, the cut sky): no_tight.txt, no_valid.txt.
        const auto test_switch = [](const char *name) {
            std::error_code ec;
            const bool on = std::filesystem::exists(std::string(kRoot) + name, ec);
            if (on) std::fprintf(stderr, "[vita] test switch %s\n", name);
            return on;
        };
        burnout::gl::set_tight_regions(kTightRegions && !test_switch("no_tight.txt"));
        burnout::gl::set_valid_region(kValidRegion && !test_switch("no_valid.txt"));
        burnout::bonus_menu::set_heavy_test_all(test_switch("tous_les_poids_lourds.txt"));
#else
        burnout::gl::set_tight_regions(kTightRegions);
        burnout::gl::set_valid_region(kValidRegion);
#endif
        burnout::gl::set_param_buffer_mib(kParamBufferMiB);
        {
            std::error_code ec;
            const bool rings = !std::filesystem::exists(std::string(kRoot) + "anneaux_gxm_normaux.txt", ec);
            const bool shaders = kOptimisations >= 'b';
            const std::uint32_t probe = kOptimisations >= 'c' ? 4u : 1u;
            burnout::gl::set_large_rings(rings);
            if (shaders) {
                const std::string kept = std::string(kRoot) + "shd";
                sceIoMkdir(kept.c_str(), 0777);
                burnout::gl::set_gxp_cache(kept);
            }
            burnout::gl::set_probe_every(probe);
#if defined(PERF_COUNTERS)
            // Test: ux0:data/burnout/probe_every.txt holds N (static geometry
            // hashed once in N frames, pieces seen changing every frame).
            if (std::FILE *f = std::fopen((std::string(kRoot) + "probe_every.txt").c_str(), "rb")) {
                int every = 0;
                if (std::fscanf(f, "%d", &every) == 1 && every >= 1) burnout::gl::set_probe_every(static_cast<std::uint32_t>(every));
                std::fclose(f);
                std::fprintf(stderr, "[vita] probe_every.txt: static geometry hashed once in %d frames\n", every);
            }
#endif
            std::fprintf(stderr, "[vita] optimisations %c: GXM rings %s, compiled shaders kept %s, geometry hashed every %u frames\n",
                         kOptimisations, rings ? "large" : "default", shaders ? "yes" : "no", static_cast<unsigned>(probe));
        }
        burnout::gl::set_fbo_scenes(kFboScenes);
        burnout::gl::set_only_current_level(kOnlyLevel);
        burnout::gl::set_present_sharpen(kSharpen);
        // Bonus filters: chosen in the game's menus (bonus_menu); unlocks kept
        // in unlocks.bin, but every launch starts without a filter on.
        g_bonus = burnout::bonus::load(std::string(kRoot) + "unlocks.bin");
        g_bonus.enabled = true;  // always (0.0.47-0.0.50 could save it off: no BONUS row, no Heavyweights)
        g_bonus.active = {};
        burnout::paints::set_chosen(g_bonus.paint);  // the paint chosen last time
        apply_bonus();
        // PPSSPP texture pack: ux0:data/burnout/textures/<game id>/textures.ini.
        // Only when the player installed one: nothing changes otherwise.
        for (const char *id : {"ULES00125", "ULUS10025", ""})
            if (burnout::texture_pack::open(std::string(kRoot) + "textures/" + id)) break;
        // PSP picture (PAUSED > OPTIONS > DISPLAY): a fixed 480x272 inside the targets.
#if defined(PERF_COUNTERS)
        // Fixed presets, or as the players' builds with dynres.txt (2026-11-14).
        burnout::gl::set_dynamic_resolution(test_switch("dynres.txt") && kDynamicRes, 1.5f, kDynamicPin);
#else
        burnout::gl::set_dynamic_resolution(kDynamicRes, 1.5f, kDynamicPin);
#endif
        if (g_psp_picture) burnout::gl::set_picture_scale(1.0f);
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
        // The menu backgrounds, demuxed in the background before the game
        // asks for them. Started once the renderer is up, at normal priority:
        // during its boot a starved lowest-priority prefetch held the heap
        // lock and the renderer never got ready (0.0.2 test hang).
        burnout::movie::prefetch({std::string(kRoot) + "USRDIR/ovid/FEMain.pmf", (std::filesystem::exists("app0:ovid/startB.pmf") ? std::string("app0:ovid/startB.pmf") : std::string(kRoot) + "USRDIR/ovid/startB.pmf"),
                                  std::string(kRoot) + "USRDIR/ovid/WTour.pmf"});
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
