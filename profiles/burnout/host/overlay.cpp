#include "overlay.hpp"

#include <cmath>

#include "detached_thread.hpp"

#include <cctype>
#include <cstdio>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace burnout::overlay {
namespace {

constexpr int kAtlasW = 1024, kAtlasH = 512;
constexpr float kBakePixels = 64.0f;
std::vector<std::uint8_t> g_atlas;
stbtt_bakedchar g_chars[96];
float g_cap_height = 1.0f;  // baked 'H' height, pixels
bool g_ready = false;
// The white texel solid shapes sample (bottom-right corner of the atlas).
constexpr float kWhiteU = (kAtlasW - 2.0f) / kAtlasW, kWhiteV = (kAtlasH - 2.0f) / kAtlasH;

SpinLock g_lock;
std::vector<Vertex> g_current;
bool g_visible = false;

} // namespace

bool init(const std::string &font_path) {
    std::FILE *file = std::fopen(font_path.c_str(), "rb");
    if (file == nullptr) return false;
    std::vector<unsigned char> ttf;
    unsigned char chunk[16384];
    for (std::size_t n; (n = std::fread(chunk, 1u, sizeof(chunk), file)) > 0u;) ttf.insert(ttf.end(), chunk, chunk + n);
    std::fclose(file);
    g_atlas.assign(static_cast<std::size_t>(kAtlasW) * kAtlasH, 0u);
    if (stbtt_BakeFontBitmap(ttf.data(), 0, kBakePixels, g_atlas.data(), kAtlasW, kAtlasH, 32, 96, g_chars) == 0) return false;
    for (int y = kAtlasH - 4; y < kAtlasH; ++y)
        for (int x = kAtlasW - 4; x < kAtlasW; ++x) g_atlas[static_cast<std::size_t>(y) * kAtlasW + x] = 255u;
    const stbtt_bakedchar &h = g_chars['H' - 32];
    g_cap_height = static_cast<float>(h.y1 - h.y0);
    g_ready = g_cap_height > 0.0f;
    return g_ready;
}

bool ready() noexcept { return g_ready; }
const std::vector<std::uint8_t> &atlas() { return g_atlas; }
int atlas_width() noexcept { return kAtlasW; }
int atlas_height() noexcept { return kAtlasH; }

void Builder::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Colour c) {
    const Vertex a{x0, y0, u0, v0, c.r, c.g, c.b, c.a}, b{x1, y0, u1, v0, c.r, c.g, c.b, c.a}, d{x0, y1, u0, v1, c.r, c.g, c.b, c.a},
        e{x1, y1, u1, v1, c.r, c.g, c.b, c.a};
    vertices_.insert(vertices_.end(), {a, b, d, b, e, d});
}

void Builder::rotate(std::size_t first, float cx, float cy, float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (std::size_t i = first; i < vertices_.size(); ++i) {
        Vertex &v = vertices_[i];
        const float x = v.x - cx, y = v.y - cy;
        v.x = cx + x * c - y * s;
        v.y = cy + x * s + y * c;
    }
}

void Builder::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Colour c) {
    vertices_.push_back({x0, y0, kWhiteU, kWhiteV, c.r, c.g, c.b, c.a});
    vertices_.push_back({x1, y1, kWhiteU, kWhiteV, c.r, c.g, c.b, c.a});
    vertices_.push_back({x2, y2, kWhiteU, kWhiteV, c.r, c.g, c.b, c.a});
}

void Builder::bar(float x, float y, float w, float h, Colour c, float chamfer) {
    if (chamfer <= 0.0f) {
        quad(x, y, x + w, y + h, kWhiteU, kWhiteV, kWhiteU, kWhiteV, c);
        return;
    }
    // Rectangle without its bottom-right corner: a fan from the top-left.
    const float px[5]{x, x + w, x + w, x + w - chamfer, x}, py[5]{y, y, y + h - chamfer, y + h, y + h};
    for (int i = 1; i + 1 < 5; ++i) triangle(px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], c);
}

float Builder::text_width(float size, const std::string &string) const {
    if (!g_ready) return 0.0f;
    const float scale = size / g_cap_height;
    float pen = 0.0f;
    for (const char raw : string) {
        const int ch = std::toupper(static_cast<unsigned char>(raw));
        if (ch < 32 || ch >= 128) continue;
        pen += g_chars[ch - 32].xadvance * scale;
    }
    return pen;
}

float Builder::text(float x, float y, float size, Colour c, const std::string &string) {
    if (!g_ready) return 0.0f;
    const float scale = size / g_cap_height;
    const float baseline = y + size;
    float pen = 0.0f;
    for (const char raw : string) {
        const int ch = std::toupper(static_cast<unsigned char>(raw));
        if (ch < 32 || ch >= 128) continue;
        const stbtt_bakedchar &g = g_chars[ch - 32];
        if (g.x1 > g.x0) {
            const float x0 = x + pen + g.xoff * scale, y0 = baseline + g.yoff * scale;
            const float x1 = x0 + (g.x1 - g.x0) * scale, y1 = y0 + (g.y1 - g.y0) * scale;
            quad(x0, y0, x1, y1, static_cast<float>(g.x0) / kAtlasW, static_cast<float>(g.y0) / kAtlasH, static_cast<float>(g.x1) / kAtlasW,
                 static_cast<float>(g.y1) / kAtlasH, c);
        }
        pen += g.xadvance * scale;
    }
    return pen;
}

void publish(std::vector<Vertex> &&vertices) {
    std::lock_guard lock(g_lock);
    g_current = std::move(vertices);
    g_visible = !g_current.empty();
}

void current(std::vector<Vertex> &vertices) {
    std::lock_guard lock(g_lock);
    vertices = g_current;
}

bool visible() noexcept { return g_visible; }

} // namespace burnout::overlay
