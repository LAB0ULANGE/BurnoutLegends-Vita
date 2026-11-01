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

// What the worker is doing, for a sampling thread (bring-up profiling):
// 0 other, 1-5 PRIM (setup, indices, checks, vertex copy, index emit),
// 6-10 draw (state, texture, state, attributes, glDrawElements), 11 present,
// 12 waiting for work, 13 block transfer.
extern volatile std::uint32_t phase;
// Same for the emulation thread: 0 game code, 1 ge_scan, 2 render hook,
// 3 PRIM index snapshot, 4 vertex snapshot, 5 palette snapshot, 6 block
// transfer snapshot, 7 waiting for the worker.
extern volatile std::uint32_t emulation_phase;
// One line in the log, flushed at once (bring-up on the Vita).
void trace(const char *message);

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset);

// Guest bytes a command list reads, copied on the emulation thread: records
// (PRIM indices, LOAD_CLUT palettes) and PRIM vertices. Vertices are kept
// apart so consecutive PRIMs stay contiguous and can be drawn in place.
struct Snapshot {
    std::vector<std::uint8_t> records, vertices;
};

// Emulation-thread side of the GL worker. Follows the vertex and index
// pointers and appends to `snapshot` the guest bytes `word` will read, so the
// worker can draw the frame while the game overwrites them. Returns the
// record's offset in `snapshot.records`, or UINT32_MAX when the command reads
// no guest memory.
std::uint32_t snapshot_command(const psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base,
                               std::uint32_t offset, Snapshot &snapshot);
// The next ge_command reads `record` of `snapshot` instead of guest memory;
// nullptr goes back to guest memory.
void use_snapshot(const Snapshot *snapshot, std::uint32_t record);
// The command list ends and its snapshot goes away: draws what still points
// into it.
void end_list();
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
