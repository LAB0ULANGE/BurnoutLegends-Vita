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
    bool test_unlock_all{true};            // until unlock detection exists: every filter is available
    std::array<std::string, 3> active{};   // filter ids ("" = empty slot)
    std::array<std::uint64_t, 2> unlocked{};  // bit i = filters::all()[i] unlocked

    bool operator==(const State &) const = default;
};

State load(const std::string &path);
bool save(const std::string &path, const State &state);

// Indices of the filters to apply (none when Bonus is off), in slot order.
std::vector<int> applied(const State &state);
bool available(const State &state, int filter);

// Slot 0's filter moved to the next (step +1) or previous available one,
// through "none".
void cycle(State &state, int slot, int step);

} // namespace burnout::bonus
