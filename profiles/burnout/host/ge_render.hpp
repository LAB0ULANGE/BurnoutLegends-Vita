#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

namespace burnout::render {

// GE rendering: ge_scan feeds every display-list command here. The Software
// backend (PC only) is the VCS profile's rasterizer drawing into guest VRAM;
// the OpenGL backend (PC and Vita) is ge_gl. Either one only
// observes the command stream (control flow and callbacks stay in ge_scan), so
// the guest runs exactly as in the headless build.

enum class Backend { Software, OpenGL };

struct Options {
    bool enabled{};
    Backend backend{Backend::Software};
    // Displayed frame buffers written as PNG at the first flip on or after each
    // of these vblanks.
    std::set<std::uint64_t> capture_vblanks;
    std::filesystem::path capture_dir;
    // Logs every PRIM drawn between these vblanks (inclusive) to stderr.
    std::uint64_t trace_from{UINT64_MAX};
    std::uint64_t trace_to{0u};
};

void configure(const Options &options);
[[nodiscard]] bool active() noexcept;
// Emulation-thread time spent walking display lists (ge_scan with the hooks).
void account_scan(double ms) noexcept;
// Waits until everything queued has been drawn. Called before guest memory
// goes away: the GL thread reads vertices from it.
void finish();

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t word, std::uint32_t base,
                std::uint32_t offset);
// sceDisplaySetFrameBuf: the frame at `address` is about to be shown.
void frame_buffer_set(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stride,
                      std::uint32_t format, std::uint64_t vblank);

[[nodiscard]] std::string report();

} // namespace burnout::render
