#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>

namespace burnout::workload {

// Read-only statistics on what a renderer and an audio backend would have to
// process. Nothing here changes guest state; with collection off every hook is a
// single branch.

void set_collecting(bool enabled) noexcept;  // collect at all (off by default)
void set_window_open(bool open) noexcept;    // only the measured window is recorded
[[nodiscard]] bool collecting() noexcept;

// One GE command as ge_scan walks it. base/offset are the list's BASE and
// OFFSET_ADDR registers at that point.
void ge_command(const psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t word, std::uint32_t base,
                std::uint32_t offset);
void ge_list_submitted();
void end_frame(const psprecomp::GuestMemory &memory);  // sceDisplaySetFrameBuf

void sas_core(std::uint64_t time_us);
void sas_set_voice(std::uint32_t voice, std::uint32_t size, bool loop);
void sas_set_pitch(std::uint32_t voice, std::uint32_t pitch);
void sas_key_on(std::uint32_t voice, std::uint64_t time_us);
void sas_key_off(std::uint32_t voice);
void atrac_set_data(const psprecomp::GuestMemory &memory, std::uint32_t buffer, std::uint32_t size);
void atrac_decode();

[[nodiscard]] std::string report();

} // namespace burnout::workload
