#pragma once

#include <string>
#include <vector>

// Bonus filters: post-processing applied while the finished picture is copied
// to the screen (the present pass the renderer already makes), so no extra
// render target or pass: with no filter the present pass is exactly the one
// used without this feature. Up to 3 filters, combined into one generated
// fragment shader in a fixed order: resolution (console modes) -> colour ->
// screen.
namespace burnout::filters {

enum class Family { Film, Screen, Outline, Console };

struct Filter {
    const char *id;      // stable key (settings, unlock table)
    const char *name;    // shown name
    Family family;
};

const std::vector<Filter> &all();
int find(const std::string &id);  // -1 when unknown

constexpr int kMaxActive = 3;

// The fragment shader for these filters (indices into all()), empty if none.
// Uniforms: u_texture, u_extent (texture coordinates of the picture's far
// corner), u_pixels (picture size in its own pixels), u_time (seconds).
std::string fragment_shader(const std::vector<int> &active);
// "a + b + c" for logs.
std::string describe(const std::vector<int> &active);

} // namespace burnout::filters
