#pragma once

#include <cstdint>
#include <string>

// Host sound output for the PSP audio channels (sceAudioOutput*). Each guest
// channel feeds its own FIFO; on the Vita an output thread mixes them into
// one 44.1 kHz stereo port. Off by default: the headless PC build stays
// silent (and bit-exact) unless asked to dump the channels.
namespace burnout::audio {

// Vita: opens the output port and starts the mixing thread on `core`.
// PC: `dump_dir` non-empty writes each channel as raw s16 stereo there.
bool start(int core, const std::string &dump_dir, std::string &error);
void stop();
[[nodiscard]] bool enabled() noexcept;

// One output call of a guest channel: `frames` samples, interleaved stereo
// or mono, PSP volumes (0x8000 = unity).
void submit(std::uint32_t channel, const std::int16_t *samples, std::uint32_t frames, bool stereo, std::uint32_t left,
            std::uint32_t right);

// Diagnostics: frames of silence the output played because no channel had
// data (underruns), frames dropped from full FIFOs.
struct Counters {
    std::uint64_t underrun_frames{}, dropped_frames{}, played_frames{};
};
[[nodiscard]] Counters counters();

} // namespace burnout::audio
