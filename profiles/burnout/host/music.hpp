#pragma once

#include <cstdint>
#include <string>

// The game's music is ATRAC3plus, which no decoder here handles. Each track is
// converted once on a PC to block IMA ADPCM (.ima files, see
// tools/convert_music.py) and played from there: when the game hands an ATRAC
// stream to sceAtracSetDataAndGetID, its RIFF header picks the track.
namespace burnout::music {

// Scans `dir` for EATraxNN.ima files; returns how many tracks were found.
int set_dir(const std::string &dir);

// The track whose .mus RIFF header starts `riff` (at least 8 bytes), or -1.
[[nodiscard]] int open(const std::uint8_t *riff, std::uint32_t bytes);
void close(int handle);

// Decodes the next `frames` stereo frames into `out`, looping between the
// track's loop points while `loops` is not 0 (negative: forever; a positive
// count goes down at each loop). Returns false once the track has ended (the
// rest of `out` is silence).
bool decode(int handle, std::int16_t *out, std::uint32_t frames, std::int32_t &loops);

} // namespace burnout::music
