#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Bonus state, kept in ux0:data/burnout/unlocks.bin (written only when it
// changes): the global "Bonus" switch, the filters switched on (by id, at
// most 3) and the unlock bitfield.
namespace burnout::bonus {

struct State {
    bool enabled{true};                    // "Bonus": off = the original game, no filter applied
    bool test_unlock_all{false};           // every filter available (tests); not kept in unlocks.bin
    std::array<std::string, 3> active{};   // filter ids ("" = empty slot)
    std::array<std::uint64_t, 2> unlocked{};  // bit i = filters::all()[i] unlocked
    bool antialias{false};                 // FXAA in races
    int paint{-1};                         // the extra car paint chosen (paints.hpp), -1: the game's colours

    bool operator==(const State &) const = default;
};

State load(const std::string &path);
bool save(const std::string &path, const State &state);

// Indices of the filters to apply (none when Bonus is off), in slot order.
std::vector<int> applied(const State &state);
bool available(const State &state, int filter);
// The Special Events' filters (one per Special Event won, in that order):
// how many, and the n-th's index in filters::all().
int postcard_count();
int postcard_filter(int n);

// What unlocks each filter (Burnout_Legends_Recompenses_2.xlsx): the n-th
// Special Event postcard (gold at a Burning Lap) the n-th filter of the
// postcard list (22 filters), 100 % D.I.S.C.O. Crash Headlines no longer unlock
// filters (they go to the Heavyweight vehicles).
// Filters in no list are available.
enum class Source { Free, Postcard, Headline, Complete };
struct Reward {
    Source source{Source::Free};
    int index{};  // 0-based place in its list
};
Reward filter_reward(int filter);
// The game's progress (postcards: bit n = Special Event n won, the game's
// order; Crash Headlines, % complete): the filters it earned are unlocked;
// the number newly unlocked.
int unlock_progress(State &state, int postcards, int headlines, int percent);

// Signature Takedowns (the game's Takedown Book order, 0-19). They no longer
// unlock filters (the second spreadsheet gives them the Collector cars).
constexpr int kSignatures = 20;
const char *signature_name(int signature);
int signature_filter(int signature);   // filter index, -1 if none (always now)
int filter_signature(int filter);      // -1: not unlocked by a Signature (always now)
// Marks `filter` unlocked; true when it was not.
bool unlock(State &state, int filter);

// Slot 0's filter moved to the next (step +1) or previous available one,
// through "none".
void cycle(State &state, int slot, int step);

// Switches `filter` on (into a free slot; with 3 on, the oldest makes room)
// or off. Switching one on also turns Bonus on.
void toggle(State &state, int filter);

} // namespace burnout::bonus
