#include "core_load.hpp"
#include "music.hpp"

// The player's own MP3s (trax.cpp): minimp3, public domain (CC0).
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"

#include "thread_sync.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(__vita__)
#include <pthread.h>
#else
#include <thread>
#endif

namespace burnout::music {

namespace {

constexpr std::uint32_t kHeaderBytes = 36u;
constexpr std::uint32_t kBlockSamples = 2048u;
constexpr std::uint32_t kBlockBytes = 8u + kBlockSamples;  // states, then 1024 bytes per channel
constexpr std::uint32_t kWindowBlocks = 32u;

struct Header {
    std::uint32_t riff_size{}, total{}, start{}, loop_start{}, loop_end{}, block_samples{}, blocks{};
};

// The next window of a track, read by the prefetch thread with its own file
// handle while the current one plays, so the emulation thread does not wait
// for the memory card.
struct Ahead {
    std::FILE *file{};
    std::vector<std::uint8_t> data;
    std::uint32_t first{UINT32_MAX}, count{};
    bool busy{};   // queued or being read
    bool ready{};  // data holds blocks [first, first + count)
};

// An added MP3 (2026-11-14, trax.cpp): decoded frame by frame, resampled to
// 44.1 kHz stereo.
struct Mp3 {
    mp3dec_t decoder{};
    std::vector<std::uint8_t> input = std::vector<std::uint8_t>(16384u);
    std::size_t have{}, used{};
    bool eof{};
    std::array<std::int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm{};
    int frame_samples{}, channels{2}, hz{44100};
    std::uint32_t phase{};      // 16.16 position in the current frame
    long data_start{};          // past the ID3v2 tag
    bool started{};
};

struct Track {
    std::FILE *file{};
    std::unique_ptr<Mp3> mp3;
    Header header;
    std::unique_ptr<Ahead> ahead;
    std::uint32_t position{};                  // next sample
    std::uint32_t cached_block{UINT32_MAX};
    std::array<std::int16_t, kBlockSamples * 2u> samples{};  // decoded cached block, interleaved
    // Raw blocks read ahead in one go: the emulation thread reads the memory
    // card about once a second instead of twenty times.
    std::vector<std::uint8_t> window;
    std::uint32_t window_first{UINT32_MAX}, window_blocks{};
};

std::map<std::uint32_t, std::string> g_paths;  // by RIFF size
std::map<std::uint32_t, std::string> g_mp3_paths;  // added MP3s, by the RIFF size the HLE gave them
std::map<int, Track> g_tracks;
int g_next_handle{1};

constexpr std::int32_t kSteps[89]{
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr std::int32_t kIndexStep[16]{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

std::uint32_t le32(const std::uint8_t *p) { return p[0] | (p[1] << 8u) | (p[2] << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u); }

// Intel/DVI IMA ADPCM, two codes per byte, the first in the high nibble.
void decode_channel(const std::uint8_t *codes, std::int32_t predictor, std::int32_t index, std::int16_t *out) {
    for (std::uint32_t i = 0; i < kBlockSamples; ++i) {
        const std::uint8_t byte = codes[i / 2u];
        const std::uint32_t code = (i & 1u) == 0u ? byte >> 4u : byte & 15u;
        const std::int32_t step = kSteps[index];
        std::int32_t diff = step >> 3;
        if ((code & 4u) != 0u) diff += step;
        if ((code & 2u) != 0u) diff += step >> 1;
        if ((code & 1u) != 0u) diff += step >> 2;
        predictor = std::clamp((code & 8u) != 0u ? predictor - diff : predictor + diff, -32768, 32767);
        index = std::clamp(index + kIndexStep[code], 0, 88);
        out[i * 2u] = static_cast<std::int16_t>(predictor);
    }
}

SyncMutex g_ahead_mutex{"music_ahead"};
SyncCondition g_ahead_cv{"music_ahead", g_ahead_mutex};
std::deque<Ahead *> g_ahead_queue;
bool g_reader_started{};

bool read_window(std::FILE *file, std::uint32_t first, std::uint32_t count, std::vector<std::uint8_t> &data) {
    data.resize(static_cast<std::size_t>(count) * kBlockBytes);
    return std::fseek(file, static_cast<long>(kHeaderBytes + first * kBlockBytes), SEEK_SET) == 0 &&
           std::fread(data.data(), 1u, data.size(), file) == data.size();
}

void reader_loop() {
    for (;;) {
        Ahead *ahead = nullptr;
        {
            std::unique_lock lock(g_ahead_mutex);
            g_ahead_cv.wait(lock, [] { return !g_ahead_queue.empty(); });
            ahead = g_ahead_queue.front();
            g_ahead_queue.pop_front();
        }
        // Only this thread touches the window while it is busy.
        const bool ok = read_window(ahead->file, ahead->first, ahead->count, ahead->data);
        std::lock_guard lock(g_ahead_mutex);
        ahead->ready = ok;
        ahead->busy = false;
        g_ahead_cv.notify_all();
    }
}

#if defined(__vita__)
void *reader_entry(void *) {
    core_load::register_current_thread("music_read");
    reader_loop();
    return nullptr;
}
#endif

void start_reader() {
    if (g_reader_started) return;
#if defined(__vita__)
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reader_entry, nullptr) != 0) return;
    pthread_detach(thread);
#else
    std::thread(reader_loop).detach();
#endif
    g_reader_started = true;
}

// Waits until the prefetch thread is done with `ahead`.
void settle(Ahead &ahead) {
    std::unique_lock lock(g_ahead_mutex);
    g_ahead_cv.wait(lock, [&] { return !ahead.busy; });
}

// Queues the window that will be needed after the current one: the next
// blocks, or the loop start once the current window holds the loop end.
void prefetch_next(Track &t) {
    if (!t.ahead || t.window_blocks == 0u) return;
    const std::uint32_t loop_end_block = t.header.loop_end / kBlockSamples;
    std::uint32_t first = t.window_first + t.window_blocks;
    if (loop_end_block >= t.window_first && loop_end_block < t.window_first + t.window_blocks)
        first = t.header.loop_start / kBlockSamples;
    if (first >= t.header.blocks) return;
    Ahead &ahead = *t.ahead;
    settle(ahead);
    if (ahead.ready && ahead.first == first) return;
    std::lock_guard lock(g_ahead_mutex);
    ahead.first = first;
    ahead.count = std::min(kWindowBlocks, t.header.blocks - first);
    ahead.ready = false;
    ahead.busy = true;
    g_ahead_queue.push_back(&ahead);
    g_ahead_cv.notify_all();
}

bool load_block(Track &t, std::uint32_t block) {
    if (t.cached_block == block) return true;
    if (block >= t.header.blocks) return false;
    if (block < t.window_first || block >= t.window_first + t.window_blocks) {
        bool have = false;
        if (t.ahead) {
            Ahead &ahead = *t.ahead;
            settle(ahead);
            if (ahead.ready && block >= ahead.first && block < ahead.first + ahead.count) {
                t.window.swap(ahead.data);
                t.window_first = ahead.first;
                t.window_blocks = ahead.count;
                ahead.ready = false;
                ahead.first = UINT32_MAX;
                have = true;
            }
        }
        if (!have) {
            const std::uint32_t count = std::min(kWindowBlocks, t.header.blocks - block);
            if (!read_window(t.file, block, count, t.window)) return false;
            t.window_first = block;
            t.window_blocks = count;
        }
        prefetch_next(t);
    }
    const std::uint8_t *raw = t.window.data() + static_cast<std::size_t>(block - t.window_first) * kBlockBytes;
    const auto state = [&](std::uint32_t at, std::int32_t &predictor, std::int32_t &index) {
        predictor = static_cast<std::int16_t>(raw[at] | (raw[at + 1u] << 8u));
        index = std::min<std::int32_t>(raw[at + 2u], 88);
    };
    std::int32_t pl, il, pr, ir;
    state(0u, pl, il);
    state(4u, pr, ir);
    decode_channel(raw + 8u, pl, il, t.samples.data());
    decode_channel(raw + 8u + kBlockSamples / 2u, pr, ir, t.samples.data() + 1);
    t.cached_block = block;
    return true;
}

// The next MP3 frame decoded into m.pcm; false at the end of the file.
bool next_frame(std::FILE *file, Mp3 &m) {
    for (int guard = 0; guard < 64; ++guard) {
        if (m.have - m.used < 2048u && !m.eof) {
            std::memmove(m.input.data(), m.input.data() + m.used, m.have - m.used);
            m.have -= m.used;
            m.used = 0u;
            const std::size_t got = std::fread(m.input.data() + m.have, 1u, m.input.size() - m.have, file);
            m.have += got;
            if (got == 0u) m.eof = true;
        }
        if (m.have == m.used) return false;
        mp3dec_frame_info_t info{};
        const int samples = mp3dec_decode_frame(&m.decoder, m.input.data() + m.used, static_cast<int>(m.have - m.used), m.pcm.data(), &info);
        if (info.frame_bytes == 0) {
            if (m.eof) return false;
            m.used = m.have;  // no frame in what is left: read on
            continue;
        }
        m.used += static_cast<std::size_t>(info.frame_bytes);
        if (samples > 0) {
            m.frame_samples = samples;
            m.channels = info.channels;
            m.hz = info.hz > 0 ? info.hz : 44100;
            return true;
        }
    }
    return false;
}

void rewind_mp3(std::FILE *file, Mp3 &m) {
    std::fseek(file, m.data_start, SEEK_SET);
    mp3dec_init(&m.decoder);
    m.have = m.used = 0u;
    m.eof = false;
    m.frame_samples = 0;
    m.phase = 0u;
}

// Stereo 44.1 kHz frames from the MP3 (nearest sample: the game's music
// is 44.1 kHz, most MP3s too); false once the file has ended.
bool decode_mp3(Track &t, std::int16_t *out, std::uint32_t frames, std::int32_t &loops) {
    Mp3 &m = *t.mp3;
    for (std::uint32_t i = 0; i < frames; ++i) {
        while (m.frame_samples == 0 || (m.phase >> 16u) >= static_cast<std::uint32_t>(m.frame_samples)) {
            if (m.frame_samples != 0) m.phase -= static_cast<std::uint32_t>(m.frame_samples) << 16u;
            if (!next_frame(t.file, m)) {
                if (loops == 0 || !m.started) {
                    std::memset(out + i * 2u, 0, (frames - i) * 4u);
                    return false;
                }
                if (loops > 0) --loops;
                rewind_mp3(t.file, m);
                if (!next_frame(t.file, m)) {
                    std::memset(out + i * 2u, 0, (frames - i) * 4u);
                    return false;
                }
            }
            m.started = true;
        }
        const std::uint32_t at = m.phase >> 16u;
        if (m.channels >= 2) {
            out[i * 2u] = m.pcm[at * 2u];
            out[i * 2u + 1u] = m.pcm[at * 2u + 1u];
        } else {
            out[i * 2u] = out[i * 2u + 1u] = m.pcm[at];
        }
        m.phase += static_cast<std::uint32_t>((static_cast<std::uint64_t>(m.hz) << 16u) / 44100u);
    }
    return true;
}

} // namespace

void add_mp3(std::uint32_t riff_size, const std::string &path) { g_mp3_paths[riff_size] = path; }

int set_dir(const std::string &dir) {
    g_paths.clear();
    for (int n = 0; n < 100; ++n) {
        char name[32];
        std::snprintf(name, sizeof(name), "EATrax%02d.ima", n);
        const std::string path = dir + "/" + name;
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) continue;
        std::uint8_t header[kHeaderBytes];
        if (std::fread(header, 1u, sizeof(header), file) == sizeof(header) && std::memcmp(header, "BIMA", 4u) == 0)
            g_paths[le32(header + 8u)] = path;
        std::fclose(file);
    }
    return static_cast<int>(g_paths.size());
}

int open(const std::uint8_t *riff, std::uint32_t bytes) {
    if (riff == nullptr || bytes < 8u || std::memcmp(riff, "RIFF", 4u) != 0) return -1;
    if (const auto mp3 = g_mp3_paths.find(le32(riff + 4u)); mp3 != g_mp3_paths.end()) {
        std::FILE *file = std::fopen(mp3->second.c_str(), "rb");
        if (file == nullptr) return -1;
        Track track;
        track.file = file;
        track.mp3 = std::make_unique<Mp3>();
        // Past an ID3v2 tag (its size in the header), else from the start.
        std::uint8_t id3[10]{};
        if (std::fread(id3, 1u, 10u, file) == 10u && std::memcmp(id3, "ID3", 3u) == 0)
            track.mp3->data_start = 10L + ((id3[6] & 0x7FL) << 21 | (id3[7] & 0x7FL) << 14 | (id3[8] & 0x7FL) << 7 | (id3[9] & 0x7FL));
        rewind_mp3(file, *track.mp3);
        const int handle = g_next_handle++;
        g_tracks.emplace(handle, std::move(track));
        return handle;
    }
    const auto it = g_paths.find(le32(riff + 4u));
    if (it == g_paths.end()) return -1;
    std::FILE *file = std::fopen(it->second.c_str(), "rb");
    if (file == nullptr) return -1;
    std::uint8_t raw[kHeaderBytes];
    if (std::fread(raw, 1u, sizeof(raw), file) != sizeof(raw)) {
        std::fclose(file);
        return -1;
    }
    Track track;
    track.file = file;
    start_reader();
    if (g_reader_started) {
        track.ahead = std::make_unique<Ahead>();
        track.ahead->file = std::fopen(it->second.c_str(), "rb");
        if (track.ahead->file == nullptr) track.ahead.reset();
    }
    track.header = {le32(raw + 8u), le32(raw + 12u), le32(raw + 16u), le32(raw + 20u), le32(raw + 24u),
                    le32(raw + 28u), le32(raw + 32u)};
    track.position = track.header.start;
    const int handle = g_next_handle++;
    g_tracks.emplace(handle, std::move(track));
    return handle;
}

void close(int handle) {
    const auto it = g_tracks.find(handle);
    if (it == g_tracks.end()) return;
    if (it->second.ahead) {
        settle(*it->second.ahead);
        std::fclose(it->second.ahead->file);
    }
    std::fclose(it->second.file);
    g_tracks.erase(it);
}

bool decode(int handle, std::int16_t *out, std::uint32_t frames, std::int32_t &loops) {
    const auto it = g_tracks.find(handle);
    if (it == g_tracks.end()) {
        std::memset(out, 0, frames * 4u);
        return false;
    }
    Track &t = it->second;
    if (t.mp3) return decode_mp3(t, out, frames, loops);
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (t.position > t.header.loop_end || t.position >= t.header.total) {
            if (loops == 0) {
                std::memset(out + i * 2u, 0, (frames - i) * 4u);
                return false;
            }
            if (loops > 0) --loops;
            t.position = t.header.loop_start;
        }
        if (!load_block(t, t.position / kBlockSamples)) {
            std::memset(out + i * 2u, 0, (frames - i) * 4u);
            return false;
        }
        const std::uint32_t at = (t.position % kBlockSamples) * 2u;
        out[i * 2u] = t.samples[at];
        out[i * 2u + 1u] = t.samples[at + 1u];
        ++t.position;
    }
    return true;
}

} // namespace burnout::music
