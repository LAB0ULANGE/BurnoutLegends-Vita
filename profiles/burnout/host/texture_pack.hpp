#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// PPSSPP texture replacement packs (textures.ini + PNG files), as made for
// PPSSPP: the same keys (texture address, size and palette, and a hash of the
// texture bytes) pick the same PNG. The PNG files are decoded on a thread of
// their own; until one is ready the game's texture is drawn.
namespace burnout::texture_pack {

// Loads <dir>/textures.ini; false (and the pack stays off) when there is none.
bool open(const std::string &dir);
bool active() noexcept;

// PPSSPP's key parts for the texture about to be drawn (raw guest bytes as
// stored, swizzled or not).
struct TextureInfo {
    std::uint32_t address{}, format{}, buffer_width{}, width{}, height{};
    std::uint32_t dim{};          // GE TSIZE0 & 0xF0F
    std::uint32_t clut_format{};  // GE CLUT format word, command byte included
    const std::uint8_t *bytes{};  // texture data in guest memory
    const std::uint8_t *clut{};   // the palette as loaded (clut_bytes valid)
    std::uint32_t clut_bytes{};
};

struct Image {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};

// The replacement PNG for this texture (its path), or nullptr.
const std::string *match(const TextureInfo &info);
// Starts decoding `path` in the background (resized to powers of two, at
// most 1024 texels a side).
struct Pending;
std::shared_ptr<Pending> request(const std::string &path);
// The decoded image once ready (moved out: call once), else nullptr.
std::unique_ptr<Image> take(Pending &pending);
bool failed(const Pending &pending) noexcept;

// Diagnostics.
std::string report();

// PPSSPP's hashes, exposed for tests.
std::uint32_t quick_hash(const std::uint8_t *data, std::uint32_t size, bool aligned);
std::uint32_t xxh32(const std::uint8_t *data, std::size_t size, std::uint32_t seed);

} // namespace burnout::texture_pack
