#pragma once

#include <cstdint>
#include <string>

namespace psprecomp {
class GuestMemory;
}

// The player's own music in EA Trax (2026-11-14, profiles/burnout/EA_TRAX.md).
// Tracks dropped in a folder are added after the game's 21 in its EA Trax
// table (menu Driver Details > Settings > EA GAMES TRAX, in-race rotation),
// with their title, artist and album written over 23 of the game's unused
// Burnout 3 texts. Their EA Trax setting is kept in our own file, never in
// the game's settings (which hold exactly 21). Nothing changes when the
// folder is empty or missing.
namespace burnout::trax {

// Before the game starts: the folder scanned and the file keeping the added
// tracks' settings.
void configure(const std::string &folder, const std::string &state_path);

// Right after the executable is loaded, before it runs: the bigger table
// built and the player's template pointed at it.
void after_load(psprecomp::GuestMemory &memory);

// Every frame: the added tracks' texts kept in the game's text table.
void update(psprecomp::GuestMemory &memory);

// The EA Trax menu's commit (0x088C6D44, tools/patch_trax.py): track
// `index`'s setting, in the game's settings for the first 21, ours after.
void store_setting(psprecomp::GuestMemory &memory, std::uint32_t index, std::uint8_t value);

// The menu's load and the playing track's (0x088C6DF4, 0x089B0370): the
// game's setting for the first 21, ours after.
std::uint8_t load_setting(psprecomp::GuestMemory &memory, std::uint32_t index);

// Added tracks found (0: the game's table untouched).
int added();

// The file of EATraxNN.mus for an added track (n >= 24), or empty.
std::string file_of(std::uint32_t n);

} // namespace burnout::trax
