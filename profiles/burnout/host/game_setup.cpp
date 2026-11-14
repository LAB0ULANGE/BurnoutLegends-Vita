// First-launch install from the player's disc image. GPL-2.0-or-later (uses
// PPSSPP's PrxDecrypter and the FFmpeg ATRAC3plus decoder, LGPL, through
// PPSSPP's at3_standalone).
#include "game_setup.hpp"

#include "iso_reader.hpp"
#include "third_party/PrxDecrypter.h"
#include "third_party/at3/at3_decoders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace burnout::setup {
namespace fs = std::filesystem;
namespace {

// The versions the recompiled code runs: their decrypted EBOOT.BIN have the
// same code byte for byte (ULUS10025 1.00 against ULES00125 1.01: only a
// region letter, one Riviera crash target and the build date differ in the
// data, which the port loads from the player's own executable).
struct Version {
    const char *disc_id;
    const char *sha256;  // of the decrypted EBOOT.BIN
};
constexpr Version kVersions[]{
    {"ULES00125", "89263a8f1f262971ad85761202fa6b02f4dfc86b3fbc733402ec20d0003e57c4"},  // Europe 1.01 (the code's source)
    {"ULUS10025", "166bf5a428ff5b9f46192bf80ec3f26cfff1d1833af50ea74fa81d23906e70f2"},  // USA 1.00
};
constexpr const char *kSupported = "ULES00125 (Europe) or ULUS10025 (USA)";

bool supported_disc(const std::string &id) {
    for (const Version &version : kVersions)
        if (id == version.disc_id) return true;
    return false;
}

bool supported_eboot(const std::string &sha) {
    for (const Version &version : kVersions)
        if (sha == version.sha256) return true;
    return false;
}
constexpr const char *kPartial = "install.partial";
constexpr const char *kDone = "install.ok";

bool exists(const std::string &path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && lower(s.substr(s.size() - n)) == suffix;
}

// ---------------------------------------------------------------- SHA-256
std::string sha256(const std::uint8_t *data, std::size_t size) {
    static constexpr std::uint32_t k[64]{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    std::vector<std::uint8_t> msg(data, data + size);
    msg.push_back(0x80u);
    while (msg.size() % 64u != 56u) msg.push_back(0u);
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8u;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<std::uint32_t>(msg[chunk + i * 4]) << 24 | static_cast<std::uint32_t>(msg[chunk + i * 4 + 1]) << 16 |
                   static_cast<std::uint32_t>(msg[chunk + i * 4 + 2]) << 8 | msg[chunk + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char hex[65];
    for (int i = 0; i < 8; ++i) std::snprintf(hex + i * 8, 9, "%08x", h[i]);
    return hex;
}

// ---------------------------------------------------------------- PARAM.SFO
std::string sfo_string(const std::vector<std::uint8_t> &sfo, const char *wanted) {
    if (sfo.size() < 20u || std::memcmp(sfo.data(), "\0PSF", 4) != 0) return {};
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0u;
        if (at + 4u <= sfo.size()) std::memcpy(&v, &sfo[at], 4u);
        return v;
    };
    const auto u16 = [&](std::size_t at) {
        std::uint16_t v = 0u;
        if (at + 2u <= sfo.size()) std::memcpy(&v, &sfo[at], 2u);
        return v;
    };
    const std::uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 20u + i * 16u;
        const std::size_t key_at = keys + u16(entry), value_at = values + u32(entry + 12);
        if (key_at >= sfo.size() || value_at >= sfo.size()) break;
        if (std::strcmp(reinterpret_cast<const char *>(&sfo[key_at]), wanted) != 0) continue;
        const std::uint32_t length = u32(entry + 4);
        std::string value(reinterpret_cast<const char *>(&sfo[value_at]), std::min<std::size_t>(length, sfo.size() - value_at));
        value.resize(std::strlen(value.c_str()));
        return value;
    }
    return {};
}

// ---------------------------------------------------------------- music
constexpr std::int32_t kSteps[89]{
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,    34,    37,
    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,
    1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexTable[16]{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr std::uint32_t kBlock = 2048u;

struct ImaState {
    std::int32_t predicted{};
    std::int32_t index{};
};

// Python's audioop.lin2adpcm, which made the tracks so far: first code in the
// high nibble.
void ima_encode(const std::int16_t *samples, std::uint32_t count, ImaState &state, std::uint8_t *out) {
    std::int32_t predicted = state.predicted, index = state.index, step = kSteps[index];
    for (std::uint32_t i = 0; i < count; ++i) {
        std::int32_t diff = samples[i] - predicted;
        const std::int32_t sign = diff < 0 ? 8 : 0;
        if (sign != 0) diff = -diff;
        std::int32_t delta = 0, vpdiff = step >> 3;
        if (diff >= step) { delta = 4; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { delta |= 2; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { delta |= 1; vpdiff += step; }
        predicted += sign != 0 ? -vpdiff : vpdiff;
        predicted = std::clamp(predicted, -32768, 32767);
        delta |= sign;
        index = std::clamp(index + kIndexTable[delta], 0, 88);
        step = kSteps[index];
        if ((i & 1u) == 0u) out[i / 2u] = static_cast<std::uint8_t>((delta << 4) & 0xF0);
        else out[i / 2u] = static_cast<std::uint8_t>(out[i / 2u] | (delta & 0x0F));
    }
    state.predicted = predicted;
    state.index = index;
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

} // namespace

namespace {

struct MusInfo {
    std::uint32_t riff_size{}, start{}, loop_start{}, loop_end{}, channels{2u}, block_align{};
    bool looped{};
    std::uint32_t data_at{}, data_size{};
};

// The RIFF chunks before the audio data (read piece by piece: the files are
// up to 10 MB and the Vita's heap is shared with everything else).
bool read_mus_info(std::FILE *file, MusInfo &info) {
    std::uint8_t head[12];
    if (std::fread(head, 1u, 12u, file) != 12u || std::memcmp(head, "RIFF", 4) != 0) return false;
    std::memcpy(&info.riff_size, head + 4, 4u);
    std::uint32_t pos = 12u;
    for (int guard = 0; guard < 64; ++guard) {
        std::uint8_t chunk[8];
        if (std::fseek(file, static_cast<long>(pos), SEEK_SET) != 0 || std::fread(chunk, 1u, 8u, file) != 8u) return false;
        std::uint32_t size = 0u;
        std::memcpy(&size, chunk + 4, 4u);
        if (std::memcmp(chunk, "data", 4) == 0) {
            info.data_at = pos + 8u;
            info.data_size = size;
            return info.block_align != 0u && info.channels != 0u && info.channels <= 2u;
        }
        std::vector<std::uint8_t> body(std::min<std::uint32_t>(size, 4096u));
        if (!body.empty() && std::fread(body.data(), 1u, body.size(), file) != body.size()) return false;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && body.size() >= 14u) {
            info.channels = body[2] | body[3] << 8u;
            info.block_align = body[12] | body[13] << 8u;
        } else if (std::memcmp(chunk, "fact", 4) == 0 && body.size() >= 8u) {
            std::memcpy(&info.start, &body[4], 4u);
        } else if (std::memcmp(chunk, "smpl", 4) == 0 && body.size() >= 60u) {
            std::uint32_t loops = 0u;
            std::memcpy(&loops, &body[28], 4u);
            if (loops != 0u) {
                std::memcpy(&info.loop_start, &body[44], 4u);
                std::memcpy(&info.loop_end, &body[48], 4u);
                info.looped = true;
            }
        }
        pos += 8u + size + (size & 1u);
    }
    return false;
}

std::uint64_t file_size(const std::string &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0u : static_cast<std::uint64_t>(size);
}

} // namespace

// Expected .ima size for a .mus (0 when unreadable): an existing track of that
// size was finished by an earlier, interrupted install.
std::uint64_t expected_ima_size(const std::string &mus) {
    std::FILE *file = std::fopen(mus.c_str(), "rb");
    if (file == nullptr) return 0u;
    MusInfo info;
    const bool ok = read_mus_info(file, info);
    std::fclose(file);
    if (!ok) return 0u;
    const std::uint64_t total = static_cast<std::uint64_t>(info.data_size / info.block_align) * 2048u;
    const std::uint64_t blocks = (total + kBlock - 1u) / kBlock;
    return 36u + blocks * (8u + kBlock);
}

bool convert_music(const std::string &mus, const std::string &ima) {
    std::FILE *in = std::fopen(mus.c_str(), "rb");
    if (in == nullptr) return false;
    MusInfo info;
    if (!read_mus_info(in, info) || std::fseek(in, static_cast<long>(info.data_at), SEEK_SET) != 0) {
        std::fclose(in);
        return false;
    }
    int align = static_cast<int>(info.block_align);
    ATRAC3PContext *decoder = atrac3p_alloc(static_cast<int>(info.channels), &align);
    std::FILE *out = std::fopen(ima.c_str(), "wb");
    if (decoder == nullptr || out == nullptr) {
        if (decoder != nullptr) atrac3p_free(decoder);
        if (out != nullptr) std::fclose(out);
        std::fclose(in);
        return false;
    }
    // Header first (patched at the end with the real counts), then blocks of
    // 2048 samples per channel as the frames are decoded.
    std::vector<std::uint8_t> header;
    header.insert(header.end(), {'B', 'I', 'M', 'A'});
    for (int i = 0; i < 8; ++i) put32(header, 0u);
    bool ok = std::fwrite(header.data(), 1u, header.size(), out) == header.size();

    std::vector<std::uint8_t> frame(info.block_align);
    std::vector<float> buffer_l(4096), buffer_r(4096);
    std::int16_t pending_l[kBlock], pending_r[kBlock];
    std::uint32_t pending = 0u, total = 0u, blocks = 0u;
    ImaState state_l, state_r;
    std::uint8_t block[8u + kBlock];
    const auto flush = [&](std::uint32_t count) {
        for (std::uint32_t i = count; i < kBlock; ++i) pending_l[i] = pending_r[i] = 0;
        const auto put_state = [](std::uint8_t *at, const ImaState &s) {
            const auto p = static_cast<std::uint16_t>(static_cast<std::int16_t>(s.predicted));
            at[0] = static_cast<std::uint8_t>(p);
            at[1] = static_cast<std::uint8_t>(p >> 8u);
            at[2] = static_cast<std::uint8_t>(s.index);
            at[3] = 0u;
        };
        put_state(block, state_l);
        put_state(block + 4, state_r);
        ima_encode(pending_l, kBlock, state_l, block + 8);
        ima_encode(pending_r, kBlock, state_r, block + 8 + kBlock / 2u);
        ok = ok && std::fwrite(block, 1u, sizeof(block), out) == sizeof(block);
        ++blocks;
    };
    const auto to16 = [](float v) {
        const long s = std::lrintf(v * 32768.0f);
        return static_cast<std::int16_t>(std::clamp<long>(s, -32768, 32767));
    };
    for (std::uint32_t at = 0u; ok && at + info.block_align <= info.data_size; at += info.block_align) {
        if (std::fread(frame.data(), 1u, frame.size(), in) != frame.size()) break;
        float *planes[2]{buffer_l.data(), buffer_r.data()};
        int samples = 0;
        if (atrac3p_decode_frame(decoder, planes, &samples, frame.data(), static_cast<int>(info.block_align)) < 0) continue;
        for (int i = 0; i < samples; ++i) {
            pending_l[pending] = to16(buffer_l[static_cast<std::size_t>(i)]);
            pending_r[pending] = to16(info.channels == 2u ? buffer_r[static_cast<std::size_t>(i)] : buffer_l[static_cast<std::size_t>(i)]);
            ++total;
            if (++pending == kBlock) {
                flush(kBlock);
                pending = 0u;
            }
        }
    }
    if (pending != 0u) flush(pending);
    atrac3p_free(decoder);
    std::fclose(in);
    if (total == 0u) ok = false;
    if (!info.looped) {
        info.loop_start = info.start;
        info.loop_end = total - 1u;
    }
    info.loop_end = std::min(info.loop_end, total - 1u);
    header.resize(4u);
    for (const std::uint32_t v : {1u, info.riff_size, total, info.start, info.loop_start, info.loop_end, kBlock, blocks}) put32(header, v);
    ok = ok && std::fseek(out, 0, SEEK_SET) == 0 && std::fwrite(header.data(), 1u, header.size(), out) == header.size();
    ok = (std::fclose(out) == 0) && ok;
    return ok;
}

bool installed(const std::string &root) {
    if (exists(root + kPartial)) return false;
    if (exists(root + kDone)) return true;
    // Installed by hand (the PC way, before 0.0.1's own install).
    return exists(root + "EBOOT_DECRYPTED.ELF") && exists(root + "PARAM.SFO") && exists(root + "USRDIR") && exists(root + "music_ima");
}

std::vector<std::string> find_images(const std::vector<std::string> &folders) {
    std::vector<std::string> images;
    std::error_code ec;
    const auto add = [&](const fs::path &path) {
        const std::string name = path.string();
        if (ends_with(name, ".iso") || ends_with(name, ".cso")) images.push_back(name);
    };
    for (const std::string &folder : folders) {
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) {
                std::error_code sub_ec;
                for (fs::directory_iterator sub(it->path(), sub_ec), sub_end; !sub_ec && sub != sub_end; sub.increment(sub_ec))
                    add(sub->path());
            } else {
                add(it->path());
            }
        }
        ec.clear();
    }
    return images;
}

Outcome install_from(const std::string &root, const std::string &image_path, const Progress &progress) {
    Outcome outcome;
    outcome.image = image_path;
    const auto report = [&](const std::string &step, const std::string &detail, double fraction) {
        if (progress) progress(step, detail, fraction);
    };
    auto image = iso::Image::open(image_path);
    if (image == nullptr) {
        outcome.result = Result::Failed;
        outcome.message = "Cannot read the disc image " + image_path;
        return outcome;
    }
    iso::Entry sfo_entry;
    std::vector<std::uint8_t> sfo;
    if (!image->find("PSP_GAME/PARAM.SFO", sfo_entry) || !image->read_file(sfo_entry, sfo)) {
        outcome.result = Result::Failed;
        outcome.message = "No PSP_GAME/PARAM.SFO in " + image_path;
        return outcome;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (std::FILE *marker = std::fopen((root + kPartial).c_str(), "wb")) std::fclose(marker);
    fs::remove(root + kDone, ec);

    // 1. Game files.
    std::vector<iso::Entry> files = image->list("PSP_GAME/USRDIR");
    std::uint64_t total = 0u, copied = 0u;
    for (const auto &entry : files) total += entry.directory ? 0u : entry.size;
    const std::size_t usrdir_prefix = std::strlen("PSP_GAME/");
    for (const auto &entry : files) {
        const std::string host = root + entry.path.substr(usrdir_prefix);
        if (entry.directory) {
            fs::create_directories(host, ec);
            continue;
        }
        fs::create_directories(fs::path(host).parent_path(), ec);
        const std::uint64_t before = copied;
        if (file_size(host) == entry.size && exists(host)) {  // copied by an interrupted install
            copied += entry.size;
            continue;
        }
        if (!image->copy_file(entry, host, [&](std::uint64_t done) {
                report("Copying the game files", entry.path.substr(usrdir_prefix), static_cast<double>(before + done) / std::max<double>(total, 1.0));
            })) {
            outcome.result = Result::Failed;
            outcome.message = "Cannot copy " + entry.path + " (memory card full?)";
            return outcome;
        }
        copied += entry.size;
    }
    if (std::FILE *file = std::fopen((root + "PARAM.SFO").c_str(), "wb")) {
        std::fwrite(sfo.data(), 1u, sfo.size(), file);
        std::fclose(file);
    }

    // 2. The executable.
    report("Decrypting the game", "PSP_GAME/SYSDIR/EBOOT.BIN", 0.0);
    iso::Entry eboot_entry;
    std::vector<std::uint8_t> eboot;
    if (!image->find("PSP_GAME/SYSDIR/EBOOT.BIN", eboot_entry) || !image->read_file(eboot_entry, eboot) || eboot.size() < 0x150u) {
        outcome.result = Result::Failed;
        outcome.message = "Cannot read PSP_GAME/SYSDIR/EBOOT.BIN";
        return outcome;
    }
    std::vector<std::uint8_t> elf(eboot.size(), 0u);
    if (std::memcmp(eboot.data(), "~PSP", 4) == 0) {
        const int size = pspDecryptPRX(eboot.data(), elf.data(), static_cast<u32>(eboot.size()));
        if (size <= 0) {
            outcome.result = Result::Failed;
            outcome.message = "Cannot decrypt EBOOT.BIN";
            return outcome;
        }
    } else {
        elf = eboot;  // already decrypted (a homebrew-patched image)
    }
    if (!supported_eboot(sha256(elf.data(), elf.size()))) {
        // PC study of other versions (ULUS10025...): keep their decrypted executable.
        if (const char *keep = std::getenv("PSPRECOMP_KEEP_OTHER_EBOOT"); keep != nullptr && *keep != '\0') {
            if (std::FILE *file = std::fopen(keep, "wb")) {
                std::fwrite(elf.data(), 1u, elf.size(), file);
                std::fclose(file);
            }
        }
        outcome.result = Result::WrongVersion;
        outcome.message = "This Burnout Legends (" + sfo_string(sfo, "DISC_ID") + " " + sfo_string(sfo, "DISC_VERSION") +
                          ") is not a supported version (" + kSupported + ")";
        return outcome;
    }
    if (std::FILE *file = std::fopen((root + "EBOOT_DECRYPTED.ELF").c_str(), "wb")) {
        const bool ok = std::fwrite(elf.data(), 1u, elf.size(), file) == elf.size();
        if (std::fclose(file) != 0 || !ok) {
            outcome.result = Result::Failed;
            outcome.message = "Cannot write EBOOT_DECRYPTED.ELF";
            return outcome;
        }
    }

    // 3. Music.
    const std::string music = root + "USRDIR/music";
    std::vector<std::string> tracks;
    for (fs::directory_iterator it(music, ec), end; !ec && it != end; it.increment(ec))
        if (ends_with(it->path().string(), ".mus")) tracks.push_back(it->path().filename().string());
    std::sort(tracks.begin(), tracks.end());
    fs::create_directories(root + "music_ima", ec);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        report("Converting the music", tracks[i], static_cast<double>(i) / static_cast<double>(tracks.size()));
        const std::string name = tracks[i].substr(0, tracks[i].size() - 4u) + ".ima";
        const std::uint64_t expected = expected_ima_size(music + "/" + tracks[i]);
        if (expected != 0u && file_size(root + "music_ima/" + name) == expected) continue;  // done before an interruption
        if (!convert_music(music + "/" + tracks[i], root + "music_ima/" + name)) {
            outcome.result = Result::Failed;
            outcome.message = "Cannot convert " + tracks[i];
            return outcome;
        }
    }
    report("Done", "", 1.0);
    if (std::FILE *marker = std::fopen((root + kDone).c_str(), "wb")) std::fclose(marker);
    fs::remove(root + kPartial, ec);
    outcome.result = Result::Ready;
    return outcome;
}

Outcome install(const std::string &root, const std::vector<std::string> &folders, const Progress &progress) {
    Outcome outcome;
    if (installed(root)) {
        outcome.result = Result::Ready;
        return outcome;
    }
    std::string other;  // a Burnout Legends image of another version
    // The European image first when there are several (players already
    // installed from it keep the same game).
    std::vector<std::pair<std::string, std::string>> found;  // disc id, path
    for (const std::string &path : find_images(folders)) {
        auto image = iso::Image::open(path);
        if (image == nullptr) continue;
        iso::Entry entry;
        std::vector<std::uint8_t> sfo;
        if (!image->find("PSP_GAME/PARAM.SFO", entry) || !image->read_file(entry, sfo)) continue;
        const std::string id = sfo_string(sfo, "DISC_ID");
        if (supported_disc(id)) {
            found.emplace_back(id, path);
            continue;
        }
        if (id == "ULJM05049" || id == "ULKS46027" || lower(sfo_string(sfo, "TITLE")).find("burnout legends") != std::string::npos)
            other = path + " (" + id + ")";
    }
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return (a.first == kVersions[0].disc_id) > (b.first == kVersions[0].disc_id);
    });
    if (!found.empty()) return install_from(root, found.front().second, progress);
    if (!other.empty()) {
        outcome.result = Result::WrongVersion;
        outcome.image = other;
        outcome.message = "Found " + other + ", but only " + std::string(kSupported) + " is supported";
        return outcome;
    }
    outcome.result = Result::NoImage;
    outcome.message = "No Burnout Legends ISO or CSO found (" + std::string(kSupported) + ")";
    return outcome;
}

} // namespace burnout::setup
