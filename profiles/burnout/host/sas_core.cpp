#include "sas_core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>

namespace burnout::sas {

namespace {

// Envelope curve modes, as sceSasSetADSRmode numbers them.
enum Curve : std::uint8_t { LinearIncrease, LinearDecrease, LinearBent, ExponentDecrease, ExponentIncrease, Direct };
enum State : std::uint8_t { Off, Attack, Decay, Sustain, Release };

struct Envelope {
    std::int32_t height{};
    State state{Off};
    std::int32_t attack_rate{kEnvelopeMax}, decay_rate{}, sustain_rate{}, release_rate{0x08000000};
    std::int32_t sustain_level{kEnvelopeMax};
    Curve attack{LinearIncrease}, decay{ExponentDecrease}, sustain{LinearIncrease}, release{LinearDecrease};
};

struct Voice {
    const std::uint8_t *vag{};
    std::uint32_t size{};
    bool loop{}, noise{}, playing{};
    std::uint32_t pitch{0x1000u};
    std::int32_t left{0x1000}, right{0x1000};
    Envelope envelope;
    // VAG decoder: blocks of 16 bytes give 28 samples.
    std::uint32_t block{};
    std::int32_t loop_block{-1};
    std::array<std::int16_t, 28> samples{};
    std::uint32_t sample_at{}, sample_count{};
    std::int32_t history1{}, history2{};
    bool stream_end{}, end_after_block{};
    // Linear interpolation between `current` and `next`, `fraction` in 1/4096.
    std::int32_t current{}, next{};
    std::uint32_t fraction{};
    std::uint32_t noise_state{1u};
};

std::array<Voice, kVoices> g_voices;
std::uint32_t g_grain{256u};
std::vector<std::int32_t> g_mix;

bool valid(int voice) { return voice >= 0 && voice < kVoices; }

bool decode_block(Voice &v) {
    if (v.stream_end || v.end_after_block || v.vag == nullptr || (v.block + 1u) * 16u > v.size) return false;
    const std::uint8_t *b = v.vag + v.block * 16u;
    const std::uint8_t flags = b[1];
    if (flags == 7u) return false;  // end marker block
    static constexpr std::int32_t kCoefficients[5][2]{{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    std::int32_t shift = b[0] & 0xF;
    if (shift > 12) shift = 9;
    const std::int32_t *c = kCoefficients[std::min<std::uint32_t>((b[0] >> 4u) & 0xFu, 4u)];
    if ((flags & 4u) != 0u) v.loop_block = static_cast<std::int32_t>(v.block);
    for (std::uint32_t i = 0; i < 14u; ++i) {
        const std::uint8_t d = b[2u + i];
        for (std::uint32_t half = 0; half < 2u; ++half) {
            const std::int32_t nibble = half == 0u ? (d & 0xF) : (d >> 4u);
            std::int32_t s = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
            s += (v.history1 * c[0] + v.history2 * c[1]) >> 6;
            s = std::clamp(s, -32768, 32767);
            v.history2 = v.history1;
            v.history1 = s;
            v.samples[i * 2u + half] = static_cast<std::int16_t>(s);
        }
    }
    v.sample_at = 0u;
    v.sample_count = 28u;
    ++v.block;
    if ((flags & 1u) != 0u) {  // loop end
        if (v.loop && v.loop_block >= 0) v.block = static_cast<std::uint32_t>(v.loop_block);
        else v.end_after_block = true;
    }
    return true;
}

std::int32_t next_sample(Voice &v) {
    if (v.noise) {
        v.noise_state = v.noise_state * 1103515245u + 12345u;
        return static_cast<std::int16_t>(v.noise_state >> 16u);
    }
    if (v.sample_at >= v.sample_count && !decode_block(v)) {
        v.stream_end = true;
        return 0;
    }
    return v.samples[v.sample_at++];
}

void walk(std::int32_t &height, Curve curve, std::int32_t rate) {
    std::int64_t h = height;
    switch (curve) {
    case LinearIncrease: h += rate; break;
    case LinearDecrease: h -= rate; break;
    case LinearBent: h += h < (kEnvelopeMax / 4) * 3 ? rate : rate / 4; break;
    case ExponentDecrease: {
        const std::int64_t step = (h * static_cast<std::uint32_t>(rate)) >> 32;
        h -= rate != 0 ? std::max<std::int64_t>(step, 1) : 0;
        break;
    }
    case ExponentIncrease: {
        const std::int64_t step = ((kEnvelopeMax - h) * static_cast<std::uint32_t>(rate)) >> 32;
        h += rate != 0 ? std::max<std::int64_t>(step, 1) : 0;
        break;
    }
    case Direct: h = rate; break;
    }
    height = static_cast<std::int32_t>(std::clamp<std::int64_t>(h, -1, kEnvelopeMax + 1ll));
}

void step(Voice &v) {
    Envelope &e = v.envelope;
    switch (e.state) {
    case Off: break;
    case Attack:
        walk(e.height, e.attack, e.attack_rate);
        if (e.height >= kEnvelopeMax) {
            e.height = kEnvelopeMax;
            e.state = Decay;
        }
        break;
    case Decay:
        walk(e.height, e.decay, e.decay_rate);
        if (e.height <= e.sustain_level) {
            e.height = std::max(e.sustain_level, 0);
            e.state = Sustain;
        }
        break;
    case Sustain:
        walk(e.height, e.sustain, e.sustain_rate);
        e.height = std::clamp(e.height, 0, kEnvelopeMax);
        break;
    case Release:
        walk(e.height, e.release, e.release_rate);
        if (e.height <= 0) {
            e.height = 0;
            e.state = Off;
            v.playing = false;
        }
        break;
    }
}

// sceSasSetSimpleADSR bit fields (the SPU's ADSR registers).
std::int32_t simple_rate(std::uint32_t n) {
    n &= 0x7Fu;
    if (n == 0x7Fu) return 0;
    const std::int32_t rate = static_cast<std::int32_t>(((7u - (n & 3u)) << 26u) >> (n >> 2u));
    return rate == 0 ? 1 : rate;
}

} // namespace

void init(std::uint32_t grain) {
    g_grain = std::clamp<std::uint32_t>(grain, 64u, 2048u);
    g_voices = {};
}

std::uint32_t grain() noexcept { return g_grain; }

void set_voice(int voice, const std::uint8_t *vag, std::uint32_t size, bool loop) {
    if (!valid(voice)) return;
    Voice &v = g_voices[voice];
    v.vag = vag;
    v.size = size;
    v.loop = loop;
    v.noise = false;
}

void set_noise(int voice, std::uint32_t frequency) {
    if (!valid(voice)) return;
    (void)frequency;
    g_voices[voice].noise = true;
}

void set_pitch(int voice, std::uint32_t pitch) {
    if (valid(voice)) g_voices[voice].pitch = std::min<std::uint32_t>(pitch, 0x4000u);
}

void set_volume(int voice, std::int32_t left, std::int32_t right) {
    if (!valid(voice)) return;
    g_voices[voice].left = std::clamp(left, -0x1000, 0x1000);
    g_voices[voice].right = std::clamp(right, -0x1000, 0x1000);
}

void set_adsr(int voice, std::uint32_t flags, std::int32_t attack, std::int32_t decay, std::int32_t sustain,
              std::int32_t release) {
    if (!valid(voice)) return;
    Envelope &e = g_voices[voice].envelope;
    if ((flags & 1u) != 0u) e.attack_rate = attack;
    if ((flags & 2u) != 0u) e.decay_rate = decay;
    if ((flags & 4u) != 0u) e.sustain_rate = sustain;
    if ((flags & 8u) != 0u) e.release_rate = release;
}

void set_adsr_mode(int voice, std::uint32_t flags, std::uint32_t attack, std::uint32_t decay, std::uint32_t sustain,
                   std::uint32_t release) {
    if (!valid(voice)) return;
    Envelope &e = g_voices[voice].envelope;
    const auto curve = [](std::uint32_t mode) { return static_cast<Curve>(std::min<std::uint32_t>(mode, Direct)); };
    if ((flags & 1u) != 0u) e.attack = curve(attack);
    if ((flags & 2u) != 0u) e.decay = curve(decay);
    if ((flags & 4u) != 0u) e.sustain = curve(sustain);
    if ((flags & 8u) != 0u) e.release = curve(release);
}

void set_sustain_level(int voice, std::int32_t level) {
    if (valid(voice)) g_voices[voice].envelope.sustain_level = level;
}

void set_simple_adsr(int voice, std::uint32_t env1, std::uint32_t env2) {
    if (!valid(voice)) return;
    Envelope &e = g_voices[voice].envelope;
    e.attack_rate = simple_rate(env1 >> 8u);
    e.attack = (env1 & 0x8000u) == 0u ? LinearIncrease : LinearBent;
    const std::uint32_t decay = (env1 >> 4u) & 0xFu;
    e.decay_rate = decay == 0u ? 0x7FFFFFFF : static_cast<std::int32_t>(0x80000000u >> decay);
    e.decay = ExponentDecrease;
    e.sustain_level = static_cast<std::int32_t>(((env1 & 0xFu) + 1u) << 26u);
    e.sustain_rate = simple_rate(env2 >> 6u);
    switch ((env2 >> 13u) & 7u) {
    case 0u: e.sustain = LinearIncrease; break;
    case 2u: e.sustain = LinearDecrease; break;
    case 4u: e.sustain = LinearBent; break;
    default: e.sustain = ExponentDecrease; break;
    }
    const std::uint32_t release = env2 & 0x1Fu;
    e.release = (env2 & 0x20u) == 0u ? LinearDecrease : ExponentDecrease;
    if (release == 31u) e.release_rate = 0;
    else if (e.release == LinearDecrease) e.release_rate = release == 30u ? 0x40000000 : release == 29u ? 1 : static_cast<std::int32_t>(0x10000000u >> release);
    else e.release_rate = release == 0u ? 0x7FFFFFFF : static_cast<std::int32_t>(0x80000000u >> release);
}

void key_on(int voice) {
    if (!valid(voice)) return;
    static const bool trace = std::getenv("PSPRECOMP_BURNOUT_SASTRACE") != nullptr;
    static int traced = 0;
    if (trace && traced++ < 60) {
        const Voice &t = g_voices[voice];
        std::fprintf(stderr, "[sas] key on %d vag %p size %u loop %d noise %d pitch %X vol %d/%d adsr a%X/%d d%X s%X/%d r%X sl%X\n", voice,
                     static_cast<const void *>(t.vag), t.size, t.loop, t.noise, t.pitch, t.left, t.right, t.envelope.attack_rate,
                     t.envelope.attack, t.envelope.decay_rate, t.envelope.sustain_rate, t.envelope.sustain, t.envelope.release_rate,
                     t.envelope.sustain_level);
    }
    Voice &v = g_voices[voice];
    if (!v.noise && v.vag == nullptr) return;
    v.block = 0u;
    v.loop_block = -1;
    v.sample_at = v.sample_count = 0u;
    v.history1 = v.history2 = 0;
    v.stream_end = v.end_after_block = false;
    v.fraction = 0u;
    v.envelope.height = 0;
    v.envelope.state = Attack;
    v.playing = true;
    v.current = next_sample(v);
    v.next = next_sample(v);
}

void key_off(int voice) {
    if (!valid(voice)) return;
    Envelope &e = g_voices[voice].envelope;
    if (e.state != Off) e.state = Release;
}

std::uint32_t end_flags() {
    std::uint32_t flags = 0u;
    for (int i = 0; i < kVoices; ++i)
        if (!g_voices[i].playing) flags |= 1u << i;
    return flags;
}

std::int32_t envelope_height(int voice) { return valid(voice) ? std::max(g_voices[voice].envelope.height, 0) : 0; }

void mix(std::int16_t *out) {
    g_mix.assign(static_cast<std::size_t>(g_grain) * 2u, 0);
    for (Voice &v : g_voices) {
        if (!v.playing) continue;
        // Sustaining at rate 0, the envelope stops moving once a step has
        // clamped it: the per-sample walk is skipped from there on (most
        // looped voices spend their life there, and the walk dominates on
        // the Vita).
        bool settled = false;
        for (std::uint32_t i = 0; i < g_grain; ++i) {
            const std::int32_t sample = v.current + (((v.next - v.current) * static_cast<std::int32_t>(v.fraction)) >> 12);
            if (!settled) {
                step(v);
                settled = v.envelope.state == Sustain && v.envelope.sustain_rate == 0;
            }
            const std::int32_t shaped = (sample * (std::max(v.envelope.height, 0) >> 15)) >> 15;
            g_mix[i * 2u] += (shaped * v.left) >> 12;
            g_mix[i * 2u + 1u] += (shaped * v.right) >> 12;
            v.fraction += v.pitch;
            while (v.fraction >= 0x1000u) {
                v.fraction -= 0x1000u;
                v.current = v.next;
                v.next = next_sample(v);
            }
            if (v.stream_end && v.sample_at >= v.sample_count) {
                // The sample ran out: the voice ends as the PSP's does.
                v.playing = false;
                v.envelope.state = Off;
                v.envelope.height = 0;
                break;
            }
            if (!v.playing) break;
        }
    }
    for (std::uint32_t i = 0; i < g_grain * 2u; ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(g_mix[i], -32768, 32767));
}

} // namespace burnout::sas
