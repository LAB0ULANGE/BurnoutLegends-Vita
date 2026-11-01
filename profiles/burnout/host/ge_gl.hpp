#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace burnout::gl {

// GE display lists translated to OpenGL ES 2 (vitaGL on the PS Vita, desktop
// GL on Windows for validation). Vertices are decoded on the CPU (with PSP
// lighting and texture-coordinate generation), transformed by the vertex
// shader, and consecutive PRIMs that share all state are drawn as one batch.
// Every PSP render target becomes a framebuffer object; a texture whose
// address is a render target samples that object.

bool initialize(std::string &error);

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset);
// GE block transfer just copied guest memory into [address, address + size).
void invalidate_textures(std::uint32_t address, std::uint32_t size);

// sceDisplaySetFrameBuf: flushes pending draws and shows `address` on the
// Vita screen.
void present(std::uint32_t address);
// RGB888 copy of the 480x272 visible part of the render target at `address`.
bool read_frame(std::uint32_t address, std::vector<std::uint8_t> &rgb);

struct Stats {
    std::uint64_t primitives{}, batches{}, vertices{}, skipped_primitives{};
    std::uint64_t texture_uploads{}, render_targets{}, frames{};
};
[[nodiscard]] const Stats &stats() noexcept;

} // namespace burnout::gl
