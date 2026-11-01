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

} // namespace burnout
