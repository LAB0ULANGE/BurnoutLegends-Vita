#pragma once

#include <cstdint>

// The PSP's SAS software synthesizer (sceSasCore): up to 32 voices playing
// VAG ADPCM samples or noise, each with pitch, an ADSR envelope and a stereo
// volume, mixed into the grain the game hands to sceAudioOutput. Reverb is
// not modelled (the dry signal only).
namespace burnout::sas {

constexpr int kVoices = 32;
constexpr std::int32_t kEnvelopeMax = 0x40000000;

void init(std::uint32_t grain);

// `vag`: host pointer to the guest ADPCM data (no VAG header), `size` bytes.
void set_voice(int voice, const std::uint8_t *vag, std::uint32_t size, bool loop);
void set_noise(int voice, std::uint32_t frequency);
void set_pitch(int voice, std::uint32_t pitch);  // 0x1000 = 44.1 kHz
void set_volume(int voice, std::int32_t left, std::int32_t right);  // 0x1000 = unity
void set_adsr(int voice, std::uint32_t flags, std::int32_t attack, std::int32_t decay, std::int32_t sustain,
              std::int32_t release);
void set_adsr_mode(int voice, std::uint32_t flags, std::uint32_t attack, std::uint32_t decay, std::uint32_t sustain,
                   std::uint32_t release);
void set_sustain_level(int voice, std::int32_t level);
void set_simple_adsr(int voice, std::uint32_t env1, std::uint32_t env2);
void key_on(int voice);
void key_off(int voice);

[[nodiscard]] std::uint32_t end_flags();  // bit per voice that is not playing
[[nodiscard]] std::int32_t envelope_height(int voice);

// Renders one grain of interleaved stereo samples.
void mix(std::int16_t *out);
[[nodiscard]] std::uint32_t grain() noexcept;

} // namespace burnout::sas
