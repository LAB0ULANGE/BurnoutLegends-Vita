#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The game's PSMF movies (H.264, 480x272, video only: their sound is a
// separate ATRAC file the game plays itself), decoded frame by frame for the
// sceMpeg HLE. Vita: the hardware AVC decoder. PC (test tool): frames
// pre-decoded into $PSPRECOMP_BURNOUT_MOVIE_FRAMES/<name>.rgba, else black.
namespace burnout::movie {

constexpr int kWidth = 480, kHeight = 272;

class Movie {
public:
    virtual ~Movie() = default;
    // The next frame, RGBA8888 rows of kWidth pixels; false past the last one.
    virtual bool next_frame(std::vector<std::uint8_t> &rgba) = 0;
    [[nodiscard]] virtual std::size_t frame_count() const = 0;
    // Back to the first frame (a looping menu movie).
    virtual void rewind() = 0;
};

// Splits a PSMF file's MPEG-PS video stream (0xE0) into access units (one
// per frame, each starting with its access unit delimiter).
[[nodiscard]] std::vector<std::vector<std::uint8_t>> access_units(const std::vector<std::uint8_t> &file);

[[nodiscard]] std::unique_ptr<Movie> open(const std::string &host_path, std::string &error);

// Reads and demuxes these movies on a low-priority thread, so their first
// open() finds them ready (the menu backgrounds: 1-1.3 s freezes otherwise).
void prefetch(std::vector<std::string> host_paths);
// Forgets the cached movies (a playing one keeps its own copy): a race needs
// none, and they held up to 16 MiB of the Vita's heap through it
// (2026-11-13). The menus read them again when they play them.
void release_cache();

#if defined(_WIN32) && !defined(__vita__)
// The playable PC build: movies decoded by Windows (Media Foundation).
void use_windows_decoder();
#endif

} // namespace burnout::movie
