// SPDX-License-Identifier: GPL-2.0-or-later
// PSP save data encryption and PARAM.SFO hashes, hash mode 1. Ported from
// PPSSPP (Core/HLE/sceChnnlsv.cpp, Core/Dialog/SavedataParam.cpp, libkirk),
// GPL 2.0 or later. Checked against a save written by Adrenaline: same
// plain data, same file hash, same PARAM.SFO hash, bit-identical re-encryption.
#include "psp_savedata.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace burnout::psp_savedata {
namespace {

// ---------------------------------------------------------------- AES-128

constexpr std::uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59,
    0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1,
    0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83,
    0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
    0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee,
    0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
    0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1,
    0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

struct Aes128 {
    std::uint8_t round_keys[176];
    std::uint8_t inv_sbox[256];

    explicit Aes128(const std::uint8_t *key) {
        for (int i = 0; i < 256; ++i) inv_sbox[kSbox[i]] = static_cast<std::uint8_t>(i);
        std::memcpy(round_keys, key, 16);
        std::uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            std::uint8_t t[4];
            std::memcpy(t, round_keys + i - 4, 4);
            if (i % 16 == 0) {
                const std::uint8_t first = t[0];
                t[0] = static_cast<std::uint8_t>(kSbox[t[1]] ^ rcon);
                t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];
                t[3] = kSbox[first];
                rcon = xtime(rcon);
            }
            for (int j = 0; j < 4; ++j) round_keys[i + j] = static_cast<std::uint8_t>(round_keys[i - 16 + j] ^ t[j]);
        }
    }

    static std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u)); }
    static std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
        std::uint8_t r = 0;
        while (b != 0u) {
            if (b & 1u) r ^= a;
            a = xtime(a);
            b >>= 1u;
        }
        return r;
    }

    void encrypt_block(std::uint8_t *s) const {
        add_key(s, 0);
        for (int round = 1; round <= 10; ++round) {
            for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
            shift_rows(s);
            if (round != 10)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t *col = s + 4 * c;
                    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                    col[1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                    col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                    col[3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
                }
            add_key(s, round);
        }
    }

    void decrypt_block(std::uint8_t *s) const {
        add_key(s, 10);
        for (int round = 9; round >= 0; --round) {
            inv_shift_rows(s);
            for (int i = 0; i < 16; ++i) s[i] = inv_sbox[s[i]];
            add_key(s, round);
            if (round != 0)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t *col = s + 4 * c;
                    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<std::uint8_t>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                    col[1] = static_cast<std::uint8_t>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                    col[2] = static_cast<std::uint8_t>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                    col[3] = static_cast<std::uint8_t>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
                }
        }
    }

    void add_key(std::uint8_t *s, int round) const {
        for (int i = 0; i < 16; ++i) s[i] ^= round_keys[round * 16 + i];
    }
    static void shift_rows(std::uint8_t *s) {
        std::uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        std::memcpy(s, t, 16);
    }
    static void inv_shift_rows(std::uint8_t *s) {
        std::uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
        std::memcpy(s, t, 16);
    }
};

// KIRK keys (key vault entries) used by hash mode 1.
constexpr std::uint8_t kKey03[16] = {0x98, 0x02, 0xC4, 0xE6, 0xEC, 0x9E, 0x9E, 0x2F, 0xFC, 0x63, 0x4C, 0xE4, 0x2F, 0xBB, 0x46, 0x68};
constexpr std::uint8_t kKey04[16] = {0x99, 0x24, 0x4C, 0xD2, 0x58, 0xF5, 0x1B, 0xCB, 0xB0, 0x61, 0x9C, 0xA7, 0x38, 0x30, 0x07, 0x5F};
constexpr std::uint8_t kKey53[16] = {0xAF, 0xFE, 0x8E, 0xB1, 0x3D, 0xD1, 0x7E, 0xD8, 0x0A, 0x61, 0x24, 0x1C, 0x95, 0x92, 0x56, 0xB6};

const Aes128 &mac_key() { static const Aes128 aes(kKey03); return aes; }
const Aes128 &seed_key() { static const Aes128 aes(kKey04); return aes; }
const Aes128 &stream_key() { static const Aes128 aes(kKey53); return aes; }

// KIRK commands 4 / 7: AES-128-CBC with a zero IV, in place.
void cbc_encrypt(const Aes128 &aes, std::uint8_t *data, std::size_t size) {
    std::uint8_t chain[16]{};
    for (std::size_t i = 0; i + 16u <= size; i += 16u) {
        for (int j = 0; j < 16; ++j) data[i + j] ^= chain[j];
        aes.encrypt_block(data + i);
        std::memcpy(chain, data + i, 16);
    }
}
void cbc_decrypt(const Aes128 &aes, std::uint8_t *data, std::size_t size) {
    std::uint8_t chain[16]{}, next[16];
    for (std::size_t i = 0; i + 16u <= size; i += 16u) {
        std::memcpy(next, data + i, 16);
        aes.decrypt_block(data + i);
        for (int j = 0; j < 16; ++j) data[i + j] ^= chain[j];
        std::memcpy(chain, next, 16);
    }
}

// sceSdMac* (mode 1): a CBC-MAC with a CMAC-style last block, over 2 KiB
// KIRK calls chained through `result`.
class Mac {
public:
    void update(const std::uint8_t *data, std::size_t size) {
        if (pending_.size() + size < 17u) {
            pending_.insert(pending_.end(), data, data + size);
            return;
        }
        std::size_t keep = (pending_.size() + size) & 15u;
        if (keep == 0u) keep = 16u;
        std::vector<std::uint8_t> buffer(pending_);
        buffer.insert(buffer.end(), data, data + size - keep);
        pending_.assign(data + size - keep, data + size);
        for (std::size_t i = 0; i < buffer.size(); i += 2048u) block(buffer.data() + i, std::min<std::size_t>(2048u, buffer.size() - i));
    }

    Hash final() {
        std::uint8_t k[16]{};
        mac_key().encrypt_block(k);
        double_key(k);
        std::uint8_t last[16]{};
        std::memcpy(last, pending_.data(), pending_.size());
        if (pending_.size() < 16u) {
            double_key(k);
            last[pending_.size()] = 0x80u;
        }
        Hash out{};
        for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(last[i] ^ k[i] ^ result_[i]);
        mac_key().encrypt_block(out.data());
        return out;
    }

private:
    static void double_key(std::uint8_t *k) {
        const bool carry = (k[0] & 0x80u) != 0u;
        for (int i = 0; i < 15; ++i) k[i] = static_cast<std::uint8_t>((k[i] << 1) | (k[i + 1] >> 7));
        k[15] = static_cast<std::uint8_t>((k[15] << 1) ^ (carry ? 0x87u : 0u));
    }
    void block(const std::uint8_t *data, std::size_t size) {
        std::vector<std::uint8_t> work(data, data + size);
        for (int i = 0; i < 16; ++i) work[static_cast<std::size_t>(i)] ^= result_[i];
        cbc_encrypt(mac_key(), work.data(), work.size());
        std::memcpy(result_, work.data() + work.size() - 16u, 16);
    }

    std::uint8_t result_[16]{};
    std::vector<std::uint8_t> pending_;
};

void put32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
std::uint32_t get32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

// sceSdCipherUpdate (mode 1): XORs `size` bytes (a multiple of 16) with the
// key stream of `seed` from block number `counter` (counting from 1).
void cipher(const std::uint8_t *seed, std::uint8_t *data, std::size_t size) {
    std::uint8_t base[16];
    std::memcpy(base, seed, 16);
    cbc_decrypt(seed_key(), base, 16);
    std::uint32_t counter = 1u;
    std::vector<std::uint8_t> blocks;
    for (std::size_t at = 0; at < size; at += 2048u) {
        const std::size_t chunk = std::min<std::size_t>(2048u, size - at);
        blocks.resize(chunk);
        std::uint8_t previous[16]{};
        if (counter != 1u) {
            std::memcpy(previous, base, 12);
            put32(previous + 12, counter - 1u);
        }
        for (std::size_t i = 0; i < chunk; i += 16u) {
            std::memcpy(blocks.data() + i, base, 12);
            put32(blocks.data() + i + 12, counter++);
        }
        std::vector<std::uint8_t> stream(blocks);
        cbc_decrypt(stream_key(), stream.data(), stream.size());
        // The decryption above chains within the call; the first block chains
        // with the block number before this run instead of zero.
        for (int j = 0; j < 16; ++j) stream[static_cast<std::size_t>(j)] ^= previous[j];
        for (std::size_t i = 0; i < chunk; ++i) data[at + i] ^= stream[i];
    }
}

std::size_t align16(std::size_t n) { return (n + 15u) & ~static_cast<std::size_t>(15u); }

// ---------------------------------------------------------------- PARAM.SFO

struct SfoEntry {
    std::string key;
    std::uint16_t format{};  // 0x0004 bytes, 0x0204 UTF-8 text, 0x0404 int32
    std::vector<std::uint8_t> value;
    std::uint32_t max{};
};

bool parse_sfo(const std::vector<std::uint8_t> &sfo, std::vector<SfoEntry> &entries) {
    if (sfo.size() < 20u || get32(sfo.data()) != 0x46535000u) return false;
    const std::uint32_t keys = get32(sfo.data() + 8), values = get32(sfo.data() + 12), count = get32(sfo.data() + 16);
    if (20u + count * 16u > sfo.size()) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *e = sfo.data() + 20u + i * 16u;
        SfoEntry entry;
        const std::size_t key_at = keys + static_cast<std::size_t>(e[0] | e[1] << 8);
        entry.format = static_cast<std::uint16_t>(e[2] | e[3] << 8);
        const std::uint32_t length = get32(e + 4), at = values + get32(e + 12);
        entry.max = get32(e + 8);
        if (key_at >= sfo.size() || at + length > sfo.size()) return false;
        for (std::size_t k = key_at; k < sfo.size() && sfo[k] != 0u; ++k) entry.key.push_back(static_cast<char>(sfo[k]));
        entry.value.assign(sfo.begin() + at, sfo.begin() + at + length);
        entries.push_back(std::move(entry));
    }
    return true;
}

const SfoEntry *find(const std::vector<SfoEntry> &entries, const char *key) {
    for (const auto &entry : entries)
        if (entry.key == key) return &entry;
    return nullptr;
}

std::string text(const SfoEntry *entry) {
    if (entry == nullptr) return {};
    std::string s(entry->value.begin(), entry->value.end());
    const auto end = s.find('\0');
    return end == std::string::npos ? s : s.substr(0, end);
}

constexpr std::size_t kFileListSize = 3168u;  // 99 entries: name[13], hash[16], 3 padding
constexpr std::size_t kParamsSize = 128u;

} // namespace

bool decrypt(const std::vector<std::uint8_t> &stored, std::vector<std::uint8_t> &plain, Hash &hash) {
    if (stored.size() <= 16u) return false;
    const std::size_t size = stored.size() - 16u, aligned = align16(size);
    std::vector<std::uint8_t> body(aligned, 0u);
    std::memcpy(body.data(), stored.data() + 16, size);
    Mac mac;
    mac.update(stored.data(), 16u);
    mac.update(body.data(), body.size());
    hash = mac.final();
    cipher(stored.data(), body.data(), body.size());
    plain.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(size));
    return true;
}

std::vector<std::uint8_t> encrypt(const std::uint8_t *plain, std::size_t size, Hash &hash) {
    const std::size_t aligned = align16(size);
    std::vector<std::uint8_t> stored(16u + aligned, 0u);
    // sceSdCipherInit(1): 16 random bytes through KIRK command 4 (key 4).
    std::random_device random;
    for (std::size_t i = 0; i < 16u; i += 4u) put32(stored.data() + i, random());
    cbc_encrypt(seed_key(), stored.data(), 16u);
    std::memcpy(stored.data() + 16, plain, size);
    cipher(stored.data(), stored.data() + 16, aligned);
    Mac mac;
    mac.update(stored.data(), stored.size());
    hash = mac.final();
    stored.resize(16u + size);  // the padding is hashed but not stored
    return stored;
}

bool read_sfo(const std::vector<std::uint8_t> &sfo, SfoInfo &info) {
    std::vector<SfoEntry> entries;
    if (!parse_sfo(sfo, entries)) return false;
    info.title = text(find(entries, "TITLE"));
    info.savedata_title = text(find(entries, "SAVEDATA_TITLE"));
    info.detail = text(find(entries, "SAVEDATA_DETAIL"));
    info.directory = text(find(entries, "SAVEDATA_DIRECTORY"));
    if (const SfoEntry *level = find(entries, "PARENTAL_LEVEL"); level != nullptr && level->value.size() >= 4u)
        info.parental_level = get32(level->value.data());
    return true;
}

int crypt_mode(const std::vector<std::uint8_t> &sfo) {
    std::vector<SfoEntry> entries;
    if (!parse_sfo(sfo, entries)) return 0;
    const SfoEntry *params = find(entries, "SAVEDATA_PARAMS");
    if (params == nullptr || params->value.empty()) return 0;
    switch (params->value[0]) {
    case 0x00: return 0;
    case 0x21: return 3;
    case 0x41: return 5;
    default: return 1;
    }
}

bool console_signed(const std::vector<std::uint8_t> &sfo) {
    std::vector<SfoEntry> entries;
    if (!parse_sfo(sfo, entries)) return false;
    const SfoEntry *params = find(entries, "SAVEDATA_PARAMS");
    if (params == nullptr || params->value.size() < 0x30u) return false;
    return std::any_of(params->value.begin() + 0x20, params->value.begin() + 0x30, [](std::uint8_t b) { return b != 0u; });
}

bool file_hash(const std::vector<std::uint8_t> &sfo, const std::string &file, Hash &hash) {
    std::vector<SfoEntry> entries;
    if (!parse_sfo(sfo, entries)) return false;
    const SfoEntry *list = find(entries, "SAVEDATA_FILE_LIST");
    if (list == nullptr) return false;
    for (std::size_t at = 0; at + 32u <= list->value.size(); at += 32u) {
        const char *name = reinterpret_cast<const char *>(list->value.data() + at);
        if (std::strncmp(name, file.c_str(), 13) == 0) {
            std::memcpy(hash.data(), list->value.data() + at + 13, 16);
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> build_sfo(const SfoInfo &info, const std::string &file, const Hash &hash,
                                    const std::vector<std::uint8_t> &previous) {
    std::vector<SfoEntry> old;
    parse_sfo(previous, old);
    // File list: the other files as they were, this one's hash updated.
    std::vector<std::uint8_t> list(kFileListSize, 0u);
    if (const SfoEntry *entry = find(old, "SAVEDATA_FILE_LIST"))
        std::memcpy(list.data(), entry->value.data(), std::min(kFileListSize, entry->value.size()));
    for (std::size_t at = 0; at + 32u <= list.size(); at += 32u) {
        char *name = reinterpret_cast<char *>(list.data() + at);
        if (name[0] != '\0' && std::strncmp(name, file.c_str(), 13) != 0) continue;
        std::memset(list.data() + at, 0, 32);
        std::memcpy(name, file.c_str(), std::min<std::size_t>(file.size(), 12u));
        std::memcpy(list.data() + at + 13, hash.data(), 16);
        break;
    }
    std::vector<std::uint8_t> params(kParamsSize, 0u);
    std::vector<std::uint8_t> console_hash(16u, 0u);  // console-bound (KIRK fuse key): kept, not checked on load
    if (const SfoEntry *entry = find(old, "SAVEDATA_PARAMS"); entry != nullptr && entry->value.size() >= 0x30u)
        std::memcpy(console_hash.data(), entry->value.data() + 0x20, 16);

    const auto text_value = [](const std::string &s, std::uint32_t max) {
        std::vector<std::uint8_t> v(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(s.size(), max - 1u)));
        v.push_back(0u);
        return v;
    };
    std::vector<std::uint8_t> level(4u);
    put32(level.data(), info.parental_level);
    // Keys in alphabetical order, as the PSP writes them.
    std::vector<SfoEntry> entries{
        {"CATEGORY", 0x0204, text_value("MS", 4u), 4u},
        {"PARENTAL_LEVEL", 0x0404, level, 4u},
        {"SAVEDATA_DETAIL", 0x0204, text_value(info.detail, 1024u), 1024u},
        {"SAVEDATA_DIRECTORY", 0x0204, text_value(info.directory, 64u), 64u},
        {"SAVEDATA_FILE_LIST", 0x0004, list, static_cast<std::uint32_t>(kFileListSize)},
        {"SAVEDATA_PARAMS", 0x0004, params, static_cast<std::uint32_t>(kParamsSize)},
        {"SAVEDATA_TITLE", 0x0204, text_value(info.savedata_title, 128u), 128u},
        {"TITLE", 0x0204, text_value(info.title, 128u), 128u},
    };
    std::vector<std::uint8_t> keys;
    std::vector<std::uint32_t> key_offsets;
    for (const auto &entry : entries) {
        key_offsets.push_back(static_cast<std::uint32_t>(keys.size()));
        keys.insert(keys.end(), entry.key.begin(), entry.key.end());
        keys.push_back(0u);
    }
    while (keys.size() % 4u != 0u) keys.push_back(0u);
    const std::uint32_t key_table = 20u + static_cast<std::uint32_t>(entries.size()) * 16u;
    const std::uint32_t value_table = key_table + static_cast<std::uint32_t>(keys.size());
    std::vector<std::uint8_t> sfo(value_table, 0u);
    put32(sfo.data(), 0x46535000u);
    put32(sfo.data() + 4, 0x00000101u);
    put32(sfo.data() + 8, key_table);
    put32(sfo.data() + 12, value_table);
    put32(sfo.data() + 16, static_cast<std::uint32_t>(entries.size()));
    std::memcpy(sfo.data() + key_table, keys.data(), keys.size());
    std::size_t params_at = 0u;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::uint8_t *e = sfo.data() + 20u + i * 16u;
        const std::uint32_t at = static_cast<std::uint32_t>(sfo.size()) - value_table;
        e[0] = static_cast<std::uint8_t>(key_offsets[i]);
        e[1] = static_cast<std::uint8_t>(key_offsets[i] >> 8);
        e[2] = static_cast<std::uint8_t>(entries[i].format);
        e[3] = static_cast<std::uint8_t>(entries[i].format >> 8);
        put32(e + 4, static_cast<std::uint32_t>(entries[i].value.size()));
        put32(e + 8, entries[i].max);
        put32(e + 12, at);
        if (entries[i].key == "SAVEDATA_PARAMS") params_at = sfo.size();
        sfo.insert(sfo.end(), entries[i].value.begin(), entries[i].value.end());
        sfo.resize(sfo.size() + (entries[i].max - entries[i].value.size()), 0u);
    }
    // Signature (hash mode 1): flag 1, the console-bound hash at 0x20, then
    // the hash of the whole file (zero padded to 16 bytes) at 0x10.
    sfo[params_at] = 0x01u;
    std::memcpy(sfo.data() + params_at + 0x20, console_hash.data(), 16);
    std::vector<std::uint8_t> padded(sfo);
    padded.resize(align16(padded.size()), 0u);
    Mac mac;
    mac.update(padded.data(), padded.size());
    const Hash signature = mac.final();
    std::memcpy(sfo.data() + params_at + 0x10, signature.data(), 16);
    return sfo;
}

} // namespace burnout::psp_savedata
