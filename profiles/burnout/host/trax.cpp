#include "trax.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace burnout::trax {
namespace {

// The game's EA Trax data (ULES00125, EA_TRAX.md).
constexpr std::uint32_t kGameTable = 0x08B97E60u;    // 21 records of 24 bytes
constexpr std::uint32_t kGameTracks = 21u;
constexpr std::uint32_t kTemplateCount = 0x08B9805Cu; // copied into every player (+0x6C)
constexpr std::uint32_t kTemplateTable = 0x08B980A4u; // (+0xB4)
constexpr std::uint32_t kSettings = 0x08C041E0u;      // the game's 21 settings, other settings after
constexpr std::uint32_t kRecordBytes = 24u;
// Texts: 44 albums, 44 artists, 44 titles (Burnout 3's list), 23 unused.
constexpr std::uint32_t kAlbums = 2985u, kArtists = 3029u, kTitles = 3073u, kBlock = 44u;
constexpr int kMaxAdded = 23;
constexpr std::uint32_t kFirstFile = 24u;  // EATrax24.mus, ...
constexpr std::uint8_t kEverywhere = 0x0Fu;  // the table's default setting
// Our guest memory (the arena bonus_menu.cpp uses from 0x08100000).
constexpr std::uint32_t kTable = 0x08110000u;
constexpr std::uint32_t kTexts = 0x08111000u;  // 3 x 23 texts
constexpr std::uint32_t kTextBytes = 0x80u;
constexpr std::uint32_t kStringsMagic0 = 0x69F60308u, kStringsMagic1 = 0xB93622FAu;  // the text table's header (bonus_menu.cpp)

struct Added {
    std::filesystem::path path;
    std::string title, artist, album;
    std::uint32_t title_id{}, artist_id{}, album_id{};
    std::uint8_t setting{kEverywhere};
};

std::string g_folder, g_state_path;
std::vector<Added> g_added;
bool g_built{};
std::map<std::string, std::uint8_t> g_saved;  // file name -> setting

std::uint32_t read32(psprecomp::GuestMemory &m, std::uint32_t a) {
    std::uint32_t v = 0u;
    if (const std::uint8_t *p = m.raw_pointer(a, 4u)) std::memcpy(&v, p, 4u);
    return v;
}
void write32(psprecomp::GuestMemory &m, std::uint32_t a, std::uint32_t v) {
    if (std::uint8_t *p = m.raw_pointer(a, 4u)) std::memcpy(p, &v, 4u);
}

// Valid UTF-8 with accents (file names, some tags) to Latin-1; anything
// else unchanged (already Latin-1).
std::string latin1_of(const std::string &text) {
    std::string out;
    bool multibyte = false;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80u) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        const std::size_t n = (c & 0xE0u) == 0xC0u ? 2u : (c & 0xF0u) == 0xE0u ? 3u : (c & 0xF8u) == 0xF0u ? 4u : 0u;
        if (n == 0u || i + n > text.size()) return text;
        unsigned code = c & (0xFFu >> (n + 1u));
        for (std::size_t k = 1; k < n; ++k) {
            const auto d = static_cast<unsigned char>(text[i + k]);
            if ((d & 0xC0u) != 0x80u) return text;
            code = (code << 6u) | (d & 0x3Fu);
        }
        out += static_cast<char>(code < 0x100u ? code : '?');
        multibyte = true;
        i += n;
    }
    return multibyte ? out : text;
}

// Latin-1 / ASCII to the game's capitals (its font: A-Z, digits, punctuation).
std::string game_text(const std::string &text) {
    static const char *const kAccents[] = {"AAAAAAACEEEEIIII", "DNOOOOOxOUUUUYPs", "AAAAAAACEEEEIIII", "dNOOOOO/OUUUUYPY"};
    const std::string latin1 = latin1_of(text);
    std::string out;
    for (const unsigned char c : latin1) {
        if (c >= 0xC0u) out += static_cast<char>(std::toupper(static_cast<unsigned char>(kAccents[(c - 0xC0u) / 16u][(c - 0xC0u) % 16u])));
        else if (c >= 0x20u && c < 0x7Fu) out += static_cast<char>(std::toupper(c));
        else if (c >= 0x80u) out += ' ';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 40u) out.resize(40u);
    return out;
}

// One ID3v2 text frame's value as Latin-1 (UTF-16 and UTF-8 reduced).
std::string id3_text(const std::uint8_t *p, std::size_t n) {
    if (n == 0u) return {};
    const std::uint8_t encoding = p[0];
    std::string out;
    if (encoding == 1u || encoding == 2u) {
        std::size_t i = 1u;
        bool big = encoding == 2u;
        if (encoding == 1u && n >= 3u) {
            big = p[1] == 0xFEu;
            i = 3u;
        }
        for (; i + 1u < n; i += 2u) {
            const unsigned u = big ? (p[i] << 8u) | p[i + 1u] : p[i] | (p[i + 1u] << 8u);
            if (u == 0u) break;
            out += static_cast<char>(u < 0x100u ? u : '?');
        }
    } else if (encoding == 3u) {
        for (std::size_t i = 1u; i < n && p[i] != 0u;) {
            const std::uint8_t c = p[i];
            if (c < 0x80u) { out += static_cast<char>(c); ++i; }
            else if ((c & 0xE0u) == 0xC0u && i + 1u < n) { out += static_cast<char>(((c & 0x1Fu) << 6u) | (p[i + 1u] & 0x3Fu)); i += 2u; }
            else { out += '?'; i += (c & 0xF0u) == 0xE0u ? 3u : 4u; }
        }
    } else {
        for (std::size_t i = 1u; i < n && p[i] != 0u; ++i) out += static_cast<char>(p[i]);
    }
    return out;
}

// Title, artist and album from the MP3's ID3 tags (v2.3/2.4, then v1).
void read_tags(Added &track) {
    std::FILE *f = std::fopen(track.path.string().c_str(), "rb");
    if (f == nullptr) return;
    std::array<std::uint8_t, 10> head{};
    if (std::fread(head.data(), 1u, 10u, f) == 10u && std::memcmp(head.data(), "ID3", 3u) == 0) {
        const std::uint32_t size = (head[6] & 0x7Fu) << 21u | (head[7] & 0x7Fu) << 14u | (head[8] & 0x7Fu) << 7u | (head[9] & 0x7Fu);
        std::vector<std::uint8_t> tag(std::min<std::uint32_t>(size, 1u << 20u));
        tag.resize(std::fread(tag.data(), 1u, tag.size(), f));
        const bool v4 = head[3] == 4u;
        for (std::size_t at = 0u; at + 10u <= tag.size();) {
            const std::uint8_t *fr = tag.data() + at;
            if (fr[0] == 0u) break;
            const std::uint32_t n = v4 ? (fr[4] & 0x7Fu) << 21u | (fr[5] & 0x7Fu) << 14u | (fr[6] & 0x7Fu) << 7u | (fr[7] & 0x7Fu)
                                       : static_cast<std::uint32_t>(fr[4]) << 24u | fr[5] << 16u | fr[6] << 8u | fr[7];
            if (n == 0u || at + 10u + n > tag.size()) break;
            const std::string id(reinterpret_cast<const char *>(fr), 4u);
            if (id == "TIT2" && track.title.empty()) track.title = id3_text(fr + 10u, n);
            else if (id == "TPE1" && track.artist.empty()) track.artist = id3_text(fr + 10u, n);
            else if (id == "TALB" && track.album.empty()) track.album = id3_text(fr + 10u, n);
            at += 10u + n;
        }
    }
    if (track.title.empty() || track.artist.empty()) {
        std::array<char, 128> v1{};
        if (std::fseek(f, -128, SEEK_END) == 0 && std::fread(v1.data(), 1u, 128u, f) == 128u && std::memcmp(v1.data(), "TAG", 3u) == 0) {
            const auto field = [&](int at, int n) { std::string text(v1.data() + at, static_cast<std::size_t>(n)); return text.substr(0, text.find(static_cast<char>(0))); };
            if (track.title.empty()) track.title = field(3, 30);
            if (track.artist.empty()) track.artist = field(33, 30);
            if (track.album.empty()) track.album = field(63, 30);
        }
    }
    std::fclose(f);
    // No tags: "Artist - Title.mp3", else the file name as the title.
    if (track.title.empty()) {
        const std::string stem = track.path.stem().string();
        const auto dash = stem.find(" - ");
        if (dash != std::string::npos) {
            if (track.artist.empty()) track.artist = stem.substr(0, dash);
            track.title = stem.substr(dash + 3u);
        } else {
            track.title = stem;
        }
    }
    track.title = game_text(track.title);
    track.artist = game_text(track.artist);
    track.album = game_text(track.album.empty() ? std::string("Musique perso") : track.album);
    if (track.title.empty()) track.title = "?";
}

// A real MP3: an ID3v2 tag or an MPEG audio frame first (YouTube downloads
// are often AAC in MP4, "ftyp", renamed .mp3: minimp3 cannot play them).
bool is_mp3(const std::filesystem::path &path) {
    std::FILE *f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) return false;
    std::uint8_t head[4]{};
    const bool read = std::fread(head, 1u, 4u, f) == 4u;
    std::fclose(f);
    return read && ((head[0] == 'I' && head[1] == 'D' && head[2] == '3') || (head[0] == 0xFFu && (head[1] & 0xE0u) == 0xE0u));
}

void load_state() {
    g_saved.clear();
    std::FILE *f = std::fopen(g_state_path.c_str(), "rb");
    if (f == nullptr) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const auto tab = s.rfind('\t');
        if (tab == std::string::npos) continue;
        g_saved[s.substr(0, tab)] = static_cast<std::uint8_t>(std::strtoul(s.c_str() + tab + 1u, nullptr, 10));
    }
    std::fclose(f);
}

void save_state() {
    if (g_state_path.empty()) return;
    std::FILE *f = std::fopen(g_state_path.c_str(), "wb");
    if (f == nullptr) return;
    for (const Added &track : g_added) std::fprintf(f, "%s\t%u\n", track.path.filename().string().c_str(), track.setting);
    std::fclose(f);
}

void write_text(psprecomp::GuestMemory &m, std::uint32_t at, const std::string &text) {
    std::uint8_t *p = m.raw_pointer(at, kTextBytes);
    if (p == nullptr) return;
    std::memset(p, 0, kTextBytes);
    for (std::size_t i = 0; i < text.size() && i < kTextBytes / 2u - 1u; ++i) p[i * 2u] = static_cast<std::uint8_t>(text[i]);
}

// The game's text table: its header (two magic words, the count, the table
// of text pointers), found once in RAM and checked every frame after.
std::uint32_t text_table_finder(psprecomp::GuestMemory &m, std::uint32_t &count) {
    static std::uint32_t header = 0u;
    if (header == 0u || read32(m, header) != kStringsMagic0 || read32(m, header + 4u) != kStringsMagic1) {
        header = 0u;
        static int tries = 0;
        if (++tries % 30 != 1) return 0u;  // the scan twice a second until the texts are loaded
        const std::uint8_t *ram = m.raw_pointer(0x08800000u, 0x01800000u);
        if (ram == nullptr) return 0u;
        for (std::uint32_t off = 0; off + 16u <= 0x01800000u; off += 4u) {
            std::uint32_t a, b;
            std::memcpy(&a, ram + off, 4u);
            if (a != kStringsMagic0) continue;
            std::memcpy(&b, ram + off + 4u, 4u);
            if (b == kStringsMagic1) {
                header = 0x08800000u + off;
                break;
            }
        }
        if (header == 0u) return 0u;
    }
    count = read32(m, header + 8u);
    const std::uint32_t table = read32(m, header + 12u);
    return table >= 0x08800000u && table < 0x0A000000u ? table : 0u;
}

} // namespace

void configure(const std::string &folder, const std::string &state_path) {
    g_folder = folder;
    g_state_path = state_path;
    g_added.clear();
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return;
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(folder, ec)) {
        std::string ext = entry.path().extension().string();
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!entry.is_regular_file(ec) || ext != ".mp3") continue;
        if (is_mp3(entry.path())) files.push_back(entry.path());
        else std::fprintf(stderr, "[trax] %s: not an MP3 (AAC/M4A renamed?), left out\n", entry.path().filename().string().c_str());
    }
    std::sort(files.begin(), files.end());
    if (files.size() > static_cast<std::size_t>(kMaxAdded)) files.resize(kMaxAdded);
    load_state();
    for (const auto &path : files) {
        Added track;
        track.path = path;
        read_tags(track);
        if (const auto saved = g_saved.find(path.filename().string()); saved != g_saved.end()) track.setting = saved->second;
        g_added.push_back(track);
        std::fprintf(stderr, "[trax] %s: \"%s\" by \"%s\" (%s)\n", path.filename().string().c_str(), track.title.c_str(),
                     track.artist.c_str(), track.album.c_str());
    }
}

void after_load(psprecomp::GuestMemory &memory) {
    g_built = false;
    if (g_added.empty()) return;
    if (read32(memory, kTemplateCount) != kGameTracks || read32(memory, kTemplateTable) != kGameTable) {
        std::fprintf(stderr, "[trax] the game's EA Trax template is not where expected: tracks not added\n");
        g_added.clear();
        return;
    }
    // Texts no record of the game's uses, in each block.
    std::array<std::vector<std::uint32_t>, 3> free;
    const std::array<std::uint32_t, 3> bases{kTitles, kAlbums, kArtists};
    for (int b = 0; b < 3; ++b)
        for (std::uint32_t id = bases[b]; id < bases[b] + kBlock; ++id) {
            bool used = false;
            for (std::uint32_t i = 0; i < kGameTracks && !used; ++i) used = read32(memory, kGameTable + i * kRecordBytes + 8u + b * 4u) == id;
            if (!used) free[b].push_back(id);
        }
    const std::size_t room = std::min({free[0].size(), free[1].size(), free[2].size()});
    if (g_added.size() > room) g_added.resize(room);
    std::uint8_t *table = memory.raw_pointer(kTable, (kGameTracks + kMaxAdded) * kRecordBytes);
    const std::uint8_t *game = memory.raw_pointer(kGameTable, kGameTracks * kRecordBytes);
    if (table == nullptr || game == nullptr) {
        g_added.clear();
        return;
    }
    std::memcpy(table, game, kGameTracks * kRecordBytes);
    for (std::size_t k = 0; k < g_added.size(); ++k) {
        Added &track = g_added[k];
        track.title_id = free[0][k];
        track.album_id = free[1][k];
        track.artist_id = free[2][k];
        const std::uint32_t record = kTable + static_cast<std::uint32_t>(kGameTracks + k) * kRecordBytes;
        write32(memory, record, kFirstFile + static_cast<std::uint32_t>(k));
        write32(memory, record + 4u, 0u);
        write32(memory, record + 8u, track.title_id);
        write32(memory, record + 12u, track.album_id);
        write32(memory, record + 16u, track.artist_id);
        write32(memory, record + 20u, track.setting);
    }
    write32(memory, kTemplateCount, kGameTracks + static_cast<std::uint32_t>(g_added.size()));
    write32(memory, kTemplateTable, kTable);
    g_built = true;
    std::fprintf(stderr, "[trax] EA Trax: %u tracks (%u added)\n", kGameTracks + static_cast<unsigned>(g_added.size()),
                 static_cast<unsigned>(g_added.size()));
}

void update(psprecomp::GuestMemory &memory) {
    if (!g_built) return;
    std::uint32_t count = 0u;
    const std::uint32_t table = text_table_finder(memory, count);
    if (table == 0u || count <= kTitles + kBlock) return;
    for (std::size_t k = 0; k < g_added.size(); ++k) {
        const Added &track = g_added[k];
        const std::uint32_t texts = kTexts + static_cast<std::uint32_t>(k) * 3u * kTextBytes;
        const std::array<std::pair<std::uint32_t, const std::string *>, 3> items{
            {{track.title_id, &track.title}, {track.album_id, &track.album}, {track.artist_id, &track.artist}}};
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t ours = texts + static_cast<std::uint32_t>(i) * kTextBytes;
            const std::uint32_t entry = table + items[i].first * 4u;
            if (read32(memory, entry) == ours) continue;
            write_text(memory, ours, *items[i].second);
            write32(memory, entry, ours);
        }
    }
}

void store_setting(psprecomp::GuestMemory &memory, std::uint32_t index, std::uint8_t value) {
    // The game's own (no track added: exactly as the game did).
    if (index < kGameTracks || !g_built) {
        if (std::uint8_t *p = memory.raw_pointer(kSettings + index, 1u)) *p = value;
        return;
    }
    const std::size_t k = index - kGameTracks;
    if (k >= g_added.size()) return;  // (never past our records: the game's other settings follow)
    if (g_added[k].setting != value) {
        g_added[k].setting = value;
        save_state();
    }
    write32(memory, kTable + index * kRecordBytes + 20u, value);
}

std::uint8_t load_setting(psprecomp::GuestMemory &memory, std::uint32_t index) {
    static int traced = 0;
    if (std::getenv("PSPRECOMP_BURNOUT_TRAX_TRACE") != nullptr && traced++ < 200) std::fprintf(stderr, "[trax] load %u\n", index);
    if (index < kGameTracks || !g_built) {
        const std::uint8_t *p = memory.raw_pointer(kSettings + index, 1u);
        return p != nullptr ? *p : 0u;
    }
    const std::size_t k = index - kGameTracks;
    return k < g_added.size() ? g_added[k].setting : 0u;
}

int added() { return g_built ? static_cast<int>(g_added.size()) : 0; }

std::string file_of(std::uint32_t n) {
    if (!g_built || n < kFirstFile || n - kFirstFile >= g_added.size()) return {};
    return g_added[n - kFirstFile].path.string();
}

} // namespace burnout::trax

// The EA Trax menu's commit loop (0x088C6D44, tools/patch_trax.py): the
// game's `sb a0, 0x7400(0x08BFCDE0 + i)` for each track.
void burnout_trax_store(psprecomp::GuestMemory &memory, std::uint32_t index, std::uint32_t value) {
    burnout::trax::store_setting(memory, index, static_cast<std::uint8_t>(value));
}

// The EA Trax menu's load (0x088C6DF4) and the playing track's (0x089B0370).
std::uint32_t burnout_trax_load(psprecomp::GuestMemory &memory, std::uint32_t index) {
    return burnout::trax::load_setting(memory, index);
}

// The settings applied to every track (0x089B2E10), from `settings` (the
// game's, or a copy of its 21): the first 21 from there, ours after.
std::uint32_t burnout_trax_load_at(psprecomp::GuestMemory &memory, std::uint32_t settings, std::uint32_t index) {
    if (index >= 21u && burnout::trax::added() > 0) return burnout::trax::load_setting(memory, index);
    const std::uint8_t *p = memory.raw_pointer(settings + index, 1u);
    return p != nullptr ? *p : 0u;
}
