// Per-frame performance counters (see perf_counters.hpp).
#include "perf_counters.hpp"

#if defined(PERF_COUNTERS)
#include "burnout_bench.hpp"  // host_now_ms
#include "thread_sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace burnout::perf {
namespace {

constexpr unsigned kWindow = 600u;
constexpr const char *kNames[] = {"t_cpu_game", "t_audio", "t_ge_translate", "vertices", "tex_uploads",
                                  "t_gl_submit", "t_swap", "draws", "state_changes"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<unsigned>(Series::Count));

#if defined(__vita__)
constexpr const char *kLogPath = "ux0:data/burnout-perf.log";
#else
constexpr const char *kLogPath = "burnout_perf.log";
#endif

struct Window {
    std::array<double, kWindow> values{};
    unsigned filled{};
    std::uint64_t windows{};
};
std::array<Window, static_cast<unsigned>(Series::Count)> g_windows;

// SyncMutex: on the Vita a std::mutex does not order the SCE main thread (the
// emulation) against the render pthreads (thread_sync.hpp); lines got mixed.
SyncMutex g_log_mutex{"perf_log"};
std::FILE *g_log{};
const double g_started = host_now_ms();

// The emulation thread's note for the next lines it writes.
std::atomic<std::uint64_t> g_vblank{0u};
std::atomic<bool> g_audio_other_core{false};

void write_text(const char *line) {
    std::lock_guard<SyncMutex> lock(g_log_mutex);
    if (g_log == nullptr) {
        g_log = std::fopen(kLogPath, "a");
        if (g_log == nullptr) return;
        std::fputs("[perf] --- new run: 600-frame windows (avg, max, worst 1% = 99th percentile) ---\n", g_log);
    }
    std::fputs(line, g_log);
    std::fflush(g_log);
}

void write_line(Series series, const Window &window) {
    std::array<double, kWindow> sorted = window.values;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (const double value : sorted) sum += value;
    const double average = sum / kWindow, maximum = sorted[kWindow - 1u];
    const double worst_1 = sorted[kWindow - kWindow / 100u];  // 99th percentile
    const bool count = series == Series::Vertices || series == Series::TextureUploads || series == Series::Draws ||
                       series == Series::StateChanges;
    char line[240];
    std::snprintf(line, sizeof(line), "[perf] %7.1f s vblank %6llu  %-14s window %3llu: avg %8.2f  max %8.2f  worst1%% %8.2f%s%s\n",
                  (host_now_ms() - g_started) / 1000.0, static_cast<unsigned long long>(g_vblank.load(std::memory_order_relaxed)),
                  kNames[static_cast<unsigned>(series)], static_cast<unsigned long long>(window.windows), average, maximum,
                  worst_1, count ? "" : " ms",
                  series == Series::Audio && g_audio_other_core.load(std::memory_order_relaxed) ? " (SAS worker, other core)" : "");
    write_text(line);
}

// Emulation thread state.
double g_blocked_ms{};
double g_last_flip{-1.0};
std::atomic<std::uint64_t> g_audio_us{0u};
std::uint64_t g_audio_us_at_flip{};

}  // namespace

void push(Series series, double value) noexcept {
    Window &window = g_windows[static_cast<unsigned>(series)];
    window.values[window.filled++] = value;
    if (window.filled == kWindow) {
        write_line(series, window);
        window.filled = 0u;
        ++window.windows;
    }
}

void emulation_blocked(double ms) noexcept { g_blocked_ms += ms; }

void note(const char *text) noexcept {
    char line[1024];
    std::snprintf(line, sizeof(line), "[perf] %s\n", text);
    write_text(line);
}

void add_audio_ms(double ms) noexcept {
    g_audio_us.fetch_add(static_cast<std::uint64_t>(ms * 1000.0), std::memory_order_relaxed);
}

void emulation_flip(std::uint64_t vblank, double /*unused*/, bool audio_other_core) noexcept {
    g_vblank.store(vblank, std::memory_order_relaxed);
    g_audio_other_core.store(audio_other_core, std::memory_order_relaxed);
    const double now = host_now_ms();
    if (g_last_flip >= 0.0) {
        push(Series::CpuGame, std::max(0.0, now - g_last_flip - g_blocked_ms));
        const std::uint64_t audio_us = g_audio_us.load(std::memory_order_relaxed);
        push(Series::Audio, static_cast<double>(audio_us - g_audio_us_at_flip) / 1000.0);
        g_audio_us_at_flip = audio_us;
    } else {
        g_audio_us_at_flip = g_audio_us.load(std::memory_order_relaxed);
    }
    g_last_flip = now;
    g_blocked_ms = 0.0;
}

}  // namespace burnout::perf
#endif
