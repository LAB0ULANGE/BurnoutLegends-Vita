#pragma once

#include <cstdint>

namespace psprecomp {
class GuestMemory;
}

// Extra car paints (Burnout_Legends_Recompenses_2.xlsx): each Takedown Trophy
// objective done gives one, offered after the game's own colours under COLOR
// in car select. A car's livery texture is 8-bit indexed and each of its
// colours is one of the palettes after its texels; a paint is the first
// palette with the body recoloured (decals, glass and wheels kept), written
// over that palette while the paint is chosen (the original put back after).
namespace burnout::paints {

// 20 paints (one per Takedown Trophy objective), then 4 liveries (one per
// whole trophy: Easy, Advanced, Expert, Champion).
constexpr int kObjectivePaints = 20;
constexpr int kLiveries = 4;
constexpr int kCount = kObjectivePaints + kLiveries;

struct Paint {
    const char *name;
    std::uint8_t r, g, b;
    // Liveries: a pattern drawn on the body's texels in two tones (ramp[0]
    // and ramp[1], the body's shading kept).
    enum class Kind : std::uint8_t { Solid, Carbon, Stripes, Flames, Criterion } kind{Kind::Solid};
    std::uint8_t ramp[2][3]{};
};
const Paint &paint(int index);

// A palette (256 entries, 8888) recoloured with `paint`; `counts` are the
// texels using each entry (the body is the most used saturated hue).
void recolour(const std::uint8_t *palette, const std::uint32_t *counts, const Paint &paint, std::uint8_t *out);

// COLOR pressed in car select (tools/patch_paints.py): the game's next colour
// is `native` (its `next` modulo the `count` colours it offers); the colour
// the game takes instead (0 while one of our paints is shown).
std::uint32_t next_colour(psprecomp::GuestMemory &memory, std::uint32_t next, std::uint32_t count, std::uint32_t native);

// Once per vblank (emulation thread): the chosen paint kept on the player's car.
void update(psprecomp::GuestMemory &memory, bool enabled);

// Cars with 8 colours: kBaseColours from the start, then one per Takedown
// Trophy won and one more with all four (kTrophyColours in all).
constexpr int kBaseColours = 3;
constexpr int kTrophyColours = 5;
// The colours the trophies won unlock (0..kTrophyColours).
int unlocked_colours(psprecomp::GuestMemory &memory);
// Every trophy colour offered whatever the trophies (PC settings: all_colours).
void set_all_unlocked(bool all);

// The paint chosen, -1: the game's colours (set at launch from unlocks.bin).
int chosen();
// The next paint won after `after` (-1: the first), -1 past the last.
int next_won(int after);
void set_chosen(int paint);

// Told when a livery rewrites a car texture's texels in guest memory (the
// renderer then uploads it again at once).
void set_texels_written(void (*written)(std::uint32_t address, std::uint32_t bytes));

} // namespace burnout::paints
