#include "paints.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace burnout::paints {
namespace {

// In the spreadsheet's order: Easy, Advanced, Expert, Champion objectives
// (the game's order too, profile progress +90).
constexpr Paint kPaints[kObjectivePaints + kLiveries]{
    {"ROSE FLUO", 255, 20, 147},      {"VERT LIME", 127, 255, 0},       {"VIOLET", 138, 43, 226},
    {"ORANGE FLUO", 255, 100, 0},     {"TURQUOISE", 48, 213, 200},      {"ARGENT METALLISE", 192, 192, 200},
    {"BRONZE", 205, 127, 50},         {"BLEU NUIT", 25, 35, 110},       {"VERT BRITISH", 0, 66, 37},
    {"BORDEAUX", 109, 7, 26},         {"NOIR MAT", 20, 20, 20},         {"BLANC NACRE", 244, 241, 232},
    {"CHROME", 216, 220, 224},        {"OR", 212, 175, 55},             {"GUNMETAL", 42, 52, 57},
    {"ROUGE TAKEDOWN", 209, 0, 0},    {"BLEU ELECTRIQUE", 0, 102, 255}, {"JAUNE RACING", 255, 215, 0},
    {"VERT NEON", 57, 255, 20},       {"BLANC PUR", 255, 255, 255},
    // The trophies' liveries (two tones and a pattern).
    {"CRITERION", 255, 110, 20, Paint::Kind::Criterion, {{255, 100, 10}, {245, 245, 245}}},
    {"TAKEDOWN", 220, 0, 0, Paint::Kind::Stripes, {{215, 0, 0}, {12, 12, 12}}},
    {"FLAMMES", 255, 90, 0, Paint::Kind::Flames, {{14, 14, 14}, {255, 110, 0}}},
    {"CARBONE", 40, 40, 44, Paint::Kind::Carbon, {{26, 26, 30}, {70, 72, 80}}},
};

struct Hsv {
    float h, s, v;  // h in [0, 1)
};

Hsv to_hsv(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float d = mx - mn;
    Hsv out{0.0f, mx > 0.0f ? d / mx : 0.0f, mx};
    if (d <= 0.0f) return out;
    float h;
    if (mx == r) h = (g - b) / d;
    else if (mx == g) h = 2.0f + (b - r) / d;
    else h = 4.0f + (r - g) / d;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
    out.h = h;
    return out;
}

void to_rgb(Hsv c, float &r, float &g, float &b) {
    const float h = c.h * 6.0f;
    const int i = static_cast<int>(std::floor(h)) % 6;
    const float f = h - std::floor(h);
    const float p = c.v * (1.0f - c.s), q = c.v * (1.0f - c.s * f), t = c.v * (1.0f - c.s * (1.0f - f));
    switch (i) {
    case 0: r = c.v; g = t; b = p; break;
    case 1: r = q; g = c.v; b = p; break;
    case 2: r = p; g = c.v; b = t; break;
    case 3: r = p; g = q; b = c.v; break;
    case 4: r = t; g = p; b = c.v; break;
    default: r = c.v; g = p; b = q; break;
    }
}

float hue_distance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 1.0f - d);
}

Hsv entry(const std::uint8_t *palette, int i) {
    return to_hsv(palette[i * 4] / 255.0f, palette[i * 4 + 1] / 255.0f, palette[i * 4 + 2] / 255.0f);
}


// The body's entries (saturated, of its hue) sorted by value: tone_of[e] =
// rank & 1; partner[e] = the entry of the other tone nearest in value.
void body_entries(const std::uint8_t *palette, float body, std::uint8_t *tone_of, std::int16_t *partner) {
    std::vector<std::pair<float, int>> entries;
    for (int i = 0; i < 256; ++i) {
        const Hsv c = entry(palette, i);
        if (c.s > 0.35f && hue_distance(c.h, body) < 0.08f) entries.push_back({c.v, i});
    }
    std::sort(entries.begin(), entries.end());
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const int e = entries[k].second;
        if (tone_of != nullptr) tone_of[e] = static_cast<std::uint8_t>(k & 1u);
        if (partner != nullptr) {
            const std::size_t other = k + 1 < entries.size() ? k + 1 : (k > 0 ? k - 1 : k);
            partner[e] = static_cast<std::int16_t>(entries[other].second);
        }
    }
}

float body_hue(const std::uint8_t *palette, const std::uint32_t *counts, double *weight = nullptr) {
    double bins[36]{};
    for (int i = 0; i < 256; ++i) {
        const Hsv c = entry(palette, i);
        if (c.s > 0.45f && c.v > 0.08f) bins[static_cast<int>(c.h * 36.0f) % 36] += counts[i];
    }
    int best = 0;
    double best_weight = -1.0;
    for (int b = 0; b < 36; ++b) {
        const double w = bins[b] + 0.5 * (bins[(b + 35) % 36] + bins[(b + 1) % 36]);
        if (w > best_weight) {
            best_weight = w;
            best = b;
        }
    }
    if (weight != nullptr) *weight = best_weight;
    return best_weight <= 0.0 ? -1.0f : (static_cast<float>(best) + 0.5f) / 36.0f;
}

// The pattern's tone at texel (x, y) of the livery texture.
int pattern_tone(Paint::Kind kind, std::uint32_t x, std::uint32_t y) {
    switch (kind) {
    case Paint::Kind::Carbon: return static_cast<int>(((x >> 1u) + (y >> 1u)) & 1u);  // a fine weave
    case Paint::Kind::Stripes: return ((x + y) / 12u) % 3u == 0u ? 1 : 0;             // diagonal black bands
    case Paint::Kind::Criterion: return (x % 32u) < 5u || (y % 64u) < 4u ? 1 : 0;    // white pinstripes
    case Paint::Kind::Flames: {
        // Tongues of fire rising from the bottom of each 64-texel band.
        const float fy = static_cast<float>(y % 64u) / 64.0f;
        const float wave = 0.55f + 0.25f * std::sin(static_cast<float>(x) * 0.21f) + 0.12f * std::sin(static_cast<float>(x) * 0.53f + 1.7f);
        return fy > 1.0f - wave ? 1 : 0;
    }
    default: return 0;
    }
}

} // namespace

namespace {
void (*g_texels_written)(std::uint32_t, std::uint32_t){};
} // namespace

void set_texels_written(void (*written)(std::uint32_t address, std::uint32_t bytes)) { g_texels_written = written; }


const Paint &paint(int index) { return kPaints[std::clamp(index, 0, kCount - 1)]; }

void recolour(const std::uint8_t *palette, const std::uint32_t *counts, const Paint &paint, std::uint8_t *out) {
    std::memcpy(out, palette, 1024u);
    // The body's hue: the most used saturated entries (36 bins, neighbours count half).
    double bins[36]{};
    for (int i = 0; i < 256; ++i) {
        const Hsv c = entry(palette, i);
        if (c.s > 0.45f && c.v > 0.08f) bins[static_cast<int>(c.h * 36.0f) % 36] += counts[i];
    }
    int best = 0;
    double best_weight = -1.0;
    for (int b = 0; b < 36; ++b) {
        const double w = bins[b] + 0.5 * (bins[(b + 35) % 36] + bins[(b + 1) % 36]);
        if (w > best_weight) {
            best_weight = w;
            best = b;
        }
    }
    if (best_weight <= 0.0) return;  // no coloured body: left as it is
    const float body = (static_cast<float>(best) + 0.5f) / 36.0f;
    // The body's bright value (90th percentile of its texels).
    std::vector<std::pair<float, std::uint32_t>> values;
    std::uint64_t total = 0;
    for (int i = 0; i < 256; ++i) {
        const Hsv c = entry(palette, i);
        if (c.s > 0.45f && hue_distance(c.h, body) < 0.08f && counts[i] != 0u) {
            values.push_back({c.v, counts[i]});
            total += counts[i];
        }
    }
    std::sort(values.begin(), values.end());
    float reference = 1.0f;
    std::uint64_t seen = 0;
    for (const auto &[v, n] : values) {
        seen += n;
        reference = v;
        if (seen * 10u >= total * 9u) break;
    }
    if (reference <= 0.0f) return;
    const Hsv target = to_hsv(paint.r / 255.0f, paint.g / 255.0f, paint.b / 255.0f);
    // Liveries: the body entries by value, alternately tone 0 and tone 1
    // (each texel then takes the entry of its tone nearest its shade).
    std::uint8_t tone_of[256]{};
    body_entries(palette, body, tone_of, nullptr);
    for (int i = 0; i < 256; ++i) {
        const Hsv c = entry(palette, i);
        if (!(c.s > 0.35f && hue_distance(c.h, body) < 0.08f)) continue;
        // The entry's shade of the body, in the paint (the game's palettes are
        // stored at about half brightness).
        const float shade = c.v / reference;
        // Much brighter than the body's bright texels: a light (the tail
        // lights of a red car share its hue; 0.0.58 painted them lime).
        if (shade > 1.3f && c.s > 0.6f) continue;
        float r, g, b;
        if (paint.kind != Paint::Kind::Solid) {
            // Tone by the entry's place among the body's shades (tone_of).
            const std::uint8_t *tone = paint.ramp[tone_of[i]];
            const float k = 0.45f * std::min(1.2f, shade) / 255.0f;
            r = std::min(1.0f, tone[0] * k);
            g = std::min(1.0f, tone[1] * k);
            b = std::min(1.0f, tone[2] * k);
        } else if (paint.kind == Paint::Kind::Solid) {
            Hsv n{target.h, target.s * std::min(1.0f, c.s / 0.9f), std::min(1.0f, shade * target.v * 0.45f)};
            to_rgb(n, r, g, b);
        } else {
            // The shade (0..1 from the darkest to the reference) along the
            // ramp; carbon: every other entry from the dark stop.
            const float t = std::clamp((shade - 0.55f) / 0.45f, 0.0f, 1.0f);
            const int lo = t < 0.5f ? 0 : 1;
            float f = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
            if (paint.kind == Paint::Kind::Carbon) f = (i & 1) != 0 ? f * 0.35f : f;
            float rgb[3];
            for (int k = 0; k < 3; ++k)
                rgb[k] = (paint.ramp[lo][k] + (paint.ramp[lo + 1][k] - paint.ramp[lo][k]) * f) / 255.0f * 0.45f;
            r = std::min(1.0f, rgb[0]);
            g = std::min(1.0f, rgb[1]);
            b = std::min(1.0f, rgb[2]);
        }
        out[i * 4] = static_cast<std::uint8_t>(std::lround(r * 255.0f));
        out[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(g * 255.0f));
        out[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(b * 255.0f));
    }
}

namespace {

// The player's car (the game's car select code, 0x088F87E0): per-car records
// at 0x08D602B0 (9312 bytes each), +64 its model, the model's +96 its livery
// texture. Car select shows the record of the slot at 0x08D88B7E (-1 out of
// car select).
constexpr std::uint32_t kCars = 0x08D602B0u, kCarBytes = 9312u, kSelectSlot = 0x08D88B7Eu;
// In a race the racers' records (10192 bytes each, the same +64 model) start
// at 0x08D5B310, the player's first.
constexpr std::uint32_t kRacers = 0x08D5B310u;
// Livery texture: +8 its palettes, +0x10 width, +0x14 bits per texel, +0x38
// texels, +176 palette count, +177 palette shown.
// Trophy objectives done: 20 bytes in the progress struct (Driver Details).
constexpr std::uint32_t kObjectives = 0x08BBFDF0u + 90u;

psprecomp::GuestMemory *g_memory{};
bool g_enabled{};            // Bonus switched on (off: the game's colours only)
int g_chosen = -1;           // our paint shown or chosen, -1: none
std::uint32_t g_texture{};   // the livery we wrote over, 0: none
std::uint32_t g_palette{};   // its first palette
std::uint8_t g_original[1024]{};
std::uint8_t g_source[1024]{};  // the palette the paint is made from (see apply)
std::uint8_t g_written[1024]{};
int g_written_paint = -1;
std::uint32_t g_select_texture{};  // the car shown in car select (a change of car: back to the game's colours)
// The car the paint is on (its texture's name, "compact1"): another car shown
// outside car select - a car just unlocked on the awards screen - keeps its
// own colours (0.0.63 painted the Dominator Muscle's flames in Rose fluo).
// Empty after launch: the first car raced or shown in car select takes it.
char g_paint_car[17]{};

bool texture_name(std::uint32_t texture, char (&out)[17]) {
    const std::uint8_t *name = texture != 0u ? g_memory->raw_pointer(texture + 0x8Cu, 16u) : nullptr;
    if (name == nullptr) return false;
    std::memcpy(out, name, 16u);
    out[16] = '\0';
    return out[0] != '\0';
}

bool guest(std::uint32_t a) { return a >= 0x08800000u && a < 0x0A000000u; }
std::uint32_t read32(std::uint32_t a) {
    std::uint32_t v = 0u;
    if (const std::uint8_t *p = g_memory->raw_pointer(a, 4u)) std::memcpy(&v, p, 4u);
    return v;
}
std::uint8_t read8(std::uint32_t a) {
    const std::uint8_t *p = g_memory->raw_pointer(a, 1u);
    return p != nullptr ? *p : 0u;
}

bool unlocked(int paint) {
    static const bool all = std::getenv("PSPRECOMP_BURNOUT_ALL_PAINTS") != nullptr;  // PC tests
    if (all) return paint >= 0 && paint < kCount;
    const std::uint8_t *done = g_memory->raw_pointer(kObjectives, kObjectivePaints);
    if (done == nullptr || paint < 0 || paint >= kCount) return false;
    if (paint < kObjectivePaints) return done[paint] == 1u;
    // A livery: its trophy's five objectives.
    const int tier = paint - kObjectivePaints;
    for (int i = 0; i < 5; ++i)
        if (done[tier * 5 + i] != 1u) return false;
    return true;
}
int next_unlocked(int after) {
    for (int i = after + 1; i < kCount; ++i)
        if (unlocked(i)) return i;
    return -1;
}

// The player's livery texture, 0 if none (or not an 8-bit one with palettes).
std::uint32_t player_texture(bool &in_select) {
    const auto slot = static_cast<std::int8_t>(read8(kSelectSlot));
    in_select = slot >= 0 && slot < 8;
    const std::uint32_t car = in_select ? kCars + static_cast<std::uint32_t>(slot) * kCarBytes : kRacers;
    const std::uint32_t model = read32(car + 64u);
    if (!guest(model)) return 0u;
    const std::uint32_t texture = read32(model + 96u);
    if (!guest(texture)) return 0u;
    const std::uint32_t palettes = read32(texture + 8u), texels = read32(texture + 0x38u);
    const std::uint32_t width = read32(texture + 0x10u), bits = read32(texture + 0x14u);
    const std::uint32_t count = read8(texture + 176u);
    if (!guest(palettes) || !guest(texels) || bits != 8u || width < 16u || width > 512u || count == 0u || count > 16u ||
        palettes <= texels || palettes - texels > 512u * 512u)
        return 0u;
    // Its name (the car's: "compact1"): printable, so a stale record is not taken.
    const std::uint8_t *name = g_memory->raw_pointer(texture + 0x8Cu, 16u);
    if (name == nullptr || name[0] == 0u) return 0u;
    for (int i = 0; i < 16 && name[i] != 0u; ++i)
        if (name[i] < 0x20u || name[i] > 0x7Eu) return 0u;
    return texture;
}

// A livery's texels: the original kept to put back.
std::uint32_t g_texels{};
std::vector<std::uint8_t> g_texels_original, g_texels_drawn;

void restore_texels() {
    if (g_texels == 0u) return;
    std::uint8_t *t = g_memory->raw_pointer(g_texels, g_texels_drawn.size());
    if (t != nullptr && std::memcmp(t, g_texels_drawn.data(), g_texels_drawn.size()) == 0) {
        std::memcpy(t, g_texels_original.data(), g_texels_original.size());
        if (g_texels_written) g_texels_written(g_texels, static_cast<std::uint32_t>(g_texels_original.size()));
    }
    g_texels = 0u;
}

void restore() {
    if (g_texture == 0u) return;
    // Put back only over our own palette (the model may have been reloaded).
    if (std::uint8_t *p = g_memory->raw_pointer(g_palette, 1024u); p != nullptr && std::memcmp(p, g_written, 1024u) == 0)
        std::memcpy(p, g_original, 1024u);
    restore_texels();
    g_texture = 0u;
    g_written_paint = -1;
}

// The livery's pattern on the body texels (swizzled 16 x 8 blocks): each
// takes its pattern tone's entry nearest its shade.
void draw_texels(std::uint32_t texture, std::uint32_t texels, std::uint32_t bytes, const Paint &paint, float body) {
    std::uint8_t *t = g_memory->raw_pointer(texels, bytes);
    if (t == nullptr) return;
    if (g_texels != texels || g_texels_drawn.size() != bytes || std::memcmp(t, g_texels_drawn.data(), bytes) != 0) {
        g_texels_original.assign(t, t + bytes);  // what the game loaded
        g_texels = texels;
    }
    std::uint8_t tone_of[256]{};
    std::int16_t partner[256];
    for (auto &p : partner) p = -1;
    body_entries(g_source, body, tone_of, partner);
    const std::uint32_t width = read32(texture + 0x10u);
    const std::uint32_t blocks_per_row = std::max<std::uint32_t>(width / 16u, 1u);
    g_texels_drawn = g_texels_original;
    for (std::uint32_t off = 0; off < bytes; ++off) {
        const std::uint8_t e = g_texels_original[off];
        if (partner[e] < 0) continue;  // not the body
        const std::uint32_t block = off / 128u, within = off % 128u;
        const std::uint32_t x = (block % blocks_per_row) * 16u + within % 16u, y = (block / blocks_per_row) * 8u + within / 16u;
        const int want = pattern_tone(paint.kind, x, y);
        g_texels_drawn[off] = tone_of[e] == want ? e : static_cast<std::uint8_t>(partner[e]);
    }
    std::memcpy(t, g_texels_drawn.data(), bytes);
    if (g_texels_written) g_texels_written(texels, bytes);
}

void apply(std::uint32_t texture, int paint) {
    const std::uint32_t palette = read32(texture + 8u);
    std::uint8_t *p = g_memory->raw_pointer(palette, 1024u);
    if (p == nullptr) return;
    if (texture == g_texture && palette == g_palette && g_written_paint == paint && std::memcmp(p, g_written, 1024u) == 0) return;
    if (texture != g_texture || palette != g_palette || std::memcmp(p, g_written, 1024u) != 0) {
        // A new car, or the model loaded again: its palette is the original.
        std::memcpy(g_original, p, 1024u);
    }
    const std::uint32_t texels = read32(texture + 0x38u);
    const std::uint32_t bytes = palette - texels;
    const std::uint8_t *data = g_memory->raw_pointer(texels, bytes);
    if (data == nullptr) return;
    std::uint32_t counts[256]{};
    for (std::uint32_t i = 0; i < bytes; ++i) ++counts[data[i]];
    // The game's colour whose body is the most coloured: its first one may
    // be black or grey (Muscle Type 2), the body then has no hue to find and
    // only the trim took the paint. The palettes share the texels.
    std::memcpy(g_source, g_original, 1024u);
    double best = -1.0;
    body_hue(g_original, counts, &best);
    const std::uint32_t palettes = std::min<std::uint32_t>(read8(texture + 176u), 16u);
    for (std::uint32_t k = 1; k < palettes; ++k) {
        const std::uint8_t *other = g_memory->raw_pointer(palette + k * 1024u, 1024u);
        if (other == nullptr) break;
        double weight = -1.0;
        body_hue(other, counts, &weight);
        if (weight > best * 1.25) {
            best = weight;
            std::memcpy(g_source, other, 1024u);
        }
    }
    const Paint &chosen_paint = burnout::paints::paint(paint);
    recolour(g_source, counts, chosen_paint, g_written);
    std::memcpy(p, g_written, 1024u);
    if (chosen_paint.kind != Paint::Kind::Solid) {
        const float body = body_hue(g_source, counts);
        if (body >= 0.0f) draw_texels(texture, texels, bytes, chosen_paint, body);
    } else if (g_texels != 0u) {
        restore_texels();
    }
    g_texture = texture;
    g_palette = palette;
    g_written_paint = paint;
}

// Trophy colours 4-8 written into the car's own palettes 3-7 (2026-11-13).
// They came from texture-pack liveries keyed on the texels and the palette:
// any change of the texels (the first crash), a pack merged differently or
// car files not the ones the keys were made from, and the game's own palette
// showed instead (0.0.82 on the Vita: orange shown red in car select, blue in
// the race until the first crash turned it orange). In the palettes they are
// the game's colours: car select, races and crashes alike, pack or no pack.
// The user's choice of 2026-11-11 (make_liveries.py COLOURS).
constexpr Paint kTrophyPaints[kTrophyColours]{
    {"ORANGE", 255, 107, 0}, {"ROSE", 255, 77, 173}, {"VERT LIME", 140, 255, 20}, {"ARGENT", 204, 209, 219}, {"OR", 255, 194, 46},
};
std::uint32_t g_trophy_texture{}, g_trophy_palette{};
std::uint8_t g_trophy_written[kTrophyColours * 1024]{};
std::uint64_t g_trophy_writes{};

void keep_trophy_colours(std::uint32_t texture) {
    if (texture == 0u || read8(texture + 176u) < 8u) return;
    const std::uint32_t palette = read32(texture + 8u);
    std::uint8_t *trophies = g_memory->raw_pointer(palette + 3u * 1024u, kTrophyColours * 1024u);
    const std::uint8_t *first = g_memory->raw_pointer(palette, 3u * 1024u);
    if (trophies == nullptr || first == nullptr) return;
    // Ours already (the same car, not loaded again).
    if (texture == g_trophy_texture && palette == g_trophy_palette &&
        std::memcmp(trophies, g_trophy_written, sizeof g_trophy_written) == 0)
        return;
    const std::uint32_t texels = read32(texture + 0x38u);
    if (palette <= texels || palette - texels > 512u * 512u) return;
    const std::uint32_t bytes = palette - texels;
    const std::uint8_t *data = g_memory->raw_pointer(texels, bytes);
    if (data == nullptr) return;
    std::uint32_t counts[256]{};
    for (std::uint32_t i = 0; i < bytes; ++i) ++counts[data[i]];
    // From the game's colour (1-3) whose body is the most coloured (a black
    // or grey first colour has no body hue to recolour, see apply).
    const std::uint8_t *source = first;
    double best = -1.0;
    body_hue(first, counts, &best);
    for (std::uint32_t k = 1; k < 3u; ++k) {
        double weight = -1.0;
        body_hue(first + k * 1024u, counts, &weight);
        if (weight > best * 1.25) {
            best = weight;
            source = first + k * 1024u;
        }
    }
    for (int k = 0; k < kTrophyColours; ++k) recolour(source, counts, kTrophyPaints[k], g_trophy_written + k * 1024);
    std::memcpy(trophies, g_trophy_written, sizeof g_trophy_written);
    g_trophy_texture = texture;
    g_trophy_palette = palette;
    if (++g_trophy_writes <= 50u) {
        char name[17]{};
        texture_name(texture, name);
        std::fprintf(stderr, "[paint] trophy colours 4-8 written into %s's palettes 4-8\n", name);
    }
}

} // namespace

int chosen() { return g_chosen; }
// The 24 extra paints are retired (2026-11-11): the trophies unlock the
// game's own colours instead (unlocked_colours).
int next_won(int) { return -1; }
void set_chosen(int) { g_chosen = -1; }

namespace {
bool g_all_unlocked{};  // settings: every trophy colour offered
} // namespace
void set_all_unlocked(bool all) { g_all_unlocked = all; }

int unlocked_colours(psprecomp::GuestMemory &memory) {
    static const bool all = std::getenv("PSPRECOMP_BURNOUT_ALL_PAINTS") != nullptr;  // PC tests
    if (all || g_all_unlocked) return kTrophyColours;
    const std::uint8_t *done = memory.raw_pointer(kObjectives, kObjectivePaints);
    if (done == nullptr) return 0;
    int trophies = 0;
    for (int tier = 0; tier < 4; ++tier) {
        bool won = true;
        for (int i = 0; i < 5; ++i) won = won && done[tier * 5 + i] == 1u;
        trophies += won ? 1 : 0;
    }
    return trophies + (trophies == 4 ? 1 : 0);  // the fifth: all four trophies
}

std::uint32_t next_colour(psprecomp::GuestMemory &memory, std::uint32_t next, std::uint32_t count, std::uint32_t native) {
    g_memory = &memory;
    g_chosen = -1;
    if (!g_enabled || count == 0u) return native;  // Bonus off: the game's colours
    // A car with 8 colours: 3 from the start, one more per Takedown Trophy
    // (Easy, Advanced, Expert, Champion) and the eighth with all four. The
    // game offers only 6 of them: its 7th and 8th were never shown.
    bool in_select = false;
    const std::uint32_t texture = player_texture(in_select);
    if (!in_select || texture == 0u || read8(texture + 176u) < 8u) return native;
    const std::uint32_t offered = static_cast<std::uint32_t>(kBaseColours + unlocked_colours(memory));
    const std::uint32_t colour = next % offered;
    std::fprintf(stderr, "[paint] colour %u of %u\n", colour + 1u, offered);
    return colour;
}

void update(psprecomp::GuestMemory &memory, bool enabled) {
    g_memory = &memory;
    g_enabled = enabled;
    bool in_select = false;
    const std::uint32_t texture = player_texture(in_select);
    if (enabled) keep_trophy_colours(texture);
    static bool select_had_car = false;
    if (in_select) {
        // Another car in car select (not the first one shown): its own
        // colours first. Between two cars the texture is 0 for a moment
        // while the next one loads: that is not a new visit (0.0.50 kept
        // Rose fluo for the next car and painted the Dominator Muscle's
        // flames).
        if (texture != 0u && texture != g_select_texture) {
            if (select_had_car && g_chosen >= 0) g_chosen = -1;
            g_select_texture = texture;
            select_had_car = true;
        }
    } else {
        g_select_texture = 0u;
        select_had_car = false;
    }
    // The paint's car: the one shown when COLOR chose the paint (next_colour),
    // or after launch the first car seen; any other car is left as the game
    // draws it (the awards screen's new car looks like car select to us).
    bool same_car = false;
    char name[17]{};
    if (g_chosen >= 0 && texture_name(texture, name)) {
        if (g_paint_car[0] == '\0') std::memcpy(g_paint_car, name, sizeof name);
        same_car = std::strcmp(g_paint_car, name) == 0;
    }
    const bool show = enabled && g_chosen >= 0 && same_car && read8(texture + 177u) == 0u && unlocked(g_chosen);
    if (!show) {
        if (g_texture != 0u) restore();
        return;
    }
    if (g_texture != 0u && g_texture != texture) restore();
    apply(texture, g_chosen);
}

} // namespace burnout::paints

// The game's COLOR press in car select (0x088F87E0, tools/patch_paints.py).
std::uint32_t burnout_car_colour(psprecomp::GuestMemory &memory, std::uint32_t next, std::uint32_t count, std::uint32_t native) {
    return burnout::paints::next_colour(memory, next, count, native);
}
