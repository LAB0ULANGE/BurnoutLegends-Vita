#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace burnout {

// PSP vblank period at 59.94 Hz, in microseconds of virtual PSP time.
inline constexpr std::uint64_t kVblankPeriodUs = 16683u;

struct HleOptions {
    std::filesystem::path usrdir;      // PSP_GAME/USRDIR of the user's own disc
    std::filesystem::path disc_root;   // directory holding PARAM.SFO etc. (optional)
    std::filesystem::path memstick;    // writable stand-in for ms0:
    std::filesystem::path save_backup; // copy of the save folders made before the first write (empty: none)
    std::vector<std::filesystem::path> extra_memsticks;  // other PSP memory sticks searched for saves (Adrenaline's)
    // Save list dialogs (no system UI): load picks this entry of the game's
    // list when it exists (0 = first), a new save the first free entry.
    std::uint32_t save_slot{};
    std::uint32_t user_arena_start{};  // first byte after the loaded ELF image
    std::uint32_t module_gp{};
    bool verbose{};
};

// Events the headless benchmark observes. All callbacks run on the emulation
// thread and must stay cheap.
struct HleObserver {
    std::function<void(std::uint64_t vblank)> on_vblank;
    std::function<void(std::int32_t thread_uid, std::uint64_t vblank)> on_wait_vblank;
    std::function<void(std::uint32_t frame_buffer, std::uint64_t vblank)> on_set_frame_buffer;
    std::function<void(const std::string &path)> on_file_open;
    std::function<std::uint32_t(std::uint64_t vblank)> controller_buttons;
    // Analog stick: x in the low byte, y in the next (128 = centre).
    std::function<std::uint32_t()> controller_analog;
};

void install_hle(psprecomp::Runtime &runtime, const HleOptions &options, HleObserver observer);

// Registers a logging stub for every module import that install_hle() did not
// implement, so an unexpected call is visible instead of stopping the run.
void install_fallback_stubs(psprecomp::Runtime &runtime,
                            const std::vector<std::pair<std::string, std::uint32_t>> &imports);

// Prepares the loader thread that runs module_start, exactly as the PSP
// kernel does for the boot module.
void prepare_boot_thread(psprecomp::Runtime &runtime, const std::string &boot_path);

[[nodiscard]] std::uint64_t virtual_time_us() noexcept;
[[nodiscard]] std::uint64_t vblank_count() noexcept;
void print_hle_summary();

// Plain saves of an older build (under old_root) rewritten in the PSP format
// under new_root, once (old_root is then renamed *_migrated).
void migrate_plain_saves(const std::filesystem::path &old_root, const std::filesystem::path &new_root, const std::string &game);
// Host ms per frame each guest thread ran since the previous call (threads
// above 0.05 ms), as "name ms, ...".
[[nodiscard]] std::string thread_time_report(double frames);
// Walks the display lists on another core from now on (see burnout_hle.cpp,
// "GE front end"); needs the renderer. ge_front_drain waits until it is idle.
bool ge_front_start(psprecomp::Runtime &runtime);
void ge_front_drain();

} // namespace burnout
