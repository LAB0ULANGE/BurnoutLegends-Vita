#include "bonus.hpp"

#include "filters.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace burnout::bonus {
namespace {

// unlocks.bin: "BLB1", u32 version, u8 enabled, u8 test_unlock_all, 2 pad,
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
    state.test_unlock_all = data[9] != 0u;
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
    std::memcpy(data + 12, state.unlocked.data(), 16);
    for (std::size_t i = 0; i < 3u; ++i)
        std::memcpy(data + 28 + i * kIdSize, state.active[i].c_str(), std::min(state.active[i].size(), kIdSize - 1u));
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool ok = std::fwrite(data, 1u, kFileSize, file) == kFileSize;
    std::fclose(file);
    return ok;
}

bool available(const State &state, int filter) {
    if (filter < 0 || filter >= static_cast<int>(filters::all().size())) return false;
    return state.test_unlock_all || ((state.unlocked[static_cast<std::size_t>(filter) / 64u] >> (filter % 64)) & 1u) != 0u;
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

} // namespace burnout::bonus
