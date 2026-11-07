#pragma once

#include <string>

namespace burnout::boot_menu {

// What the player picks on the Vita launch screen; kept in
// ux0:data/burnout/settings.txt between launches.
struct Settings {
    bool auto_race{};       // scripted menus up to a World Tour race (measurements)
    int render_scale{2};    // default 960x544, lowered by the dynamic resolution (gl-99); 2 = 960x544, 4 = 840x476, 3 = 720x408, 1 = the PSP's 480x272
    bool sound{true};
    bool frame_skip{};      // drop a frame when the GPU side falls behind (off: the user wants every frame)
    bool async_ge{};        // display lists walked on another core (no longer offered: crashed)
    bool shader_cache{};    // compiled shader programs kept on the memory card
    bool cached_gpu_memory{};  // vitaGL pools in cached memory (no longer offered)
    // gl-48, each on its own so the log tells which one helps or breaks.
    bool skip_when_behind{};      // the game never waits for the GPU side (frames skipped instead)
    bool offscreen_1x{};          // the game's effect buffers at 1x (off: changed the car lighting, gain negligible)
    bool cache_sublists{true};    // unchanged GE sub-lists forwarded from a copy
    int save_slot{0};             // profile loaded: entry of the game's save list (0-3)
    bool sound_core1{true};       // SAS mixer and audio output on core 1 instead of beside the GL thread
    bool sas_thread{true};        // SAS mixing on its own thread; the emulation keeps shadow voices (gl-58)
};

// Loads the saved settings (or the old per-option flag files).
[[nodiscard]] Settings load(const std::string &root);
void save(const std::string &root, const Settings &settings);

// Shows the menu on a plain framebuffer until the player starts the game.
// Must run before the renderer takes the display (its frame buffer stays).
[[nodiscard]] Settings run(const std::string &root, const std::string &build);

} // namespace burnout::boot_menu
