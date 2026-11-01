#include "workload_stats.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace burnout::workload {

namespace {

struct FrameStats {
    std::uint64_t lists{}, commands{}, state_commands{};
    std::uint64_t draws{}, vertices{}, vertex_bytes{}, index_bytes{};
    std::uint64_t through_draws{}, skinned_draws{}, morph_draws{}, lit_draws{}, textured_draws{}, indexed_draws{};
    std::uint64_t clear_draws{}, patches{}, patch_points{}, bone_words{};
    std::uint64_t texture_changes{}, distinct_textures{}, texture_bytes{}, uploaded_texture_bytes{}, uploaded_textures{};
    std::uint64_t vram_textures{}, swizzled_textures{}, paletted_textures{}, dxt_textures{};
    std::uint64_t clut_loads{}, clut_bytes{}, transfers{}, transfer_bytes{};
    std::uint64_t render_targets{}, render_target_switches{};
};

struct GeState {
    std::uint32_t vaddr{}, iaddr{}, vtype{};
    bool texture_enable{}, lighting{}, clear_mode{};
    std::uint32_t fb_ptr{}, fb_width{};
    std::uint32_t tex_addr{}, tex_bufw{}, tex_size{}, tex_mode{}, tex_format{};
    std::uint32_t clut_addr{}, clut_upper{}, clut_format{};
    std::uint32_t trx_size{};
    std::uint64_t last_texture_key{~0ull};
    std::uint32_t last_fb{~0u};
};

struct Voice {
    std::uint32_t size{};
    std::uint32_t pitch{4096u};
    bool loop{}, on{};
    std::uint64_t key_on_us{};
};

bool g_collecting = false;
bool g_window = false;
GeState g_ge;
FrameStats g_frame;
std::vector<FrameStats> g_frames;
std::unordered_map<std::uint64_t, std::uint64_t> g_frame_textures;  // key -> bytes, this frame
std::set<std::uint32_t> g_frame_targets;
std::unordered_map<std::uint64_t, std::uint64_t> g_texture_content;  // key -> content hash, all time
std::unordered_map<std::uint64_t, std::uint64_t> g_window_textures;  // key -> bytes, whole window

std::array<Voice, 32> g_voices{};
std::uint64_t g_sas_calls{}, g_sas_active_sum{}, g_sas_active_max{}, g_key_ons{};
double g_sas_samples{};
std::uint64_t g_atrac_decodes{};
std::vector<std::string> g_atrac_streams;
std::array<std::uint64_t, 256> g_opcodes{};        // window totals per GE opcode
std::array<std::uint64_t, 256> g_breakers{};       // opcodes seen between two draws
std::uint64_t g_mergeable_draws{};                 // draws with only VADDR between them and the previous one
std::array<bool, 256> g_since_draw{};

constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t to) { return to == 0u ? value : (value + to - 1u) / to * to; }

// Vertex size as the GE reads it: weights, uv, colour, normal, position, each
// aligned to its element size, the total aligned to the largest element.
std::uint32_t vertex_size(std::uint32_t vtype) {
    static constexpr std::uint32_t kElement[4] = {0u, 1u, 2u, 4u};
    const std::uint32_t tc = vtype & 3u, color = (vtype >> 2u) & 7u, normal = (vtype >> 5u) & 3u;
    const std::uint32_t position = (vtype >> 7u) & 3u, weight = (vtype >> 9u) & 3u;
    const std::uint32_t weights = ((vtype >> 14u) & 7u) + 1u;
    std::uint32_t size = 0u, biggest = 1u;
    const auto add = [&](std::uint32_t element, std::uint32_t count) {
        if (element == 0u) return;
        size = align_up(size, element) + element * count;
        biggest = std::max(biggest, element);
    };
    if (weight != 0u) add(kElement[weight], weights);
    if (tc != 0u) add(kElement[tc], 2u);
    if (color >= 4u) add(color == 7u ? 4u : 2u, 1u);
    if (normal != 0u) add(kElement[normal], 3u);
    add(position == 0u ? 4u : kElement[position], 3u);
    return align_up(size, biggest);
}

std::uint64_t hash_bytes(const std::uint8_t *data, std::size_t length) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    std::size_t i = 0;
    for (; i + 8u <= length; i += 8u) {
        std::uint64_t word;
        std::memcpy(&word, data + i, 8u);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29u;
    }
    for (; i < length; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
    return hash;
}

// Bits per texel for GE texture formats 0..10.
std::uint32_t texel_bits(std::uint32_t format) {
    static constexpr std::uint32_t kBits[11] = {16u, 16u, 16u, 32u, 4u, 8u, 16u, 32u, 4u, 8u, 8u};
    return format < 11u ? kBits[format] : 32u;
}

void record_texture(const psprecomp::GuestMemory &memory) {
    const std::uint32_t format = g_ge.tex_format & 0xFu;
    const std::uint32_t address = (g_ge.tex_addr & 0x00FFFFF0u) | ((g_ge.tex_bufw & 0x000F0000u) << 8u);
    const std::uint32_t log_w = g_ge.tex_size & 0xFu, log_h = (g_ge.tex_size >> 8u) & 0xFu;
    const std::uint32_t width = 1u << std::min(log_w, 9u), height = 1u << std::min(log_h, 9u);
    const std::uint32_t bufw = std::max<std::uint32_t>(g_ge.tex_bufw & 0x7FFu, width);
    const bool paletted = format >= 4u && format <= 7u;
    const std::uint32_t clut = paletted ? ((g_ge.clut_addr & 0x00FFFFF0u) | ((g_ge.clut_upper & 0x000F0000u) << 8u)) : 0u;
    const std::uint64_t key = static_cast<std::uint64_t>(address) ^ (static_cast<std::uint64_t>(format) << 28u) ^
                              (static_cast<std::uint64_t>(log_w) << 32u) ^ (static_cast<std::uint64_t>(log_h) << 36u) ^
                              (static_cast<std::uint64_t>(clut >> 4u) << 40u);
    if (key != g_ge.last_texture_key) ++g_frame.texture_changes;
    g_ge.last_texture_key = key;
    if (g_frame_textures.count(key) != 0u) return;

    const std::uint64_t bytes = static_cast<std::uint64_t>(format >= 8u ? width : bufw) * height * texel_bits(format) / 8u;
    g_frame_textures.emplace(key, bytes);
    g_window_textures.emplace(key, bytes);
    ++g_frame.distinct_textures;
    g_frame.texture_bytes += bytes;
    if ((g_ge.tex_mode & 1u) != 0u) ++g_frame.swizzled_textures;
    if (paletted) ++g_frame.paletted_textures;
    if (format >= 8u) ++g_frame.dxt_textures;

    // A texture cache uploads (and converts) whatever it has not seen with this
    // exact content. VRAM textures are render targets the GPU already owns.
    const std::uint8_t *data = memory.raw_pointer(address & 0x0FFFFFFFu, static_cast<std::size_t>(bytes));
    if (data == nullptr) { ++g_frame.vram_textures; return; }
    std::uint64_t content = hash_bytes(data, static_cast<std::size_t>(bytes));
    if (paletted) {
        const std::uint32_t clut_bytes = (format == 4u ? 16u : 256u) * ((g_ge.clut_format & 3u) == 3u ? 4u : 2u);
        if (const std::uint8_t *palette = memory.raw_pointer(clut & 0x0FFFFFFFu, clut_bytes))
            content ^= hash_bytes(palette, clut_bytes) * 31u;
    }
    auto [it, inserted] = g_texture_content.emplace(key, content);
    if (inserted || it->second != content) {
        it->second = content;
        ++g_frame.uploaded_textures;
        g_frame.uploaded_texture_bytes += bytes;
    }
}

void record_draw(const psprecomp::GuestMemory &memory, std::uint32_t data) {
    const std::uint32_t count = data & 0xFFFFu;
    const std::uint32_t vtype = g_ge.vtype;
    const std::uint32_t size = vertex_size(vtype);
    const std::uint32_t morphs = ((vtype >> 18u) & 7u) + 1u;
    const std::uint32_t index_type = (vtype >> 11u) & 3u;
    ++g_frame.draws;
    if (g_window) {
        bool only_vaddr = true;
        for (std::uint32_t op = 0; op < 256u; ++op)
            if (g_since_draw[op]) { ++g_breakers[op]; if (op != 0x01u) only_vaddr = false; }
        if (only_vaddr) ++g_mergeable_draws;
    }
    g_since_draw.fill(false);
    g_frame.vertices += count;
    if ((vtype & 0x00800000u) != 0u) ++g_frame.through_draws;
    if (((vtype >> 9u) & 3u) != 0u) ++g_frame.skinned_draws;
    if (morphs > 1u) ++g_frame.morph_draws;
    if (g_ge.lighting) ++g_frame.lit_draws;
    if (g_ge.clear_mode) ++g_frame.clear_draws;

    std::uint32_t referenced = count;
    if (index_type != 0u) {
        ++g_frame.indexed_draws;
        const std::uint32_t index_size = index_type == 1u ? 1u : (index_type == 2u ? 2u : 4u);
        const std::uint32_t address = g_ge.iaddr & 0x0FFFFFFFu;
        if (const std::uint8_t *indices = memory.raw_pointer(address, static_cast<std::size_t>(count) * index_size)) {
            std::uint32_t low = ~0u, high = 0u;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t value = indices[i * index_size];
                if (index_size >= 2u) value |= static_cast<std::uint32_t>(indices[i * index_size + 1u]) << 8u;
                if (index_size == 4u)
                    value |= (static_cast<std::uint32_t>(indices[i * 4u + 2u]) << 16u) |
                             (static_cast<std::uint32_t>(indices[i * 4u + 3u]) << 24u);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            referenced = count == 0u ? 0u : high - low + 1u;
        }
        g_frame.index_bytes += static_cast<std::uint64_t>(count) * index_size;
        g_ge.iaddr += count * index_size;
    } else {
        g_ge.vaddr += count * size * morphs;
    }
    g_frame.vertex_bytes += static_cast<std::uint64_t>(referenced) * size * morphs;

    const std::uint32_t fb = (g_ge.fb_ptr & 0x00FFFFF0u) | ((g_ge.fb_width & 0x00FF0000u) << 8u);
    if (fb != g_ge.last_fb) {
        if (g_ge.last_fb != ~0u) ++g_frame.render_target_switches;
        g_ge.last_fb = fb;
    }
    g_frame_targets.insert(fb);
    if (g_ge.texture_enable && !g_ge.clear_mode && (vtype & 3u) != 0u) {
        ++g_frame.textured_draws;
        record_texture(memory);
    }
}

struct Summary {
    double avg{}, p50{}, p99{}, max{};
};

template <typename Field>
Summary summarise(Field field) {
    std::vector<double> values;
    values.reserve(g_frames.size());
    for (const auto &frame : g_frames) values.push_back(static_cast<double>(field(frame)));
    Summary s;
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const double v : values) sum += v;
    s.avg = sum / static_cast<double>(values.size());
    s.p50 = values[values.size() / 2u];
    s.p99 = values[std::min(values.size() - 1u, values.size() * 99u / 100u)];
    s.max = values.back();
    return s;
}

} // namespace

void set_collecting(bool enabled) noexcept { g_collecting = enabled; }
void set_window_open(bool open) noexcept { g_window = open; }
bool collecting() noexcept { return g_collecting; }

void ge_list_submitted() {
    if (g_collecting) ++g_frame.lists;
}

void ge_command(const psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t word, std::uint32_t base,
                std::uint32_t offset) {
    if (!g_collecting) return;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    ++g_frame.commands;
    (void)pc;
    if (g_window) ++g_opcodes[op];
    if (op != 0x04u) g_since_draw[op] = true;
    switch (op) {
    case 0x01u: g_ge.vaddr = ((base | data) + offset) & 0x0FFFFFFFu; break;
    case 0x02u: g_ge.iaddr = ((base | data) + offset) & 0x0FFFFFFFu; break;
    case 0x04u: record_draw(memory, data); return;
    case 0x05u:
    case 0x06u: {
        const std::uint32_t points = (data & 0xFFu) * ((data >> 8u) & 0xFFu);
        ++g_frame.patches;
        g_frame.patch_points += points;
        g_frame.vertex_bytes += static_cast<std::uint64_t>(points) * vertex_size(g_ge.vtype);
        return;
    }
    case 0x12u: g_ge.vtype = data; break;
    case 0x17u: g_ge.lighting = (data & 1u) != 0u; break;
    case 0x1Eu: g_ge.texture_enable = (data & 1u) != 0u; break;
    case 0x2Bu: ++g_frame.bone_words; break;
    case 0x9Cu: g_ge.fb_ptr = data; break;
    case 0x9Du: g_ge.fb_width = data; break;
    case 0xA0u: g_ge.tex_addr = data; break;
    case 0xA8u: g_ge.tex_bufw = data; break;
    case 0xB0u: g_ge.clut_addr = data; break;
    case 0xB1u: g_ge.clut_upper = data; break;
    case 0xB8u: g_ge.tex_size = data; break;
    case 0xC2u: g_ge.tex_mode = data; break;
    case 0xC3u: g_ge.tex_format = data; break;
    case 0xC4u: ++g_frame.clut_loads; g_frame.clut_bytes += (data & 0x3Fu) * 32u; break;
    case 0xC5u: g_ge.clut_format = data; break;
    case 0xD3u: g_ge.clear_mode = (data & 1u) != 0u; break;
    case 0xEAu: {
        const std::uint32_t width = (g_ge.trx_size & 0x3FFu) + 1u, height = ((g_ge.trx_size >> 10u) & 0x3FFu) + 1u;
        ++g_frame.transfers;
        g_frame.transfer_bytes += static_cast<std::uint64_t>(width) * height * ((data & 1u) != 0u ? 4u : 2u);
        break;
    }
    case 0xEEu: g_ge.trx_size = data; break;
    default: break;
    }
    if (op != 0x00u && (op < 0x08u || op > 0x14u)) ++g_frame.state_commands;
}

void end_frame(const psprecomp::GuestMemory &) {
    if (!g_collecting) return;
    g_frame.render_targets = g_frame_targets.size();
    if (g_window) g_frames.push_back(g_frame);
    g_frame = FrameStats{};
    g_frame_textures.clear();
    g_frame_targets.clear();
}

void sas_set_voice(std::uint32_t voice, std::uint32_t size, bool loop) {
    if (!g_collecting || voice >= g_voices.size()) return;
    g_voices[voice].size = size;
    g_voices[voice].loop = loop;
}

void sas_set_pitch(std::uint32_t voice, std::uint32_t pitch) {
    if (g_collecting && voice < g_voices.size()) g_voices[voice].pitch = pitch;
}

void sas_key_on(std::uint32_t voice, std::uint64_t time_us) {
    if (!g_collecting || voice >= g_voices.size()) return;
    g_voices[voice].on = true;
    g_voices[voice].key_on_us = time_us;
    if (g_window) ++g_key_ons;
}

void sas_key_off(std::uint32_t voice) {
    if (g_collecting && voice < g_voices.size()) g_voices[voice].on = false;
}

// A voice plays its VAG (16-byte blocks of 28 samples) at pitch/4096 x 44.1 kHz
// until key-off, or to the end when it does not loop.
void sas_core(std::uint64_t time_us) {
    if (!g_collecting || !g_window) return;
    std::uint64_t active = 0u;
    for (const auto &voice : g_voices) {
        if (!voice.on || voice.size == 0u) continue;
        const double rate = 44100.0 * static_cast<double>(std::max<std::uint32_t>(voice.pitch, 1u)) / 4096.0;
        const double length_us = static_cast<double>(voice.size / 16u * 28u) / rate * 1e6;
        if (!voice.loop && static_cast<double>(time_us - voice.key_on_us) > length_us) continue;
        ++active;
        g_sas_samples += rate;  // averaged over calls: samples per second
    }
    ++g_sas_calls;
    g_sas_active_sum += active;
    g_sas_active_max = std::max(g_sas_active_max, active);
}

void atrac_set_data(const psprecomp::GuestMemory &memory, std::uint32_t buffer, std::uint32_t size) {
    if (!g_collecting) return;
    const std::uint8_t *data = memory.raw_pointer(buffer, std::min<std::uint32_t>(size, 4096u));
    std::string text = "unknown";
    if (data != nullptr && size >= 12u && std::memcmp(data, "RIFF", 4u) == 0) {
        for (std::uint32_t at = 12u; at + 24u <= std::min<std::uint32_t>(size, 4096u);) {
            std::uint32_t chunk;
            std::memcpy(&chunk, data + at + 4u, 4u);
            if (std::memcmp(data + at, "fmt ", 4u) == 0) {
                std::uint16_t codec, channels;
                std::uint32_t rate, bytes_per_second;
                std::memcpy(&codec, data + at + 8u, 2u);
                std::memcpy(&channels, data + at + 10u, 2u);
                std::memcpy(&rate, data + at + 12u, 4u);
                std::memcpy(&bytes_per_second, data + at + 16u, 4u);
                char line[96];
                std::snprintf(line, sizeof(line), "%s %u ch %u Hz %u kbps",
                              codec == 0x0270u ? "ATRAC3" : (codec == 0xFFFEu ? "ATRAC3plus" : "codec?"),
                              static_cast<unsigned>(channels), static_cast<unsigned>(rate),
                              static_cast<unsigned>(bytes_per_second * 8u / 1000u));
                text = line;
                break;
            }
            at += 8u + chunk + (chunk & 1u);
        }
    }
    g_atrac_streams.push_back(text);
}

void atrac_decode() {
    if (g_collecting && g_window) ++g_atrac_decodes;
}

std::string report() {
    if (!g_collecting) return {};
    std::string out = "\n--- workload per frame-buffer flip (measured window, " + std::to_string(g_frames.size()) +
                      " flips) ---\n                         avg       p50       p99       max\n";
    const auto line = [&](const char *name, auto field) {
        const Summary s = summarise(field);
        char text[160];
        std::snprintf(text, sizeof(text), "%-22s %9.1f %9.0f %9.0f %9.0f\n", name, s.avg, s.p50, s.p99, s.max);
        out += text;
    };
    line("display lists", [](const FrameStats &f) { return f.lists; });
    line("GE commands", [](const FrameStats &f) { return f.commands; });
    line("  state commands", [](const FrameStats &f) { return f.state_commands; });
    line("draw calls (PRIM)", [](const FrameStats &f) { return f.draws; });
    line("  indexed", [](const FrameStats &f) { return f.indexed_draws; });
    line("  through (2D)", [](const FrameStats &f) { return f.through_draws; });
    line("  textured", [](const FrameStats &f) { return f.textured_draws; });
    line("  lit (HW lighting)", [](const FrameStats &f) { return f.lit_draws; });
    line("  skinned", [](const FrameStats &f) { return f.skinned_draws; });
    line("  morphed", [](const FrameStats &f) { return f.morph_draws; });
    line("  clear", [](const FrameStats &f) { return f.clear_draws; });
    line("bezier/spline patches", [](const FrameStats &f) { return f.patches; });
    line("vertices submitted", [](const FrameStats &f) { return f.vertices; });
    line("vertex data KiB", [](const FrameStats &f) { return f.vertex_bytes / 1024u; });
    line("index data KiB", [](const FrameStats &f) { return f.index_bytes / 1024u; });
    line("bone matrix words", [](const FrameStats &f) { return f.bone_words; });
    line("texture changes", [](const FrameStats &f) { return f.texture_changes; });
    line("distinct textures", [](const FrameStats &f) { return f.distinct_textures; });
    line("  paletted", [](const FrameStats &f) { return f.paletted_textures; });
    line("  swizzled", [](const FrameStats &f) { return f.swizzled_textures; });
    line("  DXT", [](const FrameStats &f) { return f.dxt_textures; });
    line("  in VRAM", [](const FrameStats &f) { return f.vram_textures; });
    line("texture KiB referenced", [](const FrameStats &f) { return f.texture_bytes / 1024u; });
    line("textures to upload", [](const FrameStats &f) { return f.uploaded_textures; });
    line("  upload KiB", [](const FrameStats &f) { return f.uploaded_texture_bytes / 1024u; });
    line("CLUT loads", [](const FrameStats &f) { return f.clut_loads; });
    line("block transfers", [](const FrameStats &f) { return f.transfers; });
    line("  transfer KiB", [](const FrameStats &f) { return f.transfer_bytes / 1024u; });
    line("render targets", [](const FrameStats &f) { return f.render_targets; });
    line("  target switches", [](const FrameStats &f) { return f.render_target_switches; });

    std::uint64_t window_bytes = 0u;
    for (const auto &[key, bytes] : g_window_textures) window_bytes += bytes;
    char text[256];
    std::snprintf(text, sizeof(text), "distinct textures over the window: %zu (%.1f MiB)\n", g_window_textures.size(),
                  static_cast<double>(window_bytes) / (1024.0 * 1024.0));
    out += text;

    std::vector<std::pair<std::uint64_t, std::uint32_t>> ranked;
    for (std::uint32_t op = 0; op < 256u; ++op)
        if (g_opcodes[op] != 0u) ranked.emplace_back(g_opcodes[op], op);
    std::sort(ranked.rbegin(), ranked.rend());
    const double flips = std::max<double>(1.0, static_cast<double>(g_frames.size()));
    out += "GE opcodes per flip (count, and draws preceded by it since the previous draw):\n";
    for (std::size_t i = 0; i < ranked.size() && i < 24u; ++i) {
        std::snprintf(text, sizeof(text), "  0x%02X %9.1f  breaks %9.1f\n", static_cast<unsigned>(ranked[i].second),
                      static_cast<double>(ranked[i].first) / flips,
                      static_cast<double>(g_breakers[ranked[i].second]) / flips);
        out += text;
    }
    std::snprintf(text, sizeof(text), "draws separated from the previous one only by VADDR: %.1f per flip\n",
                  static_cast<double>(g_mergeable_draws) / flips);
    out += text;

    const double seconds = static_cast<double>(g_frames.size()) / 30.0;
    out += "\n--- audio (measured window) ---\n";
    std::snprintf(text, sizeof(text),
                  "sceSasCore calls: %llu (%.1f/s), active voices avg %.1f max %llu, key-ons %llu (%.1f/s)\n"
                  "ADPCM samples to decode: %.0f/s\n",
                  static_cast<unsigned long long>(g_sas_calls), seconds > 0 ? g_sas_calls / seconds : 0.0,
                  g_sas_calls ? static_cast<double>(g_sas_active_sum) / static_cast<double>(g_sas_calls) : 0.0,
                  static_cast<unsigned long long>(g_sas_active_max), static_cast<unsigned long long>(g_key_ons),
                  seconds > 0 ? g_key_ons / seconds : 0.0,
                  g_sas_calls ? g_sas_samples / static_cast<double>(g_sas_calls) : 0.0);
    out += text;
    std::snprintf(text, sizeof(text), "ATRAC decode calls: %llu (%.1f/s)\n",
                  static_cast<unsigned long long>(g_atrac_decodes), seconds > 0 ? g_atrac_decodes / seconds : 0.0);
    out += text;
    for (const auto &stream : g_atrac_streams) out += "  atrac stream: " + stream + "\n";
    return out;
}

} // namespace burnout::workload
