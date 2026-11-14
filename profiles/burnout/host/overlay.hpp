#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Our own picture drawn over the game's (the Bonus menu): coloured shapes and
// text in screen pixels (960x544, origin top-left), built on the emulation
// thread and drawn by the GL thread after the game's frame. The game's memory
// is never touched for it.
namespace burnout::overlay {

struct Vertex {
    float x, y, u, v, r, g, b, a;
};

// Loads the font (TrueType) and bakes its atlas; false if it cannot.
bool init(const std::string &font_path);
bool ready() noexcept;
// The font atlas (alpha, atlas_width x atlas_height), uploaded by the GL thread.
const std::vector<std::uint8_t> &atlas();
int atlas_width() noexcept;
int atlas_height() noexcept;

struct Colour {
    float r, g, b, a;
};

class Builder {
public:
    // A bar like the game's menu rows: a rectangle with its bottom-right
    // corner cut by `chamfer` pixels.
    void bar(float x, float y, float w, float h, Colour colour, float chamfer = 0.0f);
    void rect(float x, float y, float w, float h, Colour colour) { bar(x, y, w, h, colour, 0.0f); }
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Colour colour);
    // Text (ASCII, drawn upper case) with its top at y, `size` pixels tall
    // capitals; returns its width.
    float text(float x, float y, float size, Colour colour, const std::string &string);
    float text_width(float size, const std::string &string) const;
    std::vector<Vertex> &&take() { return std::move(vertices_); }
    // Everything added since `first` (a size() taken before) turned by
    // `radians` around (cx, cy).
    std::size_t size() const { return vertices_.size(); }
    void rotate(std::size_t first, float cx, float cy, float radians);
    bool empty() const { return vertices_.empty(); }

private:
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Colour colour);
    std::vector<Vertex> vertices_;
};

// Emulation thread: the overlay to show from now on (empty: none).
void publish(std::vector<Vertex> &&vertices);
// GL thread: the current overlay (copied).
void current(std::vector<Vertex> &vertices);
bool visible() noexcept;

} // namespace burnout::overlay
