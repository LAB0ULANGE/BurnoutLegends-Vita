#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace burnout::gl {

// GE display lists translated to OpenGL ES 2 (vitaGL on the PS Vita, desktop
// GL on Windows for validation). Vertices keep their PSP formats; the vertex
// shader converts them and does the transform, PSP lighting and texture-
// coordinate generation. Consecutive PRIMs that share all state are drawn as
// one batch.
// Every PSP render target becomes a framebuffer object; a texture whose
// address is a render target samples that object.

bool initialize(std::string &error);
// One line in the log, flushed at once (bring-up on the Vita).
void trace(const char *message);

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset);

// Emulation-thread side of the GL worker. Follows the vertex and index
// pointers and appends to `arena` the guest bytes `word` will read (PRIM
// vertices and indices, LOAD_CLUT palette), so the worker can draw the frame
// while the game overwrites them. Returns the record's offset in `arena`, or
// UINT32_MAX when the command reads no guest memory.
std::uint32_t snapshot_command(const psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base,
                               std::uint32_t offset, std::vector<std::uint8_t> &arena);
// The next ge_command reads `record` (from snapshot_command) instead of guest
// memory; nullptr goes back to guest memory.
void use_snapshot(const std::uint8_t *record);
// GE block transfer just copied guest memory into [address, address + size).
void invalidate_textures(std::uint32_t address, std::uint32_t size);

// sceDisplaySetFrameBuf: flushes pending draws and shows `address` on the
// Vita screen.
void present(std::uint32_t address);
// RGB888 copy of the 480x272 visible part of the render target at `address`.
bool read_frame(std::uint32_t address, std::vector<std::uint8_t> &rgb);

struct Stats {
    std::uint64_t primitives{}, batches{}, vertices{}, skipped_primitives{}, lit_vertices{};
    std::uint64_t texture_uploads{}, render_targets{}, frames{};
    // Reading the clock is a system call on the Vita, so it is not done per
    // PRIM: list_ms covers whole command lists (decode plus draw), draw_ms
    // the GL submission inside them.
    double list_ms{}, draw_ms{}, present_ms{}, idle_ms{};
};
[[nodiscard]] const Stats &stats() noexcept;
// Time the caller spent on a command list / waiting for work (for stats()).
void account(double list_ms, double idle_ms) noexcept;

} // namespace burnout::gl
