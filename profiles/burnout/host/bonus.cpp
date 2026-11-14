#include "bonus.hpp"

#include "filters.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <cstdio>
#include <cstring>

namespace burnout::bonus {
namespace {

// unlocks.bin: "BLB1", u32 version, u8 enabled, u8 test_unlock_all, u8 antialias, u8 paint + 1,
// u64 unlocked[2], char active[3][24].
constexpr char kMagic[4]{'B', 'L', 'B', '1'};
constexpr std::uint32_t kVersion = 1u;
constexpr std::size_t kIdSize = 24u;
constexpr std::size_t kFileSize = 4u + 4u + 4u + 16u + 3u * kIdSize;

} // namespace

State load(const std::string &path) {
    State state;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return state;
    unsigned char data[kFileSize]{};
    const bool whole = std::fread(data, 1u, kFileSize, file) == kFileSize;
    std::fclose(file);
    std::uint32_t version = 0u;
    std::memcpy(&version, data + 4, 4);
    if (!whole || std::memcmp(data, kMagic, 4) != 0 || version != kVersion) return state;
    state.enabled = data[8] != 0u;
    state.test_unlock_all = false;  // the old test builds stored 1 here
    state.antialias = data[10] != 0u;
    state.paint = static_cast<int>(data[11]) - 1;
    std::memcpy(state.unlocked.data(), data + 12, 16);
    for (std::size_t i = 0; i < 3u; ++i) {
        char id[kIdSize + 1]{};
        std::memcpy(id, data + 28 + i * kIdSize, kIdSize);
        state.active[i] = filters::find(id) >= 0 ? id : "";
    }
    return state;
}

bool save(const std::string &path, const State &state) {
    unsigned char data[kFileSize]{};
    std::memcpy(data, kMagic, 4);
    std::memcpy(data + 4, &kVersion, 4);
    data[8] = state.enabled ? 1u : 0u;
    data[9] = state.test_unlock_all ? 1u : 0u;
    data[10] = state.antialias ? 1u : 0u;
    data[11] = static_cast<unsigned char>(state.paint >= 0 && state.paint < 255 ? state.paint + 1 : 0);
    std::memcpy(data + 12, state.unlocked.data(), 16);
    for (std::size_t i = 0; i < 3u; ++i)
        std::memcpy(data + 28 + i * kIdSize, state.active[i].c_str(), std::min(state.active[i].size(), kIdSize - 1u));
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool ok = std::fwrite(data, 1u, kFileSize, file) == kFileSize;
    std::fclose(file);
    return ok;
}

namespace {

struct SignatureReward {
    const char *name;    // as in the game's Takedown Book
    const char *filter;  // filters.cpp id
};
constexpr SignatureReward kSignatureRewards[kSignatures]{
    {"Gone Fishin'", "upside_down"},     {"Homewrecker", "vintage"},         {"Pillar Driller", "felt_tip"},
    {"Hit the Split", "mirror"},         {"Truck Torpedo", "saturated"},     {"Euro Tram Ram", "res_colours"},
    {"Snowed Under", "dot_matrix"},      {"Paid the Price", "cinematic"},    {"Riviera Roustabout", "blockbuster"},
    {"Gatecrasher", "gritty"},           {"Grapes of Wrath", "sepia"},       {"Market Stalled", "painted"},
    {"Tuk-Down", "rainbow"},             {"Ship Wreck", "vhs"},              {"Rumble in the Jungle", "cartoony"},
    {"Catch the Tour Bus", "tv_ratio"},  {"Taxi Takedown", "arthouse"},      {"Tow Away Zone", "spotlight"},
    {"Bridge Over", "negative"},         {"Median Mash", "bw"},
};

bool unlocked_bit(const State &state, int filter) {
    return ((state.unlocked[static_cast<std::size_t>(filter) / 64u] >> (filter % 64)) & 1u) != 0u;
}

} // namespace

const char *signature_name(int signature) {
    return signature >= 0 && signature < kSignatures ? kSignatureRewards[signature].name : "";
}

int signature_filter(int) { return -1; }
int filter_signature(int) { return -1; }

namespace {

// Burnout_Legends_Recompenses.xlsx (2026-11-09), in order: film, screen and
// drawn filters only. The console modes are no longer rewards (the Crash
// Headlines go to the Heavyweight vehicles): they are always available.
// 2026-11-09: the game has 10 Special Events (Driver Details: SPECIAL EVENTS
// 0/10), so 10 filters in unlock order (the user's): Newspaper first, seven
// of the lightest (one colour formula), Cel Shading last.
// 2026-11-11 (the user's new order): Black & White, Sepia, Saturated,
// Newspaper, VHS, Gritty, Arthouse, Vintage, Negative, Cel Shading.
constexpr const char *kPostcardFilters[]{"bw", "sepia", "saturated", "newspaper", "vhs",
                                         "gritty", "arthouse", "vintage", "negative", "cel"};
constexpr const char *kCompleteFilter = "disco";

} // namespace

Reward filter_reward(int filter) {
    if (filter < 0 || filter >= static_cast<int>(filters::all().size())) return {};
    const std::string id = filters::all()[static_cast<std::size_t>(filter)].id;
    for (int i = 0; i < static_cast<int>(std::size(kPostcardFilters)); ++i)
        if (id == kPostcardFilters[i]) return {Source::Postcard, i};
    if (id == kCompleteFilter) return {Source::Complete, 0};
    return {};
}

int unlock_progress(State &state, int postcards, int headlines, int percent) {
    int unlocked = 0;
    for (int i = 0; i < static_cast<int>(std::size(kPostcardFilters)); ++i)
        if ((postcards & (1 << i)) != 0 && unlock(state, filters::find(kPostcardFilters[i]))) ++unlocked;
    (void)headlines;
    if (percent >= 100 && unlock(state, filters::find(kCompleteFilter))) ++unlocked;
    return unlocked;
}

bool unlock(State &state, int filter) {
    if (filter < 0 || filter >= static_cast<int>(filters::all().size()) || unlocked_bit(state, filter)) return false;
    state.unlocked[static_cast<std::size_t>(filter) / 64u] |= std::uint64_t{1} << (filter % 64);
    return true;
}

bool available(const State &state, int filter) {
    if (filter < 0 || filter >= static_cast<int>(filters::all().size())) return false;
    // Only the Special Events' filters are offered now (the others are left out).
    if (filter_reward(filter).source != Source::Postcard) return false;
    return state.test_unlock_all || unlocked_bit(state, filter);
}

int postcard_count() { return static_cast<int>(std::size(kPostcardFilters)); }
int postcard_filter(int n) {
    return n >= 0 && n < postcard_count() ? filters::find(kPostcardFilters[n]) : -1;
}

std::vector<int> applied(const State &state) {
    std::vector<int> list;
    if (!state.enabled) return list;
    for (const auto &id : state.active) {
        const int index = filters::find(id);
        if (index >= 0 && available(state, index)) list.push_back(index);
    }
    return list;
}

void cycle(State &state, int slot, int step) {
    const int count = static_cast<int>(filters::all().size());
    int index = filters::find(state.active[static_cast<std::size_t>(slot)]);  // -1 = none
    for (int tries = 0; tries <= count; ++tries) {
        index += step;
        if (index >= count) index = -1;
        if (index < -1) index = count - 1;
        if (index == -1 || available(state, index)) break;
    }
    state.active[static_cast<std::size_t>(slot)] = index < 0 ? std::string() : std::string(filters::all()[static_cast<std::size_t>(index)].id);
}

void toggle(State &state, int filter) {
    if (filter < 0 || filter >= static_cast<int>(filters::all().size())) return;
    // One filter at a time: switching one on switches the others off.
    const std::string id = filters::all()[static_cast<std::size_t>(filter)].id;
    const bool was_on = std::find(state.active.begin(), state.active.end(), id) != state.active.end();
    for (auto &slot : state.active) slot.clear();
    if (!was_on) {
        state.active[0] = id;
        state.enabled = true;
    }
}

} // namespace burnout::bonus
