#include "system_core.hpp"
#include "core_load.hpp"
#include "audio_out.hpp"

#include "thread_sync.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

#if defined(__vita__)
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <thread>
#endif

namespace burnout::audio {

namespace {

constexpr std::uint32_t kChannels = 8u;
constexpr std::uint32_t kGrain = 512u;               // frames per host output call
constexpr std::uint32_t kCapacity = 16384u;          // frames per channel FIFO (~370 ms)
constexpr std::uint32_t kPrebuffer = 2048u;          // frames queued before a channel plays (~45 ms)
constexpr std::uint32_t kTarget = 6144u;             // queue the pacing aims for (~140 ms)

struct Fifo {
    std::vector<std::int16_t> samples = std::vector<std::int16_t>(kCapacity * 2u);
    std::uint32_t read{}, count{};  // in frames
    bool playing{};                 // past the prebuffer; back to false on an underrun
};

bool g_enabled{};
SyncMutex g_mutex{"audio_out"};  // the emulation runs on the SCE main thread
std::array<Fifo, kChannels> g_fifos;
Counters g_counters;
std::array<std::FILE *, kChannels> g_dumps{};
std::string g_dump_dir;

// One grain of the channels mixed (the output threads).
void mix_grain(std::int16_t *out) {
    std::array<std::int32_t, kGrain * 2u> mix{};
    {
        std::lock_guard lock(g_mutex);
        for (Fifo &fifo : g_fifos) {
            if (!fifo.playing && fifo.count >= kPrebuffer) fifo.playing = true;
            if (!fifo.playing) continue;
            const std::uint32_t n = std::min(fifo.count, kGrain);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint32_t at = (fifo.read + i) % kCapacity;
                mix[i * 2u] += fifo.samples[at * 2u];
                mix[i * 2u + 1u] += fifo.samples[at * 2u + 1u];
            }
            fifo.read = (fifo.read + n) % kCapacity;
            fifo.count -= n;
            if (n < kGrain) {
                g_counters.underrun_frames += kGrain - n;
                fifo.playing = false;
            }
        }
        g_counters.played_frames += kGrain;
    }
    for (std::uint32_t i = 0; i < kGrain * 2u; ++i) out[i] = static_cast<std::int16_t>(std::clamp(mix[i], -32768, 32767));
}

#if defined(__vita__)
int g_port = -1;
volatile bool g_stop{};

void *output_thread(void *core_mask) {
    const bool system_core = move_to_system_core(static_cast<int>(reinterpret_cast<std::uintptr_t>(core_mask)));
    std::fprintf(stderr, "[audio] output thread on %s\n", system_core ? "the system core (3)" : "its usual core");
    // Above the GL thread sharing the core: a late output call is a click.
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
    core_load::register_current_thread("audio_out");
    std::array<std::int16_t, kGrain * 2u> out{};
    while (!g_stop) {
        mix_grain(out.data());
        sceAudioOutOutput(g_port, out.data());
    }
    return nullptr;
}
#elif defined(_WIN32)
// The playable PC build (start_output): waveOut, 8 grains queued (~90 ms).
HWAVEOUT g_wave{};
std::atomic<bool> g_wave_stop{false};
std::thread g_wave_thread;

void wave_thread(HANDLE done) {
    constexpr int kBuffers = 8;
    std::array<std::array<std::int16_t, kGrain * 2u>, kBuffers> buffers{};
    std::array<WAVEHDR, kBuffers> headers{};
    for (int i = 0; i < kBuffers; ++i) {
        headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
        headers[i].dwBufferLength = kGrain * 4u;
        waveOutPrepareHeader(g_wave, &headers[i], sizeof(WAVEHDR));
        headers[i].dwFlags |= WHDR_DONE;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!g_wave_stop) {
        bool wrote = false;
        for (int i = 0; i < kBuffers; ++i) {
            if ((headers[i].dwFlags & WHDR_DONE) == 0u) continue;
            mix_grain(buffers[i].data());
            headers[i].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            waveOutWrite(g_wave, &headers[i], sizeof(WAVEHDR));
            wrote = true;
        }
        if (!wrote) WaitForSingleObject(done, 20);
    }
    waveOutReset(g_wave);
    for (auto &header : headers) waveOutUnprepareHeader(g_wave, &header, sizeof(WAVEHDR));
    waveOutClose(g_wave);
    CloseHandle(done);
}
#endif

} // namespace

bool start(int core, const std::string &dump_dir, std::string &error) {
#if defined(__vita__)
    (void)dump_dir;
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, static_cast<int>(kGrain), 44100, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        char text[64];
        std::snprintf(text, sizeof(text), "sceAudioOutOpenPort failed: 0x%08X", static_cast<unsigned>(g_port));
        error = text;
        return false;
    }
    int volume[2]{SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         volume);
    const int masks[3]{SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, output_thread,
                       reinterpret_cast<void *>(static_cast<std::uintptr_t>(masks[std::clamp(core, 0, 2)]))) != 0) {
        error = "pthread_create failed for the audio thread";
        return false;
    }
    pthread_detach(thread);
    g_enabled = true;
    return true;
#else
    (void)core;
    (void)error;
    g_dump_dir = dump_dir;
    g_enabled = !dump_dir.empty();
    return true;
#endif
}

#if defined(_WIN32) && !defined(__vita__)
bool start_output(std::string &error) {
    HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 44100 * 4;
    const MMRESULT result = waveOutOpen(&g_wave, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(done), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        CloseHandle(done);
        error = "waveOutOpen failed (" + std::to_string(result) + ")";
        return false;
    }
    g_dump_dir.clear();
    g_enabled = true;
    g_wave_thread = std::thread(wave_thread, done);
    return true;
}
#endif

void stop() {
#if defined(__vita__)
    g_stop = true;
#elif defined(_WIN32)
    if (g_wave_thread.joinable()) {
        g_wave_stop = true;
        g_wave_thread.join();
    }
#endif
    for (std::FILE *&file : g_dumps)
        if (file != nullptr) {
            std::fclose(file);
            file = nullptr;
        }
    g_enabled = false;
}

bool enabled() noexcept { return g_enabled; }

void submit(std::uint32_t channel, const std::int16_t *samples, std::uint32_t frames, bool stereo, std::uint32_t left,
            std::uint32_t right) {
    if (!g_enabled || channel >= kChannels || samples == nullptr) return;
    const auto scale = [](std::int32_t sample, std::uint32_t volume) {
        return static_cast<std::int16_t>(std::clamp((sample * static_cast<std::int32_t>(std::min(volume, 0xFFFFu))) >> 15, -32768, 32767));
    };
    if (!g_dump_dir.empty()) {
        std::FILE *&file = g_dumps[channel];
        if (file == nullptr) {
            const std::string path = g_dump_dir + "/channel_" + std::to_string(channel) + ".raw";
            file = std::fopen(path.c_str(), "wb");
            if (file == nullptr) return;
        }
        std::vector<std::int16_t> out(static_cast<std::size_t>(frames) * 2u);
        for (std::uint32_t i = 0; i < frames; ++i) {
            out[i * 2u] = scale(samples[stereo ? i * 2u : i], left);
            out[i * 2u + 1u] = scale(samples[stereo ? i * 2u + 1u : i], right);
        }
        std::fwrite(out.data(), sizeof(std::int16_t), out.size(), file);
        return;
    }
    std::lock_guard lock(g_mutex);
    Fifo &fifo = g_fifos[channel];
    g_counters.submitted_frames += frames;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (fifo.count == kCapacity) {  // the output fell behind: drop the oldest
            fifo.read = (fifo.read + 1u) % kCapacity;
            --fifo.count;
            ++g_counters.dropped_frames;
        }
        const std::uint32_t at = (fifo.read + fifo.count) % kCapacity;
        fifo.samples[at * 2u] = scale(samples[stereo ? i * 2u : i], left);
        fifo.samples[at * 2u + 1u] = scale(samples[stereo ? i * 2u + 1u : i], right);
        ++fifo.count;
    }
}

double pace(std::uint32_t channel) {
    if (!g_enabled || !g_dump_dir.empty() || channel >= kChannels) return 1.0;
    std::lock_guard lock(g_mutex);
    const double fill = static_cast<double>(g_fifos[channel].count) / static_cast<double>(kTarget);
    return std::clamp(fill, 0.25, 1.5);
}

Counters counters() {
    std::lock_guard lock(g_mutex);
    return g_counters;
}

} // namespace burnout::audio
