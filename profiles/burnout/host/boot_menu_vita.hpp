#pragma once

#include <string>

namespace burnout::boot_menu {

// What the player picks on the Vita launch screen; kept in
// ux0:data/burnout/settings.txt between launches.
struct Settings {
    bool auto_race{};       // scripted menus up to a World Tour race (measurements)
    int render_scale{2};    // 2 = 960x544, 1 = the PSP's 480x272
    bool sound{true};
    bool frame_skip{true};  // drop a frame when the GPU side falls behind
    bool async_ge{};        // display lists walked on another core (experimental)
    bool shader_cache{};    // compiled shader programs kept on the memory card
    bool cached_gpu_memory{};  // vitaGL pools in cached memory (experimental)
};

// Loads the saved settings (or the old per-option flag files).
[[nodiscard]] Settings load(const std::string &root);
void save(const std::string &root, const Settings &settings);

// Shows the menu on a plain framebuffer until the player starts the game.
// Must run before the renderer takes the display; releases its framebuffer.
[[nodiscard]] Settings run(const std::string &root, const std::string &build);

} // namespace burnout::boot_menu
