#include "core_load.hpp"
#include "ge_render.hpp"
#if defined(_WIN32)
#include "sampling_profiler_win.hpp"
#endif

#if defined(BURNOUT_GE_SOFTWARE)
#include "framebuffer_capture.hpp"
#include "ge_renderer.hpp"
#endif
#include "ge_gl.hpp"
#include "thread_sync.hpp"

#include "burnout_bench.hpp"
#include "burnout_hle.hpp"

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <pthread.h>
#endif

#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace burnout::render {

namespace {

Options g_options;
std::array<std::uint32_t, 256> g_commands{};
#if defined(BURNOUT_GE_SOFTWARE)
vcs::GeTransformState g_transform{};
#endif
std::uint32_t g_vertex_address{};
std::uint32_t g_index_address{};
std::uint32_t g_clut_loaded_from{};
std::uint32_t g_clut_load_word{};
std::uint64_t g_primitives{};
std::uint64_t g_failures{};
std::uint64_t g_captures{};
std::uint64_t g_transfers{};
std::uint64_t g_transfer_bytes{};
std::uint64_t g_transfers_to_ram{};
std::uint64_t g_transfer_failures{};
double g_render_ms{};
std::string g_first_error;

std::uint32_t crc32(const std::uint8_t *data, std::size_t size, std::uint32_t crc = 0u) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256u; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    return ~crc;
}

// Uncompressed (stored-deflate) PNG: no zlib dependency, a 480x272 capture is
// ~400 KiB.
void write_png(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t> &rgb) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 3u + 1u));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0u);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * width * 3,
                   rgb.begin() + static_cast<std::ptrdiff_t>(y + 1u) * width * 3);
    }
    std::vector<std::uint8_t> z{0x78u, 0x01u};
    std::uint32_t a = 1u, b = 0u;
    for (const std::uint8_t byte : raw) { a = (a + byte) % 65521u; b = (b + a) % 65521u; }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0u;) {
        const std::size_t n = std::min<std::size_t>(65535u, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1u : 0u);
        z.push_back(static_cast<std::uint8_t>(n)); z.push_back(static_cast<std::uint8_t>(n >> 8u));
        z.push_back(static_cast<std::uint8_t>(~n)); z.push_back(static_cast<std::uint8_t>(~n >> 8u));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    const std::uint32_t adler = (b << 16u) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<std::uint8_t>(adler >> s));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::uint8_t signature[8]{0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    out.write(reinterpret_cast<const char *>(signature), 8);
    auto chunk = [&](const char *type, const std::vector<std::uint8_t> &body) {
        std::vector<std::uint8_t> data(type, type + 4);
        data.insert(data.end(), body.begin(), body.end());
        const std::uint32_t size = static_cast<std::uint32_t>(body.size());
        const std::uint32_t crc = crc32(data.data(), data.size());
        const std::uint8_t len[4]{static_cast<std::uint8_t>(size >> 24u), static_cast<std::uint8_t>(size >> 16u),
                                  static_cast<std::uint8_t>(size >> 8u), static_cast<std::uint8_t>(size)};
        const std::uint8_t tail[4]{static_cast<std::uint8_t>(crc >> 24u), static_cast<std::uint8_t>(crc >> 16u),
                                   static_cast<std::uint8_t>(crc >> 8u), static_cast<std::uint8_t>(crc)};
        out.write(reinterpret_cast<const char *>(len), 4);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        out.write(reinterpret_cast<const char *>(tail), 4);
    };
    std::vector<std::uint8_t> header;
    for (const std::uint32_t v : {width, height})
        for (int s = 24; s >= 0; s -= 8) header.push_back(static_cast<std::uint8_t>(v >> s));
    header.insert(header.end(), {8u, 2u, 0u, 0u, 0u});  // 8-bit RGB
    chunk("IHDR", header);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// TRXKICK: GE block copy between RAM and VRAM (textures are uploaded this way).
// The source rows are captured when ge_scan reaches the command; the copy into
// the destination can then happen later, in command order, on the GL worker.
struct Transfer {
    std::uint32_t destination{}, destination_stride{}, destination_x{}, destination_y{};
    std::uint32_t width{}, height{}, bpp{};
    std::vector<std::uint8_t> rows;
    bool valid{};
};

// Transfer registers as of a TRXKICK (`regs` indexed by command).
Transfer snapshot_transfer_from(const psprecomp::GuestMemory &memory, const std::array<std::uint32_t, 256> &regs,
                                std::uint32_t data) {
    const auto c = [&regs](std::uint32_t index) { return regs[index] & 0x00FFFFFFu; };
    Transfer t;
    const std::uint32_t source = (c(0xB2u) & 0xFFFFF0u) | ((c(0xB3u) & 0xFF0000u) << 8u);
    const std::uint32_t source_stride = c(0xB3u) & 0x7F8u;
    t.destination = (c(0xB4u) & 0xFFFFF0u) | ((c(0xB5u) & 0xFF0000u) << 8u);
    t.destination_stride = c(0xB5u) & 0x7F8u;
    const std::uint32_t source_x = c(0xEBu) & 0x3FFu, source_y = (c(0xEBu) >> 10u) & 0x3FFu;
    t.destination_x = c(0xECu) & 0x3FFu;
    t.destination_y = (c(0xECu) >> 10u) & 0x3FFu;
    t.width = (c(0xEEu) & 0x3FFu) + 1u;
    t.height = ((c(0xEEu) >> 10u) & 0x3FFu) + 1u;
    t.bpp = (data & 1u) != 0u ? 4u : 2u;
    const std::uint32_t row_bytes = t.width * t.bpp;
    t.rows.resize(static_cast<std::size_t>(row_bytes) * t.height);
    ++g_transfers;
    if ((t.destination & 0x0F000000u) != 0x04000000u) ++g_transfers_to_ram;
    for (std::uint32_t y = 0; y < t.height; ++y) {
        const std::uint32_t from = source + ((source_y + y) * source_stride + source_x) * t.bpp;
        const std::uint8_t *src = memory.raw_pointer(from, row_bytes);
        if (src == nullptr) { ++g_transfer_failures; return t; }
        std::memcpy(&t.rows[static_cast<std::size_t>(y) * row_bytes], src, row_bytes);
    }
    g_transfer_bytes += t.rows.size();
    t.valid = true;
    return t;
}

Transfer snapshot_transfer(const psprecomp::GuestMemory &memory, std::uint32_t data) {
    return snapshot_transfer_from(memory, g_commands, data);
}

// Transfers copied at sceGeListEnQueue (early_transfers), in kick order, with
// the registers they were taken with.
struct EarlyTransfer {
    std::array<std::uint32_t, 8> regs{};  // B2 B3 B4 B5 EB EC EE and the kick
    Transfer transfer;
};
SyncMutex g_early_mutex{"ge_early"};
std::deque<EarlyTransfer> g_early;
std::array<std::uint32_t, 256> g_early_regs{};  // emulation thread: TRX registers seen at the top level
std::uint64_t g_early_used{}, g_early_missed{};

constexpr std::array<std::uint32_t, 7> kTransferRegisters{0xB2u, 0xB3u, 0xB4u, 0xB5u, 0xEBu, 0xECu, 0xEEu};

std::array<std::uint32_t, 8> transfer_key(const std::array<std::uint32_t, 256> &regs, std::uint32_t kick) {
    std::array<std::uint32_t, 8> key{};
    for (std::size_t i = 0; i < kTransferRegisters.size(); ++i) key[i] = regs[kTransferRegisters[i]] & 0x00FFFFFFu;
    key[7] = kick & 0x00FFFFFFu;
    return key;
}

// The copy early_transfers made for this kick, if the registers agree.
// A kick the pre-pass did not see leaves the queue alone; copies the front end
// never reaches are dropped when a later one matches, and the queue is
// bounded (kEarlyMax): before gl-103f a copy that never matched blocked the
// head (only 4 entries were looked at) and every later copy piled up behind
// it, one texture copy per frame, until the heap ran out (std::bad_alloc
// after ~4 min of a race on US/C1_V1).
constexpr std::size_t kEarlyMax = 32u;
bool take_early_transfer(std::uint32_t data, Transfer &out) {
    const auto key = transfer_key(g_commands, data);
    std::lock_guard lock(g_early_mutex);
    for (std::size_t i = 0; i < g_early.size(); ++i) {
        if (g_early[i].regs != key) continue;
        out = std::move(g_early[i].transfer);
        g_early.erase(g_early.begin(), g_early.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        ++g_early_used;
        return true;
    }
    ++g_early_missed;
    return false;
}

void apply_transfer(psprecomp::GuestMemory &memory, const Transfer &t) {
    if (!t.valid) return;
    const std::uint32_t row_bytes = t.width * t.bpp;
    for (std::uint32_t y = 0; y < t.height; ++y) {
        const std::uint32_t to = t.destination + ((t.destination_y + y) * t.destination_stride + t.destination_x) * t.bpp;
        std::uint8_t *dst = memory.raw_pointer(to, row_bytes);
        if (dst == nullptr) return;
        std::memcpy(dst, &t.rows[static_cast<std::size_t>(y) * row_bytes], row_bytes);
    }
}

void block_transfer(psprecomp::GuestMemory &memory, std::uint32_t data) {
    apply_transfer(memory, snapshot_transfer(memory, data));
}

// ---------------------------------------------------------------- GL worker
//
// The game submits a whole frame's display list at the flip and only peeks at
// the GE afterwards, so the frame can be drawn on another core while the
// emulation thread runs the next one. ge_scan still walks the list on the
// emulation thread (control flow, callbacks, block transfers); the worker gets
// a copy of the commands and owns the GL context. At most two frames queue up.

// A run of a cached sub-list (ge_cached_call), read by the worker from the
// cache's own immutable copy of the words instead of a copy per frame.
// Only the emulation thread may copy or drop these references: the Vita's
// libstdc++ counts them without atomics (__gthread_active_p() is false there,
// its weak __pthread_key_create is unresolved), so a reference dropped by the
// worker raced with the emulation thread and corrupted the heap (gl-53 crash
// in _malloc_r). The worker moves the runs back in g_returned_cached.
struct CachedRun {
    std::shared_ptr<const std::vector<std::uint32_t>> words;
    std::uint32_t first{}, count{}, base{}, offset{};
};

struct Job {
    enum class Kind { Commands, Present, Capture } kind{};
    std::vector<std::array<std::uint32_t, 3>> commands;
    std::vector<CachedRun> cached;  // kCachedRun entries carry their index here
    // Runs of plain commands, copied whole: a commands entry {start, count,
    // kRun} with base and offset in the two words before start.
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> records;  // snapshot records of the runs' PRIM/LOAD_CLUT, in order
    std::vector<Transfer> transfers;  // TRXKICK entries carry their index here
    gl::Snapshot snapshot;  // PRIM/LOAD_CLUT entries carry their record offset here
    bool skip{};            // frame skipped: commands followed, nothing drawn
    std::uint32_t address{};
    std::uint64_t vblank{};
};

SyncMutex g_jobs_mutex{"ge_jobs"};
SyncCondition g_jobs_cv{"ge_jobs", g_jobs_mutex};
std::deque<Job> g_jobs;
std::atomic<bool> g_capture_requested{false};
// VRAM ranges the CPU wrote (sceMpegAvcCsc movie frames): the textures there
// are uploaded again. Guarded by g_jobs_mutex, drained by the worker.
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_cpu_vram_writes;
std::size_t g_queued_command_jobs{};
bool g_worker_busy{};
bool g_worker_ready{};
bool g_worker_failed{};
std::string g_worker_error;
std::vector<std::array<std::uint32_t, 3>> g_pending;
std::vector<std::uint32_t> g_pending_words, g_pending_records;
// Buffers of finished command jobs, handed back by the worker so the next
// lists reuse warm memory instead of allocating (and freeing across cores).
struct SpareBuffers {
    std::vector<std::array<std::uint32_t, 3>> commands;
    std::vector<std::uint32_t> words, records;
    std::vector<Transfer> transfers;
};
std::vector<SpareBuffers> g_spares;  // under g_jobs_mutex
// Cached runs of finished jobs, moved (never copied or destroyed) by the
// worker; the emulation thread drops their references. Under g_jobs_mutex.
std::vector<std::vector<CachedRun>> g_returned_cached;
constexpr std::uint32_t kRun = UINT32_MAX - 1u;  // commands entry tag (offsets are multiples of 256)
constexpr std::uint32_t kCachedRun = UINT32_MAX - 2u;  // commands entry tag: {index in Job::cached, 0, kCachedRun}
std::vector<CachedRun> g_pending_cached;
std::vector<Transfer> g_pending_transfers;
gl::Snapshot g_pending_snapshot;
// Overlap: the worker draws frame N while the emulation runs frame N+1. The
// guest bytes each list reads are snapshotted at scan time for that, since the
// game rewrites them while it builds the next frame.
// PSPRECOMP_BURNOUT_GL_SYNC=1 waits for each list instead.
const bool g_async = std::getenv("PSPRECOMP_BURNOUT_GL_SYNC") == nullptr;
std::uint64_t g_flips{};
psprecomp::GuestMemory *g_memory{};

void capture_rgb(const std::vector<std::uint8_t> &rgb, std::uint64_t vblank) {
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(vblank));
    // 480x272, or the render resolution (480 * s / 8 by 272 * s / 8).
    std::uint32_t width = 480u, height = 272u;
    for (std::uint32_t s = 1u; s <= 32u; ++s)
        if (static_cast<std::size_t>(480u * s / 8u) * (272u * s / 8u) * 3u == rgb.size()) { width = 480u * s / 8u; height = 272u * s / 8u; }
    if (static_cast<std::size_t>(width) * height * 3u != rgb.size()) return;
    write_png(g_options.capture_dir / name, width, height, rgb);
    ++g_captures;
}


// Sampling thread: histogram of gl::phase every ~1 ms.
std::array<std::atomic<std::uint32_t>, 16> g_phase_samples{};

std::array<std::atomic<std::uint32_t>, 16> g_emulation_samples{};

void sample_phases() {
    for (std::uint32_t tick = 1u;; ++tick) {
        ++g_phase_samples[gl::phase & 15u];
        ++g_emulation_samples[gl::emulation_phase & 15u];
        if (tick % 250u == 0u) gl::flush_traces();
        host_sleep_ms(1.0);  // shares a core with the GL thread: keep the wakeups rare
    }
}

void trace_op_costs() {
    std::string line = "worker phases (% of samples):";
    std::uint32_t total = 0;
    for (auto &n : g_phase_samples) total += n.load();
    for (int i = 0; i < 16; ++i) {
        char item[24];
        std::snprintf(item, sizeof(item), " %d:%.1f", i, total ? 100.0 * g_phase_samples[i].load() / total : 0.0);
        line += item;
        g_phase_samples[i] = 0u;
    }
    gl::trace(line.c_str());
    line = "emulation phases (% of samples):";
    total = 0;
    for (auto &n : g_emulation_samples) total += n.load();
    for (int i = 0; i < 8; ++i) {
        char item[24];
        std::snprintf(item, sizeof(item), " %d:%.1f", i, total ? 100.0 * g_emulation_samples[i].load() / total : 0.0);
        line += item;
        g_emulation_samples[i] = 0u;
    }
    gl::trace(line.c_str());
}

#if defined(_WIN32)
// PSPRECOMP_BURNOUT_PROFILE_WORKER=path samples the decode thread (PC).
std::unique_ptr<SamplingProfiler> g_worker_profiler;
std::atomic<bool> g_worker_profiling{true};
#endif

void worker_loop() {
#if defined(_WIN32)
    if (std::getenv("PSPRECOMP_BURNOUT_PROFILE_WORKER") != nullptr)
        g_worker_profiler = std::make_unique<SamplingProfiler>(g_worker_profiling);
#endif
    {
        std::string error;
        const bool ok = gl::initialize(error);
        std::lock_guard lock(g_jobs_mutex);
        g_worker_ready = true;
        g_worker_failed = !ok;
        g_worker_error = error;
        g_jobs_cv.notify_all();
        if (!ok) return;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cpu_writes;
    for (;;) {
        Job job;
        const double waiting = host_now_ms();
        gl::phase = 12u;
        {
            std::unique_lock lock(g_jobs_mutex);
            g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_worker_busy = true;
            cpu_writes.swap(g_cpu_vram_writes);
        }
        for (const auto &[address, size] : cpu_writes) gl::invalidate_textures(address, size);
        cpu_writes.clear();
        const double started = host_now_ms();
        gl::phase = 0u;
        switch (job.kind) {
        case Job::Kind::Commands: {
            gl::set_drawing(!job.skip);
            std::size_t next_record = 0u;
            for (const auto &c : job.commands) {
                if (c[2] == kCachedRun) {
                    const CachedRun &run = job.cached[c[0]];
                    gl::ge_run(*g_memory, run.words->data() + run.first, run.count, run.base, run.offset, job.snapshot,
                               job.records.data(), next_record);
                    continue;
                }
                if (c[2] == kRun) {
                    const std::uint32_t *words = job.words.data() + c[0];
                    gl::ge_run(*g_memory, words, c[1], words[-2], words[-1], job.snapshot, job.records.data(), next_record);
                    continue;
                }
                // A TRXKICK carries the index of its snapshot instead of base/offset.
                const std::uint32_t op = c[0] >> 24u;
                const bool snapshot = c[2] == UINT32_MAX;
                if (snapshot && op == 0xEAu) {
                    const Transfer &transfer = job.transfers[c[1]];
                    gl::phase = 13u;
                    apply_transfer(*g_memory, transfer);
                    const std::uint32_t first = transfer.destination +
                        (transfer.destination_y * transfer.destination_stride + transfer.destination_x) * transfer.bpp;
                    gl::invalidate_textures(first, (transfer.height - 1u) * transfer.destination_stride * transfer.bpp +
                                                       transfer.width * transfer.bpp);
                }
                gl::phase = 0u;
                if (snapshot && op != 0xEAu) gl::use_snapshot(&job.snapshot, c[1]);
                gl::ge_command(*g_memory, c[0], c[1], c[2]);
                if (snapshot) gl::use_snapshot(nullptr, 0u);
            }
            gl::end_list(std::move(job.snapshot));
            gl::account(host_now_ms() - started, started - waiting);
            break;
        }
        case Job::Kind::Present:
            gl::account(0.0, started - waiting);
            gl::set_drawing(!job.skip);
            gl::present(job.address);
            gl::set_drawing(true);
            if (gl::stats().frames % 300u == 0u) trace_op_costs();
            break;
        case Job::Kind::Capture: {
            const std::uint64_t vblank = job.vblank;
            gl::capture(job.address, [vblank](std::vector<std::uint8_t> &rgb) {
                if (!rgb.empty()) capture_rgb(rgb, vblank);
            });
            break;
        }
        }
        SpareBuffers spare;
        if (job.kind == Job::Kind::Commands) {
            spare.commands.swap(job.commands);
            spare.words.swap(job.words);
            spare.records.swap(job.records);
            spare.transfers.swap(job.transfers);
            spare.commands.clear();
            spare.words.clear();
            spare.records.clear();
            spare.transfers.clear();
        }
        std::lock_guard lock(g_jobs_mutex);
        if (!job.cached.empty()) g_returned_cached.push_back(std::move(job.cached));
        if (job.kind == Job::Kind::Commands && g_spares.size() < 4u) g_spares.push_back(std::move(spare));
        if (job.kind == Job::Kind::Commands) --g_queued_command_jobs;
        g_worker_busy = false;
        g_jobs_cv.notify_all();
    }
}

#if defined(__vita__)
int sampler_entry(SceSize, void *) {
    core_load::register_current_thread("sampler");
    sample_phases();
    return 0;
}

void *worker_entry(void *) {
    // Core 1: core 0 runs the emulation.
    const int affinity = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    core_load::register_current_thread("decode");
    char line[96];
    std::snprintf(line, sizeof(line), "worker started (core 1 affinity 0x%x, priority %d)", affinity,
                  sceKernelGetThreadCurrentPriority());
    gl::trace(line);
    const double before = host_now_ms();
    for (int i = 0; i < 10000; ++i) host_now_ms();
    std::snprintf(line, sizeof(line), "clock read costs %.2f us", (host_now_ms() - before) / 10.0);
    gl::trace(line);
    // Cheaper clocks, if any (candidates for the per-slice accounting).
    volatile std::uint32_t sink = 0u;
    double mark = host_now_ms();
    for (int i = 0; i < 10000; ++i) sink = sink + sceKernelGetProcessTimeLow();
    const double process_low = (host_now_ms() - mark) / 10.0;
    mark = host_now_ms();
    for (int i = 0; i < 10000; ++i) sink = sink + sceKernelGetProcessTimeLowCore();
    const double process_low_core = (host_now_ms() - mark) / 10.0;
    mark = host_now_ms();
    for (int i = 0; i < 10000; ++i) sink = sink + static_cast<std::uint32_t>(sceKernelGetSystemTimeWide());
    const double system_wide = (host_now_ms() - mark) / 10.0;
    std::snprintf(line, sizeof(line), "clock costs (us): ProcessTimeLow %.2f, ProcessTimeLowCore %.2f, SystemTimeWide %.2f",
                  process_low, process_low_core, system_wide);
    gl::trace(line);
    worker_loop();
    return nullptr;
}
#endif

void start_worker() {
#if defined(__vita__)
    // A pthread, so the std::mutex/condition_variable handshakes work; the
    // shader compiler wants a large stack.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);
    pthread_t thread;
    const int result = pthread_create(&thread, &attributes, worker_entry, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        std::lock_guard lock(g_jobs_mutex);
        g_worker_ready = g_worker_failed = true;
        g_worker_error = "pthread_create failed";
        return;
    }
    pthread_detach(thread);
    const SceUID sampler = sceKernelCreateThread("ge_gl_sampler", sampler_entry, 0x10000100, 0x4000, 0,
                                                 SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (sampler >= 0) sceKernelStartThread(sampler, 0, nullptr);
#else
    std::thread(worker_loop).detach();
    std::thread(sample_phases).detach();
#endif
}

// Commands that cannot change what the worker draws are not queued: control
// flow (ge_scan follows it), BASE/OFFSET (folded into each entry's base and
// offset) and state commands repeating the value last sent.
std::array<std::uint32_t, 256> g_last_forwarded{};
std::array<bool, 256> g_forwarded_once{};

bool forwarded(std::uint32_t op, std::uint32_t word) {
    switch (op) {
    case 0x00u: case 0x03u: case 0x08u: case 0x0Au: case 0x0Bu: case 0x0Eu: case 0x0Fu: case 0x10u: case 0x13u: case 0x14u:
        return false;
    // Actions, addresses and matrix uploads (a repeated select still resets
    // the cursor) always go.
    case 0x01u: case 0x02u: case 0x04u: case 0x0Cu: case 0x2Au: case 0x2Bu: case 0x3Au: case 0x3Bu: case 0x3Cu:
    case 0x3Du: case 0x3Eu: case 0x3Fu: case 0x40u: case 0x41u: case 0xC4u: case 0xEAu:
        return true;
    default:
        if (g_forwarded_once[op] && g_last_forwarded[op] == word) return false;
        g_forwarded_once[op] = true;
        g_last_forwarded[op] = word;
        return true;
    }
}

double g_emulation_wait_ms{};  // emulation thread blocked on the worker
bool g_skip_frame{};             // the frame being built is not drawn (frame_skip)
std::uint64_t g_skipped_frames{};
// skip_when_behind: lists turned into skipped ones because two were already
// queued (instead of the game waiting for the renderer), and the skipped
// frames in a row so far.
std::uint64_t g_behind_skips{};
std::uint32_t g_skips_in_a_row{};
double g_scan_ms{};            // emulation thread walking display lists
// Inside it (Vita trace): cached sub-lists forwarded, lists handed to the
// worker (with the wait for room), and the guest bytes snapshotted.
double g_cached_ms{}, g_submit_ms{};
std::uint64_t g_snapshot_vertex_bytes{}, g_snapshot_record_bytes{}, g_forwarded_words{};
std::uint32_t g_scans{};       // ge_scan calls (list submissions and stall updates)

} // namespace

std::size_t early_queue_size() {
    std::lock_guard lock(g_early_mutex);
    return g_early.size();
}

void request_capture() {
    g_capture_requested = true;
    gl::trace_next_frame();
}

void cpu_wrote_vram(std::uint32_t address, std::uint32_t size) {
    std::lock_guard lock(g_jobs_mutex);
    g_cpu_vram_writes.emplace_back(address & 0x0FFFFFFFu, size);
}

namespace {

void push_job(Job job) {
    std::unique_lock lock(g_jobs_mutex);
    if (job.kind == Job::Kind::Commands) {
        const double started = host_now_ms();
        const std::uint32_t outer = gl::emulation_phase;
        gl::emulation_phase = 7u;
        // Renderer behind: rather than slowing the game down, the rest of
        // this frame is followed without drawing (the screen keeps the last
        // one).
        if (!job.skip && g_options.frame_skip && g_options.skip_when_behind && g_queued_command_jobs >= 2u) {
            job.skip = true;
            if (!g_skip_frame) ++g_behind_skips;
            g_skip_frame = true;
        }
        // A skipped frame costs the worker little: no need to wait for it.
        // Three lists in flight: the worker's uneven frames (skinning, new
        // textures) are absorbed instead of stopping the game.
        const std::uint32_t room = job.skip ? 4u : 3u;
        g_jobs_cv.wait(lock, [room] { return g_queued_command_jobs < room; });
        gl::emulation_phase = outer;
        g_emulation_wait_ms += host_now_ms() - started;
        ++g_queued_command_jobs;
    }
    g_jobs.push_back(std::move(job));
    g_jobs_cv.notify_all();
}

void submit_pending() {
    if (g_pending.empty()) return;
    const double submit_started = host_now_ms();
    g_snapshot_vertex_bytes += g_pending_snapshot.vertices.size();
    g_snapshot_record_bytes += g_pending_snapshot.records.size();
    g_forwarded_words += g_pending_words.size();
    Job job;
    job.kind = Job::Kind::Commands;
    job.skip = g_skip_frame;
    job.commands.swap(g_pending);
    job.cached.swap(g_pending_cached);
    job.transfers.swap(g_pending_transfers);
    job.words.swap(g_pending_words);
    job.records.swap(g_pending_records);
    std::vector<std::vector<CachedRun>> returned;
    {
        std::lock_guard lock(g_jobs_mutex);
        returned.swap(g_returned_cached);
        if (!g_spares.empty()) {
            SpareBuffers &spare = g_spares.back();
            g_pending.swap(spare.commands);
            g_pending_words.swap(spare.words);
            g_pending_records.swap(spare.records);
            g_pending_transfers.swap(spare.transfers);
            g_spares.pop_back();
        }
    }
    // Here, on the emulation thread, the finished jobs' references go.
    for (auto &runs : returned) {
        runs.clear();
        if (g_pending_cached.capacity() < runs.capacity()) g_pending_cached.swap(runs);
    }
    returned.clear();
    g_pending.reserve(job.commands.size());
    g_pending_words.reserve(job.words.size());
    g_pending_records.reserve(job.records.size());
    job.snapshot.records.swap(g_pending_snapshot.records);
    job.snapshot.vertices.swap(g_pending_snapshot.vertices);
    // Room for the next list without regrowing.
    g_pending_snapshot.records.reserve(job.snapshot.records.size());
    g_pending_snapshot.vertices.reserve(job.snapshot.vertices.size());
    push_job(std::move(job));
    g_submit_ms += host_now_ms() - submit_started;
    if (!g_async) {
        // Without snapshots the GL thread reads guest memory: let it finish too.
        std::unique_lock lock(g_jobs_mutex);
        g_jobs_cv.wait(lock, [] { return g_jobs.empty() && !g_worker_busy; });
        lock.unlock();
        gl::finish();
    }
}

void drain_worker() {
    submit_pending();
    std::unique_lock lock(g_jobs_mutex);
    g_jobs_cv.wait(lock, [] { return g_jobs.empty() && !g_worker_busy; });
    lock.unlock();
    gl::finish();
}

} // namespace

void configure(const Options &options) {
    g_options = options;
    g_commands.fill(0u);
#if defined(BURNOUT_GE_SOFTWARE)
    vcs::reset_ge_transform_state(g_transform);
#else
    g_options.backend = Backend::OpenGL;
#endif
    if (g_options.enabled && g_options.backend == Backend::OpenGL) {
        gl::set_render_scale(g_options.render_scale);
        gl::set_offscreen_1x(g_options.offscreen_1x);
        gl::set_shader_cache(g_options.shader_cache_dir.string());
        gl::set_cached_memory(g_options.cached_gpu_memory);
        gl::set_variant_list(g_options.variant_list.string());
        start_worker();
        std::unique_lock lock(g_jobs_mutex);
        g_jobs_cv.wait(lock, [] { return g_worker_ready; });
        if (g_worker_failed) {
            std::cerr << "[render] OpenGL unavailable: " << g_worker_error << "\n";
            g_options.enabled = false;
        }
    }
    if (g_options.enabled && !g_options.capture_vblanks.empty())
        std::filesystem::create_directories(g_options.capture_dir);
}

bool active() noexcept { return g_options.enabled; }

void account_scan(double ms) noexcept {
    g_scan_ms += ms;
    ++g_scans;
}

void finish() {
    if (g_options.enabled && g_options.backend == Backend::OpenGL) drain_worker();
#if defined(_WIN32)
    if (g_worker_profiler) {
        g_worker_profiling = false;
        g_worker_profiler->write_report(std::getenv("PSPRECOMP_BURNOUT_PROFILE_WORKER"));
    }
#endif
}

namespace {

// One command on its way to the GL worker (emulation thread).
void forward_command(psprecomp::GuestMemory &memory, std::uint32_t word, std::uint32_t base, std::uint32_t offset) {
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    g_commands[op] = word;
    if (op == 0xEAu) {
        gl::emulation_phase = 6u;
        Transfer transfer;
        if (!take_early_transfer(data, transfer)) transfer = snapshot_transfer(memory, data);
        if ((transfer.destination & 0x0F000000u) == 0x04000000u) {
            // Into VRAM, which only the GE reads: replay in command order.
            g_pending.push_back({word, static_cast<std::uint32_t>(g_pending_transfers.size()), UINT32_MAX});
            g_pending_transfers.push_back(std::move(transfer));
            return;
        }
        apply_transfer(memory, transfer);  // guest RAM: stays synchronous
    }
    if (!forwarded(op, word)) return;
    if (g_async) {
        // The worker may draw this list while the game builds the next one.
        const std::uint32_t record = gl::snapshot_command(memory, word, base, offset, g_pending_snapshot);
        if (record != UINT32_MAX) {
            g_pending.push_back({word, record, UINT32_MAX});
            return;
        }
    }
    g_pending.push_back({word, base, offset});
    if (op == 0x0Cu) submit_pending();  // END: hand the list to the worker
}

} // namespace

namespace {

// Starts loading the cache line at `address` (no effect on the result).
inline void prefetch_line(const void *address) {
#if defined(_MSC_VER)
    _mm_prefetch(static_cast<const char *>(address), _MM_HINT_T0);
#else
    __builtin_prefetch(address);
#endif
}

enum class ForwardKind : std::uint8_t { Plain, Pointer, Snapshot, Transfer, Control };
// How the emulation thread treats each opcode inside a run: snapshot pointer
// and PRIM/LOAD_CLUT commands, TRXKICK, and the control flow that ends a run
// (as ge_scan's ge_is_control).
constexpr std::array<ForwardKind, 256> kForwardKind = [] {
    std::array<ForwardKind, 256> kinds{};
    for (const std::uint32_t op : {0x01u, 0x02u, 0x12u, 0xB0u, 0xB1u}) kinds[op] = ForwardKind::Pointer;
    for (const std::uint32_t op : {0x04u, 0xC4u}) kinds[op] = ForwardKind::Snapshot;
    kinds[0xEAu] = ForwardKind::Transfer;
    for (const std::uint32_t op : {0x08u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Eu, 0x0Fu, 0x10u, 0x13u, 0x14u})
        kinds[op] = ForwardKind::Control;
    return kinds;
}();

// The run goes to the worker as one block (it ignores repeated state
// itself); only the commands snapshots and transfers need are looked at.
// With `stop_at_control`, the run ends at the first control command
// (ge_plain_commands); returns the commands taken.
std::uint32_t forward_run(psprecomp::GuestMemory &memory, const std::uint8_t *words, std::uint32_t count,
                          std::uint32_t base, std::uint32_t offset, bool stop_at_control) {
    std::uint32_t start = 0u;
    const auto emit_run = [&](std::uint32_t end) {
        if (end == start) return;
        g_pending_words.push_back(base);
        g_pending_words.push_back(offset);
        const auto at = static_cast<std::uint32_t>(g_pending_words.size());
        const auto *first = reinterpret_cast<const std::uint32_t *>(words) + start;  // guest words are 4-byte aligned
        g_pending_words.insert(g_pending_words.end(), first, first + (end - start));
        g_pending.push_back({at, end - start, kRun});
    };
    const auto *list = reinterpret_cast<const std::uint32_t *>(words);
    std::uint32_t i = 0u;
    for (; i < count; ++i) {
        if ((i & 7u) == 0u) prefetch_line(list + i + 64u);  // long runs: stream ahead
        const std::uint32_t word = list[i];
        const std::uint32_t op = word >> 24u;
        const ForwardKind kind = kForwardKind[op];
        if (kind == ForwardKind::Plain) {
            g_commands[op] = word;
            continue;
        }
        if (kind == ForwardKind::Control && stop_at_control) break;
        g_commands[op] = word;
        if (kind == ForwardKind::Pointer) {
            const std::uint32_t data = word & 0x00FFFFFFu;
            switch (op) {
            case 0x01u: gl::g_scan.vertex_address = ((base | data) + offset) & 0x0FFFFFFFu; break;
            case 0x02u: gl::g_scan.index_address = ((base | data) + offset) & 0x0FFFFFFFu; break;
            case 0x12u: gl::g_scan.vtype = data; break;
            case 0xB0u: gl::g_scan.clut_low = data; break;
            default: gl::g_scan.clut_high = data; break;
            }
        } else if (kind == ForwardKind::Snapshot) {
            const std::uint32_t record = gl::snapshot_command(memory, word, base, offset, g_pending_snapshot);
            if (record != UINT32_MAX) g_pending_records.push_back(record);
            gl::emulation_phase = 2u;
        } else if (kind == ForwardKind::Transfer) {
            emit_run(i);
            forward_command(memory, word, base, offset);
            start = i + 1u;
        }
    }
    emit_run(i);
    gl::emulation_phase = 1u;
    return i;
}

// ------------------------------------------------------- cached sub-lists
//
// Most of a frame's commands are in sub-lists the top level CALLs (models),
// and most of those are the same words every frame, called in the same
// state. A sub-list already seen with the same words and the same entry
// state is forwarded from a copy made the first time, with the snapshot
// records it needed then, instead of being walked command by command. Only
// leaf sub-lists qualify: no control flow but BASE/OFFSET and the final RET,
// no SIGNAL/FINISH, no transfer, and nothing drawn from outside the static
// range, so what they forward depends only on their words and entry state.

struct CallKey {
    std::uint32_t target{}, base{}, offset{};
    gl::SnapshotState entry{};
    bool operator==(const CallKey &other) const noexcept {
        return target == other.target && base == other.base && offset == other.offset && entry.vtype == other.entry.vtype &&
               entry.vertex_address == other.entry.vertex_address && entry.index_address == other.entry.index_address &&
               entry.clut_low == other.entry.clut_low && entry.clut_high == other.entry.clut_high;
    }
};
struct CallKeyHash {
    std::size_t operator()(const CallKey &k) const noexcept {
        std::uint64_t h = (static_cast<std::uint64_t>(k.target) << 32u) ^ k.base ^ (k.offset * 0x9E3779B1u);
        for (const std::uint32_t v : {k.entry.vtype, k.entry.vertex_address, k.entry.index_address, k.entry.clut_low, k.entry.clut_high})
            h = (h ^ v) * 0x100000001B3ull;
        return static_cast<std::size_t>(h ^ (h >> 29u));
    }
};
// A stretch of the sub-list forwarded with one base and offset.
struct CallSegment {
    std::uint32_t base{}, offset{}, first{}, count{};
};
struct CachedCall {
    bool cacheable{};
    std::uint32_t changes{};                 // times the words were found changed
    // Every word up to the RET (excluded), as in memory. Never changed once
    // built (a rewrite makes a new one): queued jobs may still read it.
    std::shared_ptr<const std::vector<std::uint32_t>> words;
    std::vector<CallSegment> segments;       // the plain words among them
    std::vector<std::uint8_t> records;       // snapshot records the words needed
    std::vector<std::uint32_t> record_offsets;
    gl::SnapshotState exit{};                // pointer state after the RET
    std::uint32_t exit_base{}, exit_offset{};
};
std::unordered_map<CallKey, CachedCall, CallKeyHash> g_calls;
std::uint64_t g_call_hits{}, g_call_walked{}, g_call_hit_words{};

// memcmp(...) == 0 for the sub-list check: both sides are usually cold (the
// sub-lists are scattered), so the lines a few iterations ahead are fetched
// while these compare, and a difference is only looked for every 8 words.
bool same_words(const std::uint8_t *live_bytes, const std::uint32_t *copy, std::uint32_t count) noexcept {
    const auto *live = reinterpret_cast<const std::uint32_t *>(live_bytes);  // guest words are 4-byte aligned
    for (std::uint32_t at = 0u; at < count && at < 32u; at += 8u) {
        prefetch_line(live + at);
        prefetch_line(copy + at);
    }
    std::uint32_t i = 0u;
    for (; i + 8u <= count; i += 8u) {
        prefetch_line(live + i + 32u);
        prefetch_line(copy + i + 32u);
        std::uint32_t diff = 0u;
        for (std::uint32_t j = 0u; j < 8u; ++j) diff |= live[i + j] ^ copy[i + j];
        if (diff != 0u) return false;
    }
    std::uint32_t diff = 0u;
    for (; i < count; ++i) diff |= live[i] ^ copy[i];
    return diff == 0u;
}

bool static_range(std::uint32_t address) noexcept { return address >= 0x08800000u && address < 0x09C00000u; }

void forget_words(CachedCall &call) {
    call.words.reset();
    call.segments = {};
    call.records = {};
    call.record_offsets = {};
}

// Walks the sub-list at `target` as ge_scan and forward_run would, into
// `call`, from the current pointer state. False when it does not qualify (the
// caller then walks it as usual); the pointer state is left as it was.
bool classify_call(const psprecomp::GuestMemory &memory, std::uint32_t target, std::uint32_t base, std::uint32_t offset,
                   CachedCall &call) {
    constexpr std::uint32_t kMaxWords = 8192u;
    forget_words(call);
    std::vector<std::uint32_t> words;
    const gl::SnapshotState saved = gl::g_scan;
    gl::Snapshot snapshot;
    bool ok = false;
    std::uint32_t segment_start = 0u;
    const auto close_segment = [&](std::uint32_t end) {
        // The plain words since the last control word, at the base and
        // offset they were read with.
        if (end > segment_start) call.segments.push_back({base, offset, segment_start, end - segment_start});
    };
    for (std::uint32_t i = 0; i < kMaxWords; ++i) {
        const std::uint32_t pc = target + i * 4u;
        const std::uint8_t *at = memory.raw_pointer(pc, 4u);
        if (at == nullptr) break;
        std::uint32_t word;
        std::memcpy(&word, at, 4u);
        const std::uint32_t op = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        if (op == 0x0Bu) {  // RET
            close_segment(i);
            ok = true;
            break;
        }
        bool fail = false;
        bool control = false;
        switch (op) {
        case 0x10u: control = true; close_segment(i); base = (data << 8u) & 0x0F000000u; break;  // BASE
        case 0x13u: control = true; close_segment(i); offset = data << 8u; break;                // OFFSET_ADDR
        case 0x14u: control = true; close_segment(i); offset = pc; break;                        // ORIGIN_ADDR
        case 0x08u: case 0x0Au: case 0x0Cu: case 0x0Eu: case 0x0Fu:  // JUMP CALL END SIGNAL FINISH
        case 0xB2u: case 0xB3u: case 0xB4u: case 0xB5u: case 0xEAu: case 0xEBu: case 0xECu: case 0xEDu: case 0xEEu:
            fail = true;
            break;
        case 0x04u: {
            const bool indexed = ((gl::g_scan.vtype >> 11u) & 3u) != 0u;
            fail = !static_range(gl::g_scan.vertex_address) || (indexed && !static_range(gl::g_scan.index_address));
            break;
        }
        case 0xC4u: {
            const std::uint32_t clut = (gl::g_scan.clut_low & 0xFFFFF0u) | ((gl::g_scan.clut_high << 8u) & 0x0F000000u);
            fail = !static_range(clut);
            break;
        }
        default: break;
        }
        if (fail) {
            static int reported = 0;
            if (std::getenv("PSPRECOMP_BURNOUT_CALL_DEBUG") != nullptr && reported++ < 40)
                std::fprintf(stderr, "[calls] %08X fails at word %u op %02X vtype %06x vaddr %08x\n", target, i, op,
                             gl::g_scan.vtype, gl::g_scan.vertex_address);
            break;
        }
        words.push_back(word);
        if (control) {
            segment_start = i + 1u;
            continue;
        }
        if (kForwardKind[op] == ForwardKind::Pointer || kForwardKind[op] == ForwardKind::Snapshot) {
            const std::uint32_t record = gl::snapshot_command(memory, word, base, offset, snapshot);
            if (record != UINT32_MAX) call.record_offsets.push_back(record);
            if (!snapshot.vertices.empty()) break;  // vertices copied: not static after all
        }
    }
    ok = ok && snapshot.vertices.empty() && !words.empty();
    if (ok) call.words = std::make_shared<const std::vector<std::uint32_t>>(std::move(words));
    call.exit = gl::g_scan;
    call.exit_base = base;
    call.exit_offset = offset;
    gl::g_scan = saved;
    call.records.swap(snapshot.records);
    call.cacheable = ok;
    if (!ok) forget_words(call);
    return ok;
}

// What walking the sub-list would have forwarded, from the copy.
void forward_cached(const CachedCall &call) {
    gl::g_scan = call.exit;
    for (const CallSegment &segment : call.segments) {
        g_pending.push_back({static_cast<std::uint32_t>(g_pending_cached.size()), 0u, kCachedRun});
        g_pending_cached.push_back({call.words, segment.first, segment.count, segment.base, segment.offset});
    }
    const auto first = static_cast<std::uint32_t>(g_pending_snapshot.records.size());
    g_pending_snapshot.records.insert(g_pending_snapshot.records.end(), call.records.begin(), call.records.end());
    for (const std::uint32_t record : call.record_offsets) g_pending_records.push_back(first + record);
}

} // namespace

bool ge_cached_call(const psprecomp::GuestMemory &memory, std::uint32_t target, std::uint32_t &base, std::uint32_t &offset,
                    std::uint32_t stall) {
    if (!g_options.enabled || g_options.backend != Backend::OpenGL || !g_async || !g_options.cache_sublists) return false;
    target &= 0x0FFFFFFCu;
    if (!static_range(target)) return false;
    // Each entry state of each sub-list is a key: bounded, in case some state
    // never repeats.
    if (g_calls.size() > 32768u) g_calls.clear();
    auto [it, inserted] = g_calls.try_emplace(CallKey{target, base, offset, gl::g_scan});
    CachedCall &call = it->second;
    if (!inserted && !call.cacheable) return false;
    const auto bytes = static_cast<std::uint32_t>(call.words != nullptr ? call.words->size() * 4u : 0u);
    // A stall address inside the sub-list: the GE stops there, walk it.
    const std::uint32_t stop = stall & 0x0FFFFFFFu;
    if (stall != 0u && stop >= target && stop <= target + std::max<std::uint32_t>(bytes, 8192u * 4u) + 4u) return false;
    if (!inserted) {
        const std::uint8_t *live = memory.raw_pointer(target, bytes + 4u);
        std::uint32_t ret = 0u;
        if (live != nullptr) std::memcpy(&ret, live + bytes, 4u);
        const double started = host_now_ms();
        if (live != nullptr && (ret >> 24u) == 0x0Bu && same_words(live, call.words->data(), bytes / 4u)) {
            forward_cached(call);
            g_cached_ms += host_now_ms() - started;
            base = call.exit_base;
            offset = call.exit_offset;
            ++g_call_hits;
            g_call_hit_words += call.words->size();
            return true;
        }
        // Rewritten: a sub-list the game rebuilds is not worth caching.
        if (++call.changes > 4u) {
            call.cacheable = false;
            forget_words(call);
            return false;
        }
    }
    ++g_call_walked;
    if (!classify_call(memory, target, base, offset, call)) return false;
    forward_cached(call);
    base = call.exit_base;
    offset = call.exit_offset;
    return true;
}

bool early_transfers(const psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t stall, std::uint32_t base,
                     std::uint32_t offset) {
    if (!g_options.enabled || g_options.backend != Backend::OpenGL) return false;
    constexpr std::uint32_t kLimit = 1u << 20u;
    std::vector<EarlyTransfer> found;
    bool covered = false;  // reached END, and every transfer goes to VRAM
    bool to_ram = false;
    for (std::uint32_t count = 0; count < kLimit; ++count) {
        pc &= 0x0FFFFFFCu;
        if (stall != 0u && pc == (stall & 0x0FFFFFFFu)) break;
        const std::uint8_t *at = memory.raw_pointer(pc, 4u);
        if (at == nullptr) break;
        std::uint32_t word;
        std::memcpy(&word, at, 4u);
        const std::uint32_t op = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        pc += 4u;
        switch (op) {
        case 0x08u: pc = (base | data) + offset; continue;  // JUMP
        case 0x0Bu: case 0x0Cu: covered = true; count = kLimit; continue;  // RET at the top level, END
        case 0x10u: base = (data << 8u) & 0x0F000000u; continue;
        case 0x13u: offset = data << 8u; continue;
        case 0x14u: offset = pc - 4u; continue;
        case 0xB2u: case 0xB3u: case 0xB4u: case 0xB5u: case 0xEBu: case 0xECu: case 0xEEu:
            g_early_regs[op] = word;
            continue;
        case 0xEAu: {
            EarlyTransfer early;
            early.regs = transfer_key(g_early_regs, data);
            early.transfer = snapshot_transfer_from(memory, g_early_regs, data);
            // A copy into RAM may feed a later transfer: its source is only
            // right once the front end has done the first.
            if ((early.transfer.destination & 0x0F000000u) != 0x04000000u) to_ram = true;
            found.push_back(std::move(early));
            continue;
        }
        default: continue;  // CALL: sub-lists hold no transfers
        }
    }
    if (!found.empty()) {
        std::lock_guard lock(g_early_mutex);
        for (EarlyTransfer &early : found) g_early.push_back(std::move(early));
        while (g_early.size() > kEarlyMax) g_early.pop_front();
    }
    return covered && !to_ram;
}

void ge_commands(psprecomp::GuestMemory &memory, const std::uint8_t *words, std::uint32_t count, std::uint32_t base,
                 std::uint32_t offset) {
    if (!g_options.enabled || g_options.backend != Backend::OpenGL) return;
    g_memory = &memory;
    gl::emulation_phase = 2u;
    if (!g_async) {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t word;
            std::memcpy(&word, words + i * 4u, 4u);
            forward_command(memory, word, base, offset);
        }
        gl::emulation_phase = 1u;
        return;
    }
    forward_run(memory, words, count, base, offset, false);
}

std::uint32_t ge_plain_commands(psprecomp::GuestMemory &memory, const std::uint8_t *words, std::uint32_t span,
                                std::uint32_t base, std::uint32_t offset) {
    if (g_options.enabled && g_options.backend == Backend::OpenGL && g_async) {
        g_memory = &memory;
        gl::emulation_phase = 2u;
        return forward_run(memory, words, span, base, offset, true);
    }
    std::uint32_t plain = 0u;
    while (plain < span && kForwardKind[words[plain * 4u + 3u]] != ForwardKind::Control) ++plain;
    if (plain != 0u) ge_commands(memory, words, plain, base, offset);
    return plain;
}

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t word, std::uint32_t base,
                std::uint32_t offset) {
    (void)pc;
    if (!g_options.enabled) return;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    g_commands[op] = word;
    if (g_options.backend == Backend::OpenGL) {
        g_memory = &memory;
        gl::emulation_phase = 2u;
        forward_command(memory, word, base, offset);
        gl::emulation_phase = 1u;
        return;
    }
#if defined(BURNOUT_GE_SOFTWARE)
    if (op >= 0x2Au && op <= 0x41u) vcs::update_ge_transform_state(g_transform, op, data);
    const auto address = [&]() { return ((base | data) + offset) & 0x0FFFFFFFu; };
    switch (op) {
    case 0xC4u:                                        // LOAD_CLUT
        g_clut_loaded_from = (g_commands[0xB0u] & 0x00FFFFF0u) | ((g_commands[0xB1u] << 8u) & 0x0F000000u);
        g_clut_load_word = data;
        break;
    case 0xEAu: block_transfer(memory, data); break;  // TRXKICK
    case 0x01u: g_vertex_address = address(); break;  // VADDR
    case 0x02u: g_index_address = address(); break;   // IADDR
    case 0x03u: {                                      // BBOX: only advances the vertex pointer here
        vcs::GeBoundingBoxResult result{};
        std::string error;
        if (vcs::test_ge_bounding_box(memory, g_commands, g_transform, g_vertex_address, g_index_address,
                                      data & 0xFFFFu, result, error)) {
            g_vertex_address = result.next_vertex_address;
            g_index_address = result.next_index_address;
        }
        break;
    }
    case 0x04u: {                                      // PRIM
        // Bring-up knob: PSPRECOMP_BURNOUT_SKIP_TEXTURE=addr skips textured draws
        // sampling that address.
        static const std::uint32_t skip_texture = [] {
            const char *env = std::getenv("PSPRECOMP_BURNOUT_SKIP_TEXTURE");
            return env != nullptr ? static_cast<std::uint32_t>(std::stoul(env, nullptr, 16)) : 0u;
        }();
        if (skip_texture != 0u && (g_commands[0x1Eu] & 1u) != 0u &&
            ((g_commands[0xA0u] & 0xFFFFF0u) | ((g_commands[0xA8u] << 8u) & 0x0F000000u)) == skip_texture)
            break;
        const auto started = std::chrono::steady_clock::now();
        vcs::GeRenderStats stats{};
        std::string error;
        ++g_primitives;
        const std::uint64_t vblank = vblank_count();
        const bool trace = vblank >= g_options.trace_from && vblank <= g_options.trace_to;
        const std::uint32_t draw_vertex_address = g_vertex_address;
        // Bring-up knob: PSPRECOMP_BURNOUT_MASK=c1,c2 renders with those GE
        // command registers cleared (e.g. 1f disables fog).
        static const std::vector<std::uint32_t> masked = [] {
            std::vector<std::uint32_t> list;
            if (const char *env = std::getenv("PSPRECOMP_BURNOUT_MASK")) {
                std::istringstream items(env);
                for (std::string item; std::getline(items, item, ',');) list.push_back(std::stoul(item, nullptr, 16));
            }
            return list;
        }();
        std::array<std::uint32_t, 8> saved{};
        for (std::size_t i = 0; i < masked.size() && i < saved.size(); ++i) {
            saved[i] = g_commands[masked[i]];
            g_commands[masked[i]] &= 0xFF000000u;
        }
        if (vcs::render_ge_primitive(memory, g_commands, g_transform, g_vertex_address, g_index_address, data, stats,
                                     error, 1u, 0u, 0u, 0u, trace)) {
            g_vertex_address = stats.next_vertex_address;
            g_index_address = stats.next_index_address;
        } else if (g_failures++ == 0u) {
            g_first_error = error;
        }
        for (std::size_t i = 0; i < masked.size() && i < saved.size(); ++i) g_commands[masked[i]] = saved[i];
        if (trace) {
            const auto c = [&](std::uint32_t index) { return g_commands[index] & 0x00FFFFFFu; };
            std::fprintf(stderr,
                         "[draw] vb=%llu prim=%u n=%u vtype=%06x vaddr=%08x tex=%u fmt=%u taddr=%08x clut=%08x loaded=%08x/%02x cfmt=%06x tfunc=%06x "
                         "light=%u blend=%u/%06x fix=%06x/%06x atest=%u/%06x ztest=%u/%06x zw=%u fog=%u mat=%06x amb=%06x "
                         "fb=%06x map=%06x shade=%06x tsize=%06x tmode=%06x uv=%06x,%06x,%06x,%06x wrap=%06x tflt=%06x tested=%llu written=%llu tris=%llu culled=%llu scr=[%.0f,%.0f:%.0f,%.0f]\n",
                         static_cast<unsigned long long>(vblank), (data >> 16u) & 7u, data & 0xFFFFu, c(0x12u),
                         draw_vertex_address, c(0x1Eu) & 1u, c(0xC3u) & 0xFu, (c(0xA0u) & 0xFFFFF0u) | ((c(0xA8u) << 8u) & 0x0F000000u),
                         (c(0xB0u) & 0xFFFFF0u) | ((c(0xB1u) << 8u) & 0x0F000000u), g_clut_loaded_from, g_clut_load_word & 0x3Fu,
                         c(0xC5u), c(0xC9u), c(0x17u) & 1u,
                         c(0x21u) & 1u, c(0xDFu), c(0xE0u), c(0xE1u), c(0x22u) & 1u, c(0xDBu), c(0x23u) & 1u, c(0xDEu), c(0xE7u) & 1u,
                         c(0x1Fu) & 1u, c(0x53u), c(0x55u), c(0x9Cu), c(0xC0u), c(0xC1u), c(0xB8u), c(0xC2u), c(0x48u), c(0x49u),
                         c(0x4Au), c(0x4Bu), c(0xC7u), c(0xC6u),
                         static_cast<unsigned long long>(stats.pixels_tested),
                         static_cast<unsigned long long>(stats.pixels_written),
                         static_cast<unsigned long long>(stats.triangles),
                         static_cast<unsigned long long>(stats.culled_triangles), stats.screen_min_x,
                         stats.screen_min_y, stats.screen_max_x, stats.screen_max_y);
        }
        g_render_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        break;
    }
    default: break;
    }
#endif
}

void frame_buffer_set(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stride,
                      std::uint32_t format, std::uint64_t vblank) {
    if (!g_options.enabled) return;
    if (g_options.backend == Backend::OpenGL) {
        submit_pending();
        Job present;
        present.kind = Job::Kind::Present;
        present.address = address;
        present.skip = g_skip_frame;
        push_job(std::move(present));
        // Decide for the next frame: skip it when the worker is still busy
        // with earlier lists, but never two frames in a row.
        g_skips_in_a_row = g_skip_frame ? g_skips_in_a_row + 1u : 0u;
        if (g_options.frame_skip) {
            std::lock_guard lock(g_jobs_mutex);
            // skip_when_behind allows several in a row (at most 3), so the
            // game keeps its speed whatever the GPU manages.
            const bool may_skip = g_options.skip_when_behind ? g_skips_in_a_row < 3u : !g_skip_frame;
            g_skip_frame = may_skip && g_queued_command_jobs >= 2u;
            if (g_skip_frame) ++g_skipped_frames;
        } else {
            g_skip_frame = false;
        }
        // Longest gap between two flips: the hitches a player sees.
        static double last_flip = host_now_ms(), worst_flip = 0.0;
        const double now = host_now_ms();
        worst_flip = std::max(worst_flip, now - last_flip);
        last_flip = now;
        if (++g_flips % 300u == 0u) {
            char line[640];
            std::uint64_t early_used = 0u, early_missed = 0u;
            {
                std::lock_guard lock(g_early_mutex);
                early_used = g_early_used;
                early_missed = g_early_missed;
            }
            std::snprintf(line, sizeof(line),
                          "emulation: list scan %.1f ms/flip in %.0f scans, waited %.1f ms/flip for the worker, %llu of 300 frames skipped; "
                          "worst flip %.0f ms; file reads %.0f ms (longest %.0f); early transfers %llu used, %llu missed; %llu skipped when behind; "
                          "sub-lists %.0f cached (%.0f words), %.1f walked anew per flip; in the scan: cached sub-lists %.1f ms, submit %.1f ms, "
                          "%.0f words forwarded, %.0f KiB vertices and %.0f KiB records copied per flip",
                          g_scan_ms / 300.0, g_scans / 300.0, g_emulation_wait_ms / 300.0, static_cast<unsigned long long>(g_skipped_frames),
                          worst_flip, gl::io_read_ms, gl::io_read_max_ms, static_cast<unsigned long long>(early_used),
                          static_cast<unsigned long long>(early_missed), static_cast<unsigned long long>(g_behind_skips),
                          g_call_hits / 300.0, g_call_hit_words / 300.0, g_call_walked / 300.0, g_cached_ms / 300.0, g_submit_ms / 300.0,
                          g_forwarded_words / 300.0, g_snapshot_vertex_bytes / 300.0 / 1024.0, g_snapshot_record_bytes / 300.0 / 1024.0);
            g_cached_ms = g_submit_ms = 0.0;
            g_snapshot_vertex_bytes = g_snapshot_record_bytes = g_forwarded_words = 0u;
            g_call_hits = g_call_hit_words = g_call_walked = 0u;
            g_behind_skips = 0u;
            worst_flip = 0.0;
            gl::io_read_ms = gl::io_read_max_ms = 0.0;
            g_skipped_frames = 0u;
            g_scan_ms = 0.0;
            g_scans = 0u;
            gl::trace(line);
#if defined(__vita__)
            // A plugin may change the clocks behind the game's back.
            std::snprintf(line, sizeof(line), "clocks: ARM %d MHz, GPU %d MHz, GPU crossbar %d MHz", scePowerGetArmClockFrequency(),
                          scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
            gl::trace(line);
#endif
            g_emulation_wait_ms = 0.0;
        }
    }
    // A capture asked for from the pad (request_capture), else the planned ones.
    const bool requested = g_capture_requested.exchange(false);
    if (requested) {
        std::error_code ec;
        std::filesystem::create_directories(g_options.capture_dir, ec);
    } else {
        if (g_options.capture_vblanks.empty()) return;
        const auto next = g_options.capture_vblanks.begin();
        if (vblank < *next) return;
        g_options.capture_vblanks.erase(next);
    }
    if (address == 0u || stride == 0u) return;
    if (g_options.backend == Backend::OpenGL) {
        Job capture;
        capture.kind = Job::Kind::Capture;
        capture.address = address;
        capture.vblank = vblank;
        push_job(std::move(capture));
        return;
    }
    try {
        std::vector<std::uint8_t> rgb;
#if defined(BURNOUT_GE_SOFTWARE)
        rgb = vcs::decode_framebuffer_rgb(memory, vcs::FramebufferDescription{address, 480u, 272u, stride, format});
#endif
        capture_rgb(rgb, vblank);
    } catch (const std::exception &e) {
        std::cerr << "[render] capture at vblank " << vblank << " failed: " << e.what() << "\n";
    }
}

std::string report() {
    if (!g_options.enabled) return {};
    std::ostringstream out;
    if (g_options.backend == Backend::OpenGL) {
        drain_worker();
        const auto &s = gl::stats();
        out << "GE OpenGL render: " << s.primitives << " PRIM in " << s.batches << " batches (" << s.vertices
            << " vertices, " << s.skipped_primitives << " PRIM skipped), " << s.texture_uploads << " texture uploads, "
            << s.render_targets << " render targets, " << s.frames << " frames, " << g_captures << " captures\n"
            << "GE OpenGL time: decode " << (s.list_ms - s.draw_ms) / 1000.0 << " s, draw " << s.draw_ms / 1000.0
            << " s, present " << s.present_ms / 1000.0 << " s, idle " << s.idle_ms / 1000.0 << " s\n";
    }
    out << "GE software render: " << g_primitives << " PRIM, " << g_failures << " failed"
        << (g_first_error.empty() ? "" : " (first: " + g_first_error + ")") << ", " << g_render_ms / 1000.0
        << " s in the rasterizer, " << g_captures << " captures\n"
        << "GE block transfers: " << g_transfers << " (" << g_transfer_bytes / 1024u << " KiB, " << g_transfers_to_ram
        << " into RAM, " << g_transfer_failures << " out of range)\n";
    return out.str();
}

} // namespace burnout::render
