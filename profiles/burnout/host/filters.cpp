#include "filters.hpp"

#include <algorithm>

namespace burnout::filters {
namespace {

// Each filter is code inserted in the generated shader's main(), in its own
// block, at one of these points (variables in scope: s = picture coordinate
// sampled, 0..1; screen = screen coordinate, 0..1; inside = 1 within a console
// mode's frame; c = colour):
//   uv:     moves s before anything (mirror, upside down)
//   res:    resolution (console modes: frame box, pixel grid)
//   rescol: the console's colours, right after the sample
//   col:    colour (films, outlines)
//   scr:    screen (bars, vignette, dot grid), on screen coordinates
struct Code {
    const char *uv, *res, *rescol, *col, *scr;
};

struct Entry {
    Filter filter;
    Code code;
};

// Console frame: aspect ratio, resolution (in the console's pixels).
#define CONSOLE(aspect, w, h) "console_box(s, inside, " #aspect ", vec2(" #w ", " #h "));"

const Entry kFilters[] = {
    // ---- Film (colour)
    {{"bw", "Black & White Film", Family::Film}, {nullptr, nullptr, nullptr, "c = vec3(luma(c));", nullptr}},
    {{"sepia", "Sepia Film", Family::Film},
     {nullptr, nullptr, nullptr,
      "float l = luma(c); c = vec3(l * 1.07 + 0.03, l * 0.87 + 0.02, l * 0.66);", nullptr}},
    {{"noir", "Noir Film", Family::Film},
     {nullptr, nullptr, nullptr,
      "float l = clamp((luma(c) - 0.5) * 1.6 + 0.5, 0.0, 1.0);"
      "l += (hash(screen * 960.0 + fract(u_time) * 97.0) - 0.5) * 0.08;"
      "c = vec3(l);",
      "vec2 d = screen - 0.5; c *= clamp(1.25 - dot(d, d) * 2.2, 0.0, 1.0);"}},
    {{"vintage", "Vintage Film", Family::Film},
     {nullptr, nullptr, nullptr,
      "c = c * vec3(1.08, 0.98, 0.78) * 0.85 + vec3(0.09, 0.07, 0.05);"
      "c += (hash(screen * 960.0 + fract(u_time) * 53.0) - 0.5) * 0.07;",
      "vec2 d = screen - 0.5; c *= clamp(1.15 - dot(d, d) * 1.2, 0.0, 1.0);"}},
    {{"saturated", "Saturated Film", Family::Film},
     {nullptr, nullptr, nullptr, "c = clamp(mix(vec3(luma(c)), c, 1.8), 0.0, 1.0);", nullptr}},
    {{"gritty", "Gritty Film", Family::Film},
     {nullptr, nullptr, nullptr,
      "c = mix(vec3(luma(c)), c, 0.45); c = clamp((c - 0.5) * 1.45 + 0.5, 0.0, 1.0);", nullptr}},
    {{"arthouse", "Arthouse Film", Family::Film},
     {nullptr, nullptr, nullptr, "c = mix(vec3(luma(c)), c, 0.4) * vec3(0.9, 0.98, 1.1);",
      "vec2 d = screen - 0.5; c *= clamp(1.2 - dot(d, d) * 1.6, 0.0, 1.0);"}},
    {{"blockbuster", "Blockbuster Film", Family::Film},
     {nullptr, nullptr, nullptr,
      "float l = luma(c);"
      "vec3 graded = mix(vec3(0.0, 0.32, 0.38), vec3(1.0, 0.62, 0.3), l);"
      "c = clamp(mix(c, graded * (0.6 + l), 0.45) * 1.05, 0.0, 1.0);",
      nullptr}},
    {{"negative", "Negative Film", Family::Film}, {nullptr, nullptr, nullptr, "c = 1.0 - c;", nullptr}},
    {{"rainbow", "Rainbow", Family::Film}, {nullptr, nullptr, nullptr, "c = clamp(hue(c, u_time * 1.5), 0.0, 1.0);", nullptr}},
    {{"res_colours", "Res Colours", Family::Film}, {nullptr, nullptr, nullptr, "c = quantize(c, 32.0);", nullptr}},
    {{"gb_colours", "GB Colours", Family::Film}, {nullptr, nullptr, nullptr, "c = gb_palette(luma(c));", nullptr}},
    {{"painted", "Painted", Family::Film}, {nullptr, nullptr, nullptr, "c = quantize(c, 5.0);", nullptr}},
    {{"shadow_puppets", "Shadow Puppets", Family::Film},
     {nullptr, nullptr, nullptr, "float l = luma(c); c = l < 0.42 ? vec3(0.02) : mix(vec3(1.0, 0.85, 0.6), vec3(0.95, 0.5, 0.25), screen.y);",
      nullptr}},
    {{"glow_worm", "Glow Worm", Family::Film},
     {nullptr, nullptr, nullptr,
      "vec2 r = 3.0 / u_pixels; vec3 b = fetch(s + vec2(r.x, 0.0)) + fetch(s - vec2(r.x, 0.0)) + fetch(s + vec2(0.0, r.y)) +"
      "fetch(s - vec2(0.0, r.y)) + fetch(s + r) + fetch(s - r) + fetch(s + vec2(r.x, -r.y)) + fetch(s + vec2(-r.x, r.y));"
      "b = max(b * 0.125 - 0.55, 0.0) * 2.2; c = clamp(c + b, 0.0, 1.0);",
      nullptr}},
    // ---- Screen
    {{"mirror", "Derorrim", Family::Screen}, {"s.x = 1.0 - s.x;", nullptr, nullptr, nullptr, nullptr}},
    {{"upside_down", "Upside Down", Family::Screen}, {"s.y = 1.0 - s.y;", nullptr, nullptr, nullptr, nullptr}},
    {{"cinematic", "Cinematic Ratio", Family::Screen},
     {nullptr, nullptr, nullptr, nullptr, "c *= step(abs(screen.y - 0.5), 0.369);"}},
    {{"tv_ratio", "TV Ratio", Family::Screen},
     {nullptr, nullptr, nullptr, nullptr,
      "vec2 p = vec2((screen.x - 0.5) / 0.3778, (screen.y - 0.5) / 0.5);"  // 4:3 box, unit square
      "vec2 q = max(abs(p) - vec2(0.88, 0.85), 0.0);"
      "c *= 1.0 - smoothstep(0.08, 0.11, length(q * vec2(1.0, 1.15)));"
      "c *= step(abs(p.x), 1.0) * step(abs(p.y), 1.0);"}},
    {{"spotlight", "Spotlight", Family::Screen},
     {nullptr, nullptr, nullptr, nullptr, "vec2 d = (screen - 0.5) * vec2(1.7647, 1.0); c *= clamp(1.35 - length(d) * 1.9, 0.08, 1.15);"}},
    {{"dot_matrix", "Dot Matrix", Family::Screen},
     {nullptr, nullptr, nullptr, nullptr,
      "vec2 cell = fract(screen * vec2(240.0, 136.0)) - 0.5;"
      "c = mix(c * 1.1 + 0.06, vec3(0.0), smoothstep(0.32, 0.46, length(cell)));"}},
    {{"vhs", "VHS", Family::Screen},
     {nullptr, nullptr, nullptr,
      "float band = smoothstep(0.0, 0.03, abs(fract(screen.y * 0.8 - u_time * 0.12) - 0.5) - 0.47);"
      "vec2 o = vec2(2.5 / u_pixels.x + (1.0 - band) * 0.01, 0.0);"
      "c = vec3(fetch(s + o).r, c.g, fetch(s - o).b);"
      "c = mix(c, vec3(luma(c)), 0.25) + (hash(vec2(screen.y * 544.0, floor(u_time * 30.0))) - 0.5) * 0.09;",
      "c *= 0.92 + 0.08 * step(0.5, fract(screen.y * 272.0));"}},
    // ---- Outlines (Sobel)
    {{"felt_tip", "Felt Tip Outline", Family::Outline},
     {nullptr, nullptr, nullptr, "c = mix(c, vec3(0.03), smoothstep(0.4, 0.7, sobel(s, 1.5 / u_pixels)));", nullptr}},
    {{"comic", "Comic", Family::Outline},
     {nullptr, nullptr, nullptr,
      "float e = sobel(s, 1.5 / u_pixels); vec3 p = quantize(c, 4.0);"
      "float dots = step(length(fract(screen * vec2(160.0, 90.0)) - 0.5), 0.55 - luma(c) * 0.5);"
      "c = mix(p, p * 0.55, dots * 0.6); c = mix(c, vec3(0.0), smoothstep(0.4, 0.65, e));",
      nullptr}},
    {{"gritty_comic", "Gritty Comic", Family::Outline},
     {nullptr, nullptr, nullptr,
      "float e = sobel(s, 1.5 / u_pixels); c = clamp((mix(vec3(luma(c)), c, 0.5) - 0.5) * 1.5 + 0.5, 0.0, 1.0);"
      "c = mix(c, vec3(0.0), smoothstep(0.35, 0.6, e));",
      nullptr}},
    {{"cartoony", "Cartoony", Family::Outline},
     {nullptr, nullptr, nullptr,
      "float e = sobel(s, 1.5 / u_pixels); float l = luma(c);"
      "c = c / max(l, 0.05) * (floor(l * 3.0 + 0.5) / 3.0 * 0.85 + 0.1);"
      "c = mix(clamp(c, 0.0, 1.0), vec3(0.02), smoothstep(0.4, 0.65, e));",
      nullptr}},
    // ---- Console modes (resolution, then colours)
    {{"psp", "PSP", Family::Console}, {nullptr, CONSOLE(1.7647, 480.0, 272.0), nullptr, nullptr, nullptr}},
    {{"psp1000", "PSP-1000", Family::Console},
     {nullptr, CONSOLE(1.7647, 480.0, 272.0),
      "c = mix(c, (fetch(s + vec2(1.0 / 480.0, 0.0)) + fetch(s - vec2(1.0 / 480.0, 0.0))) * 0.5, 0.3) * 0.96;", nullptr, nullptr}},
    {{"psp2000", "PSP-2000", Family::Console}, {nullptr, CONSOLE(1.7647, 480.0, 272.0), nullptr, nullptr, nullptr}},
    {{"psp3000", "PSP-3000", Family::Console},
     {nullptr, CONSOLE(1.7647, 480.0, 272.0), nullptr, nullptr, "c *= 0.88 + 0.12 * step(0.5, fract(screen.y * 272.0));"}},
    {{"game_boy", "Game Boy", Family::Console}, {nullptr, CONSOLE(1.1111, 160.0, 144.0), "c = gb_palette(luma(c));", nullptr, nullptr}},
    {{"gb_resolution", "GB Resolution", Family::Console}, {nullptr, CONSOLE(1.1111, 160.0, 144.0), nullptr, nullptr, nullptr}},
    {{"game_boy_color", "Game Boy Color", Family::Console},
     {nullptr, CONSOLE(1.1111, 160.0, 144.0), "c = quantize(c, 32.0);", nullptr, nullptr}},
    {{"game_boy_advance", "Game Boy Advance", Family::Console},
     {nullptr, CONSOLE(1.5, 240.0, 160.0), "c = quantize(mix(c, vec3(0.75), 0.15), 32.0);", nullptr, nullptr}},
    {{"nes", "NES", Family::Console},
     {nullptr, CONSOLE(1.3333, 256.0, 240.0), "c = quantize(c + (bayer4(s * vec2(256.0, 240.0)) - 0.5) * 0.15, 4.0);", nullptr, nullptr}},
    {{"snes", "Super NES", Family::Console}, {nullptr, CONSOLE(1.3333, 256.0, 224.0), "c = quantize(c, 32.0);", nullptr, nullptr}},
    {{"mega_drive", "Mega Drive", Family::Console},
     {nullptr, CONSOLE(1.3333, 320.0, 224.0),
      "vec2 g = floor(s * vec2(320.0, 224.0)); c = quantize(c + (mod(g.x + g.y, 2.0) - 0.5) * 0.12, 8.0);", nullptr, nullptr}},
    {{"ps1", "PS1", Family::Console},
     {nullptr, CONSOLE(1.3333, 320.0, 240.0), "c = quantize(c + (bayer4(s * vec2(320.0, 240.0)) - 0.5) * (1.0 / 16.0), 32.0);", nullptr,
      nullptr}},
    {{"n64", "N64", Family::Console},
     {nullptr, "console_box(s, inside, 1.3333, vec2(10000.0, 10000.0));",
      "vec2 r = vec2(0.5 / 320.0, 0.5 / 240.0); c = (fetch(s + r) + fetch(s - r) + fetch(s + vec2(r.x, -r.y)) + fetch(s + vec2(-r.x, r.y))) * 0.25;",
      nullptr, nullptr}},
    {{"ds", "DS", Family::Console}, {nullptr, CONSOLE(1.3333, 256.0, 192.0), "c = quantize(c, 64.0);", nullptr, nullptr}},
    // Added after the first 40 (unlocks.bin keys the unlocked ones by index).
    // Shadow Stingdom: a night drive, the scene lit by the headlights' cone
    // ahead of the car and by its own bright lights.
    {{"shadow_stingdom", "Shadow Stingdom", Family::Film},
     {nullptr, nullptr, nullptr,
      "vec2 d = (screen - vec2(0.5, 0.3)) * vec2(1.0, 1.7); float beam = smoothstep(0.62, 0.05, length(d));"
      "float glow = smoothstep(0.88, 1.0, luma(c)) * (1.0 - smoothstep(0.4, 0.6, screen.y)); c = c * (0.08 + 0.95 * beam) + c * glow * 0.7;",
      nullptr}},
    // D.I.S.C.O: coloured lights sweeping the screen to the beat.
    {{"disco", "D.I.S.C.O", Family::Film},
     {nullptr, nullptr, nullptr,
      "float beat = 0.75 + 0.25 * sin(u_time * 8.0);"
      "vec3 light = 0.5 + 0.5 * cos(u_time * 2.0 + vec3(0.0, 2.1, 4.2) + screen.x * 7.0 - screen.y * 4.0);"
      "c = clamp(c * (0.45 + 0.85 * light * beat), 0.0, 1.0);",
      nullptr}},
    // Chameleon Colours: the picture's hue follows the car's speed (a full
    // turn of the colour wheel from standstill to 180 mph).
    {{"chameleon", "Chameleon Colours", Family::Film},
     {nullptr, nullptr, nullptr, "c = clamp(hue(c, u_speed * 0.0349), 0.0, 1.0);", nullptr}},
    // Newspaper (the 1st Special Event, 2026-11-09: the Crash Headlines'
    // front pages): ink dots on yellowed paper, each dot as large as the
    // picture is dark (a halftone screen), a little paper grain.
    {{"newspaper", "Newspaper", Family::Film},
     {nullptr, nullptr, nullptr,
      "float l = clamp((luma(c) - 0.5) * 1.3 + 0.6, 0.0, 1.0); vec2 g = fract(screen * vec2(170.0, 96.0)) - 0.5;"
      "float ink = 1.0 - smoothstep(-0.06, 0.06, length(g) - (1.0 - l) * 0.62);"
      "c = mix(vec3(0.93, 0.90, 0.80), vec3(0.12, 0.11, 0.10), ink);"
      "c *= 0.96 + 0.04 * hash(screen * 960.0);",
      nullptr}},
    // Cel Shading (the 10th Special Event, 2026-11-09: "Borderlands", lines
    // thinner and smoother than Cartoony's): soft colour bands, and thin dark
    // lines where the brightness changes (four neighbours, one pixel away:
    // half the Sobel filters' reads).
    {{"cel", "Cel Shading", Family::Outline},
     {nullptr, nullptr, nullptr,
      "float l = luma(c); vec2 d = 1.0 / u_pixels;"
      "float e = abs(luma(fetch(s + vec2(d.x, 0.0))) + luma(fetch(s - vec2(d.x, 0.0)))"
      " + luma(fetch(s + vec2(0.0, d.y))) + luma(fetch(s - vec2(0.0, d.y))) - 4.0 * l);"
      "vec3 band = c / max(l, 0.05) * (floor(l * 4.0 + 0.5) / 4.0 * 0.85 + 0.12);"
      "c = mix(c, clamp(band, 0.0, 1.0), 0.6);"
      "c = mix(c, vec3(0.06), smoothstep(0.22, 0.5, e) * 0.8);",
      nullptr}},
};

#undef CONSOLE

const char *kPrelude = R"(
uniform sampler2D u_texture;
uniform vec2 u_extent;
uniform vec2 u_pixels;
uniform float u_time;
uniform float u_speed;  // the player's car, mph (0 when unknown)
varying vec2 v_texcoord;
vec3 fetch(vec2 s) { return texture2D(u_texture, clamp(s, 0.0, 1.0) * u_extent).rgb; }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float bayer2(vec2 a) { a = floor(a); return fract(dot(a, vec2(0.5, a.y * 0.75))); }
float bayer4(vec2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
vec3 quantize(vec3 c, float levels) { return floor(clamp(c, 0.0, 1.0) * (levels - 1.0) + 0.5) / (levels - 1.0); }
vec3 hue(vec3 c, float a) {
    vec3 k = vec3(0.57735);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}
vec3 gb_palette(float l) {
    if (l < 0.25) return vec3(0.06, 0.22, 0.06);
    if (l < 0.5) return vec3(0.19, 0.38, 0.19);
    if (l < 0.75) return vec3(0.55, 0.67, 0.06);
    return vec3(0.61, 0.74, 0.06);
}
float sobel(vec2 s, vec2 d) {
    float a = luma(fetch(s + vec2(-d.x, -d.y))), b = luma(fetch(s + vec2(0.0, -d.y))), e = luma(fetch(s + vec2(d.x, -d.y)));
    float f = luma(fetch(s + vec2(-d.x, 0.0))), h = luma(fetch(s + vec2(d.x, 0.0)));
    float i = luma(fetch(s + vec2(-d.x, d.y))), j = luma(fetch(s + vec2(0.0, d.y))), k = luma(fetch(s + vec2(d.x, d.y)));
    float gx = (e + 2.0 * h + k) - (a + 2.0 * f + i);
    float gy = (i + 2.0 * j + k) - (a + 2.0 * b + e);
    return sqrt(gx * gx + gy * gy);
}
// A console's frame: centred box of that aspect ratio on the 16:9 screen
// (black around it) showing the whole picture (squeezed, as an old TV shows
// a wide picture: the HUD stays readable) at that resolution.
void console_box(inout vec2 s, inout float inside, float aspect, vec2 res) {
    float w = min(1.0, aspect / 1.7647);
    float x0 = 0.5 - 0.5 * w;
    float lx = (s.x - x0) / w;
    inside *= step(0.0, lx) * step(lx, 1.0);
    s.x = (floor(clamp(lx, 0.0, 0.9999) * res.x) + 0.5) / res.x;
    s.y = (floor(clamp(s.y, 0.0, 0.9999) * res.y) + 0.5) / res.y;
}
)";

} // namespace

const std::vector<Filter> &all() {
    static const std::vector<Filter> filters = [] {
        std::vector<Filter> list;
        for (const auto &entry : kFilters) list.push_back(entry.filter);
        return list;
    }();
    return filters;
}

int find(const std::string &id) {
    const auto &list = all();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (id == list[i].id) return static_cast<int>(i);
    return -1;
}

std::string fragment_shader(const std::vector<int> &active) {
    std::vector<const Entry *> used;
    for (const int index : active)
        if (index >= 0 && index < static_cast<int>(std::size(kFilters)) && used.size() < static_cast<std::size_t>(kMaxActive))
            used.push_back(&kFilters[index]);
    if (used.empty()) return {};
    std::string body = "void main() {\n    vec2 s = v_texcoord / u_extent;\n    vec2 screen = s;\n    float inside = 1.0;\n";
    const auto stage = [&](const char *Code::*part, const char *comment) {
        body += std::string("    // ") + comment + "\n";
        for (const Entry *entry : used)
            if (entry->code.*part != nullptr) body += std::string("    { ") + (entry->code.*part) + " }\n";
    };
    stage(&Code::uv, "flips");
    stage(&Code::res, "resolution");
    body += "    vec3 c = fetch(s);\n";
    stage(&Code::rescol, "console colours");
    stage(&Code::col, "colour");
    stage(&Code::scr, "screen");
    body += "    gl_FragColor = vec4(c * inside, 1.0);\n}\n";
    return kPrelude + body;
}

std::string describe(const std::vector<int> &active) {
    std::string text;
    for (const int index : active) {
        if (index < 0 || index >= static_cast<int>(all().size())) continue;
        if (!text.empty()) text += " + ";
        text += all()[static_cast<std::size_t>(index)].name;
    }
    return text.empty() ? std::string("none") : text;
}

} // namespace burnout::filters
