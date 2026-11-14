#include "texture_pack.hpp"
#include "detached_thread.hpp"
#include "burnout_bench.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG  // the Bonus postcards (ge_gl)
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace burnout::texture_pack {

struct Pending {
    std::string path;
    std::atomic<int> state{0};  // 0 queued, 1 ready, 2 failed
    std::unique_ptr<Image> image;
};

namespace {

enum class Hash { Quick, Xxh32, Xxh64 };

struct Key {
    std::uint64_t cachekey{};
    std::uint32_t hash{};
    bool operator==(const Key &o) const noexcept { return cachekey == o.cachekey && hash == o.hash; }
};
struct KeyHash {
    std::size_t operator()(const Key &k) const noexcept {
        return static_cast<std::size_t>(k.cachekey * 0x9E3779B97F4A7C15ull ^ k.hash);
    }
};

std::string g_dir;
bool g_active = false;
Hash g_hash = Hash::Quick;
bool g_ignore_address = false;
std::unordered_map<Key, std::string, KeyHash> g_files;  // level 0 replacements

SpinLock g_mutex;  // see detached_thread.hpp: std::mutex does nothing on the Vita
std::deque<std::shared_ptr<Pending>> g_queue;
std::atomic<std::uint64_t> g_found{0}, g_decoded{0}, g_failed{0}, g_lookups{0};
std::atomic<double> g_decode_ms{0.0};

constexpr std::uint8_t kBitsPerPixel[16]{16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
constexpr std::uint32_t kAlignMask[16]{0x7F8u, 0x7F8u, 0x7F8u, 0x7FCu, 0x7E0u, 0x7F0u, 0x7F8u, 0x7FCu,
                                       0x7FFu, 0x7FFu, 0x7FFu, 0u, 0u, 0u, 0u, 0u};

std::uint32_t read32(const std::uint8_t *p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4u);
    return v;
}

std::uint32_t rotl(std::uint32_t x, int r) noexcept { return (x << r) | (x >> (32 - r)); }

std::string trim(const std::string &s) {
    std::size_t a = 0u, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// The bytes in use (alloc_stats_vita.cpp, the Vita GL build): weak, so the
// other builds go without.
#if defined(__vita__)
extern "C" long long burnout_alloc_in_use() __attribute__((weak));
#else
long long (*const burnout_alloc_in_use)() = nullptr;  // (PC: no budget)
#endif
// 2026-11-11 (0.0.74, a Heavyweight race with three swapped rivals): the
// pack's pictures took the heap to 143 MiB in use and the game died. Past
// this, a picture is left out (the game's own texture stays).
constexpr long long kPackBudget = 136ll * 1024 * 1024;
std::atomic<int> g_skipped_low{0};

void decoder_main() {
    for (;;) {
        std::shared_ptr<Pending> job;
        {
            std::lock_guard lock(g_mutex);
            if (!g_queue.empty()) {
                job = std::move(g_queue.front());
                g_queue.pop_front();
            }
        }
        if (job == nullptr) {
            host_sleep_ms(5.0);
            continue;
        }
        if (burnout_alloc_in_use != nullptr && burnout_alloc_in_use() > kPackBudget) {
            ++g_skipped_low;
            job->state.store(2, std::memory_order_release);
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        // Memory runs short: that picture is left out (the game's own texture
        // stays), the game goes on (0.0.48 stopped on a std::bad_alloc here).
        stbi_uc *pixels = nullptr;
        try {
        std::ifstream file(job->path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, channels = 0;
        pixels = bytes.empty() ? nullptr
                                        : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (pixels == nullptr) {
            std::fprintf(stderr, "[pack] cannot decode %s\n", job->path.c_str());
            ++g_failed;
            job->state.store(2, std::memory_order_release);
            continue;
        }
        auto image = std::make_unique<Image>();
        // Powers of two (mip chains on the Vita), at most 1024 a side: the
        // nearest one in ratio, then a box/bilinear resample.
        const auto pow2 = [](int v) {
            int p = 1;
            while (p * 2 <= v) p *= 2;
            if (p < v && static_cast<double>(v) / p > static_cast<double>(p * 2) / v) p *= 2;
            return std::min(p, 1024);
        };
        const int tw = pow2(w), th = pow2(h);
        image->width = static_cast<std::uint32_t>(tw);
        image->height = static_cast<std::uint32_t>(th);
        if (tw == w && th == h) {
            image->rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        } else {
            image->rgba.resize(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4u);
            const double sx = static_cast<double>(w) / tw, sy = static_cast<double>(h) / th;
            for (int y = 0; y < th; ++y) {
                const int y0 = static_cast<int>(y * sy), y1 = std::max(y0 + 1, std::min(h, static_cast<int>((y + 1) * sy + 0.999)));
                for (int x = 0; x < tw; ++x) {
                    const int x0 = static_cast<int>(x * sx), x1 = std::max(x0 + 1, std::min(w, static_cast<int>((x + 1) * sx + 0.999)));
                    std::uint32_t sum[4]{};
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx)
                            for (int c = 0; c < 4; ++c) sum[c] += pixels[(static_cast<std::size_t>(yy) * w + xx) * 4u + c];
                    const std::uint32_t n = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
                    for (int c = 0; c < 4; ++c)
                        image->rgba[(static_cast<std::size_t>(y) * tw + x) * 4u + c] = static_cast<std::uint8_t>(sum[c] / n);
                }
            }
        }
        stbi_image_free(pixels);
        pixels = nullptr;
        job->image = std::move(image);
        ++g_decoded;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        g_decode_ms.store(g_decode_ms.load() + ms);
        job->state.store(1, std::memory_order_release);
        } catch (const std::bad_alloc &) {
            if (pixels != nullptr) stbi_image_free(pixels);
            std::fprintf(stderr, "[pack] out of memory decoding %s: left out\n", job->path.c_str());
            ++g_failed;
            job->state.store(2, std::memory_order_release);
        }
    }
}

std::uint32_t texture_hash(const TextureInfo &info) {
    const std::uint32_t bpp = kBitsPerPixel[info.format & 15u];
    std::uint32_t bufw = info.buffer_width & kAlignMask[info.format & 15u];
    if (bufw == 0u && info.format <= 10u && bpp != 0u) bufw = 128u / bpp;
    const auto hash_bytes = [](const std::uint8_t *p, std::uint32_t size, std::uint32_t address) -> std::uint32_t {
        switch (g_hash) {
        case Hash::Quick: return quick_hash(p, size, (address & 15u) == 0u && (size & 63u) == 0u);
        case Hash::Xxh32: return xxh32(p, size, 0xBACD7814u);
        case Hash::Xxh64: return 0u;  // not supported (packs here use quick)
        }
        return 0u;
    };
    if (bufw <= info.width) {
        const std::uint32_t pixels = bufw * info.height + (info.width - bufw);
        return hash_bytes(info.bytes, bpp * pixels / 8u, info.address);
    }
    const std::uint32_t line = bpp * info.width / 8u, stride = bpp * bufw / 8u;
    std::uint32_t result = 0u;
    for (std::uint32_t y = 0; y < info.height; ++y)
        result = (result * 11u) ^ hash_bytes(info.bytes + y * stride, line, info.address + y * stride);
    return result;
}

// PPSSPP's LookupWildcard order.
const std::string *lookup(Key key) {
    const std::uint64_t cachekey = key.cachekey;
    const std::uint32_t hash = key.hash;
    const auto find = [](const Key &k) -> const std::string * {
        const auto it = g_files.find(k);
        return it != g_files.end() ? &it->second : nullptr;
    };
    if (const auto *f = find(key)) return f;
    if (const auto *f = find({cachekey & 0xFFFFFFFFull, 0u})) return f;
    if (!g_ignore_address)
        if (const auto *f = find({cachekey, 0u})) return f;
    if (const auto *f = find({cachekey & 0xFFFFFFFFull, hash})) return f;
    if (!g_ignore_address) {
        if (const auto *f = find({cachekey & ~0xFFFFFFFFull, hash})) return f;
        if (const auto *f = find({cachekey & ~0xFFFFFFFFull, 0u})) return f;
    }
    return find({0u, hash});
}

} // namespace

std::uint32_t quick_hash(const std::uint8_t *data, std::uint32_t size, bool aligned) {
    std::uint32_t check = 0u;
    if (aligned) {
        static constexpr std::uint16_t initial[8]{0xc00bu, 0x9bd9u, 0x4b73u, 0xb651u, 0x4d9bu, 0x4309u, 0x0083u, 0x0001u};
        std::uint16_t cursor16[8]{};
        std::uint16_t cursor2[8];
        std::memcpy(cursor2, initial, sizeof(cursor2));
        for (std::uint32_t i = 0; i < size / 16u; i += 4u) {
            const std::uint8_t *p = data + i * 16u;
            for (int j = 0; j < 8; ++j) {
                std::uint16_t v;
                std::memcpy(&v, p + j * 2, 2u);
                cursor16[j] = static_cast<std::uint16_t>(cursor16[j] + static_cast<std::uint16_t>(v * cursor2[j]));
            }
            for (int j = 0; j < 4; ++j) {
                std::uint32_t c;
                std::memcpy(&c, cursor16 + j * 2, 4u);
                c ^= read32(p + 16 + j * 4);
                c += read32(p + 32 + j * 4);
                std::memcpy(cursor16 + j * 2, &c, 4u);
            }
            for (int j = 0; j < 8; ++j) {
                std::uint16_t v;
                std::memcpy(&v, p + 48 + j * 2, 2u);
                cursor16[j] = static_cast<std::uint16_t>(cursor16[j] ^ static_cast<std::uint16_t>(v * cursor2[j]));
            }
            for (int j = 0; j < 8; ++j) cursor2[j] = static_cast<std::uint16_t>(cursor2[j] + 0x2455u);
        }
        for (int j = 0; j < 4; ++j) {
            std::uint32_t c, c2;
            std::memcpy(&c, cursor16 + j * 2, 4u);
            std::memcpy(&c2, cursor2 + j * 2, 4u);
            check += c + c2;
        }
    } else {
        for (std::uint32_t i = 0; i < size / 8u; ++i) {
            check += read32(data + i * 8u);
            check ^= read32(data + i * 8u + 4u);
        }
    }
    return check;
}

std::uint32_t xxh32(const std::uint8_t *p, std::size_t len, std::uint32_t seed) {
    constexpr std::uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const std::uint8_t *end = p + len;
    std::uint32_t h;
    if (len >= 16u) {
        std::uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const std::uint8_t *limit = end - 16;
        do {
            v1 = rotl(v1 + read32(p) * P2, 13) * P1;
            v2 = rotl(v2 + read32(p + 4) * P2, 13) * P1;
            v3 = rotl(v3 + read32(p + 8) * P2, 13) * P1;
            v4 = rotl(v4 + read32(p + 12) * P2, 13) * P1;
            p += 16;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h = seed + P5;
    }
    h += static_cast<std::uint32_t>(len);
    while (p + 4 <= end) {
        h = rotl(h + read32(p) * P3, 17) * P4;
        p += 4;
    }
    while (p < end) {
        h = rotl(h + (*p) * P5, 11) * P1;
        ++p;
    }
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

bool open(const std::string &dir) {
    std::ifstream ini(dir + "/textures.ini");
    if (!ini) return false;
    g_dir = dir;
    std::string section, line;
    while (std::getline(ini, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);  // UTF-8 BOM
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            section = lower(trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (section == "options") {
            const std::string key = lower(name), v = lower(value);
            if (key == "hash") g_hash = v == "xxh32" ? Hash::Xxh32 : v == "xxh64" ? Hash::Xxh64 : Hash::Quick;
            if (key == "ignoreaddress") g_ignore_address = v == "true" || v == "1";
        } else if (section == "hashes") {
            unsigned long long cachekey = 0u;
            unsigned hash = 0u;
            int level = 0;
            if (value.empty() || std::sscanf(name.c_str(), "%16llx%8x_%d", &cachekey, &hash, &level) < 1 || level != 0) continue;
            for (char &c : value)
                if (c == '\\') c = '/';
            g_files[{cachekey, hash}] = dir + "/" + value;
        }
    }
    if (g_hash == Hash::Xxh64) {
        std::fprintf(stderr, "[pack] %s: xxh64 packs are not supported yet\n", dir.c_str());
        return false;
    }
    g_active = !g_files.empty();
    if (g_active) start_detached(decoder_main, 512u * 1024u);
    std::fprintf(stderr, "[pack] %s: %lu replacements (hash %s%s)\n", dir.c_str(), static_cast<unsigned long>(g_files.size()),
                 g_hash == Hash::Quick ? "quick" : "xxh32", g_ignore_address ? ", address ignored" : "");
    return g_active;
}

bool active() noexcept { return g_active; }

const std::string *match(const TextureInfo &info) {
    if (!g_active || info.bytes == nullptr) return nullptr;
    ++g_lookups;
    std::uint64_t cachekey = (static_cast<std::uint64_t>(info.address & 0x3FFFFFFFu) << 32u) | info.dim;
    if ((info.format & 4u) != 0u) {
        const std::uint32_t clut_hash = info.clut != nullptr ? xxh32(info.clut, info.clut_bytes, 0xC0108888u) : 0u;
        cachekey ^= clut_hash ^ info.clut_format;
    }
    if (g_ignore_address) cachekey &= 0xFFFFFFFFull;
    const std::uint32_t hash = texture_hash(info);
    const std::string *path = lookup({cachekey, hash});
    static const bool log_keys = std::getenv("PSPRECOMP_BURNOUT_PACK_KEYS") != nullptr;
    if (log_keys)
        std::fprintf(stderr, "[pack] key %016llx%08x %ux%u fmt %u at %08x%s%s\n", static_cast<unsigned long long>(cachekey), hash,
                     info.width, info.height, info.format, info.address, path != nullptr ? " -> " : "", path != nullptr ? path->c_str() : "");
    if (path != nullptr) ++g_found;
    return path;
}

std::shared_ptr<Pending> request(const std::string &path) {
    auto pending = std::make_shared<Pending>();
    pending->path = path;
    {
        std::lock_guard lock(g_mutex);
        g_queue.push_back(pending);
    }
    return pending;
}

std::unique_ptr<Image> take(Pending &pending) {
    if (pending.state.load(std::memory_order_acquire) != 1) return nullptr;
    return std::move(pending.image);
}

bool failed(const Pending &pending) noexcept { return pending.state.load(std::memory_order_acquire) == 2; }

std::string report() {
    char line[200];
    std::snprintf(line, sizeof(line), "texture pack: %llu lookups, %llu replaced, %llu decoded (%.0f ms), %llu failed",
                  static_cast<unsigned long long>(g_lookups.load()), static_cast<unsigned long long>(g_found.load()),
                  static_cast<unsigned long long>(g_decoded.load()), g_decode_ms.load(), static_cast<unsigned long long>(g_failed.load()));
    const int skipped = g_skipped_low.load();
    if (skipped > 0) return std::string(line) + ", " + std::to_string(skipped) + " left out (memory low)";
    return line;
}

} // namespace burnout::texture_pack
