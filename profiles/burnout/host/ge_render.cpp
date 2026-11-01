#include "ge_render.hpp"

#if defined(BURNOUT_GE_SOFTWARE)
#include "framebuffer_capture.hpp"
#include "ge_renderer.hpp"
#endif
#include "ge_gl.hpp"

#include "burnout_hle.hpp"

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#endif

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
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

Transfer snapshot_transfer(const psprecomp::GuestMemory &memory, std::uint32_t data) {
    const auto c = [](std::uint32_t index) { return g_commands[index] & 0x00FFFFFFu; };
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

struct Job {
    enum class Kind { Commands, Present, Capture } kind{};
    std::vector<std::array<std::uint32_t, 3>> commands;
    std::vector<Transfer> transfers;  // TRXKICK entries carry their index here
    std::vector<std::uint8_t> arena;  // PRIM/LOAD_CLUT entries carry their record offset here
    std::uint32_t address{};
    std::uint64_t vblank{};
};

std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;
std::deque<Job> g_jobs;
std::size_t g_queued_command_jobs{};
bool g_worker_busy{};
bool g_worker_ready{};
bool g_worker_failed{};
std::string g_worker_error;
std::vector<std::array<std::uint32_t, 3>> g_pending;
std::vector<Transfer> g_pending_transfers;
std::vector<std::uint8_t> g_pending_arena;
// Overlap: the worker draws frame N while the emulation runs frame N+1. The
// guest bytes each list reads are snapshotted at scan time for that, since the
// game rewrites them while it builds the next frame.
// PSPRECOMP_BURNOUT_GL_SYNC=1 waits for each list instead.
const bool g_async = std::getenv("PSPRECOMP_BURNOUT_GL_SYNC") == nullptr;
psprecomp::GuestMemory *g_memory{};

void capture_rgb(const std::vector<std::uint8_t> &rgb, std::uint64_t vblank) {
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(vblank));
    write_png(g_options.capture_dir / name, 480u, 272u, rgb);
    ++g_captures;
}

void worker_loop() {
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
    for (;;) {
        Job job;
        {
            std::unique_lock lock(g_jobs_mutex);
            g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_worker_busy = true;
        }
        switch (job.kind) {
        case Job::Kind::Commands:
            for (const auto &c : job.commands) {
                // A TRXKICK carries the index of its snapshot instead of base/offset.
                const std::uint32_t op = c[0] >> 24u;
                const bool snapshot = c[2] == UINT32_MAX;
                if (snapshot && op == 0xEAu) apply_transfer(*g_memory, job.transfers[c[1]]);
                if (snapshot && op != 0xEAu) gl::use_snapshot(job.arena.data() + c[1]);
                gl::ge_command(*g_memory, c[0], c[1], c[2]);
                if (snapshot) gl::use_snapshot(nullptr);
            }
            break;
        case Job::Kind::Present:
            gl::present(job.address);
            break;
        case Job::Kind::Capture: {
            std::vector<std::uint8_t> rgb;
            if (gl::read_frame(job.address, rgb)) capture_rgb(rgb, job.vblank);
            break;
        }
        }
        std::lock_guard lock(g_jobs_mutex);
        if (job.kind == Job::Kind::Commands) --g_queued_command_jobs;
        g_worker_busy = false;
        g_jobs_cv.notify_all();
    }
}

#if defined(__vita__)
void *worker_entry(void *) {
    // Core 1: core 0 runs the emulation.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    gl::trace("worker started");
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
#else
    std::thread(worker_loop).detach();
#endif
}

void push_job(Job job) {
    std::unique_lock lock(g_jobs_mutex);
    if (job.kind == Job::Kind::Commands) {
        g_jobs_cv.wait(lock, [] { return g_queued_command_jobs < 2u; });
        ++g_queued_command_jobs;
    }
    g_jobs.push_back(std::move(job));
    g_jobs_cv.notify_all();
}

void submit_pending() {
    if (g_pending.empty()) return;
    Job job;
    job.kind = Job::Kind::Commands;
    job.commands.swap(g_pending);
    job.transfers.swap(g_pending_transfers);
    job.arena.swap(g_pending_arena);
    push_job(std::move(job));
    if (!g_async) {
        std::unique_lock lock(g_jobs_mutex);
        g_jobs_cv.wait(lock, [] { return g_jobs.empty() && !g_worker_busy; });
    }
}

void drain_worker() {
    submit_pending();
    std::unique_lock lock(g_jobs_mutex);
    g_jobs_cv.wait(lock, [] { return g_jobs.empty() && !g_worker_busy; });
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

void ge_command(psprecomp::GuestMemory &memory, std::uint32_t pc, std::uint32_t word, std::uint32_t base,
                std::uint32_t offset) {
    (void)pc;
    if (!g_options.enabled) return;
    const std::uint32_t op = word >> 24u;
    const std::uint32_t data = word & 0x00FFFFFFu;
    g_commands[op] = word;
    if (g_options.backend == Backend::OpenGL) {
        g_memory = &memory;
        if (op == 0xEAu) {
            Transfer transfer = snapshot_transfer(memory, data);
            if ((transfer.destination & 0x0F000000u) == 0x04000000u) {
                // Into VRAM, which only the GE reads: replay in command order.
                g_pending.push_back({word, static_cast<std::uint32_t>(g_pending_transfers.size()), UINT32_MAX});
                g_pending_transfers.push_back(std::move(transfer));
                return;
            }
            apply_transfer(memory, transfer);  // guest RAM: stays synchronous
        }
        if (g_async) {
            // The worker may draw this list while the game builds the next one.
            const std::uint32_t record = gl::snapshot_command(memory, word, base, offset, g_pending_arena);
            if (record != UINT32_MAX) {
                g_pending.push_back({word, record, UINT32_MAX});
                return;
            }
        }
        g_pending.push_back({word, base, offset});
        if (op == 0x0Cu) submit_pending();  // END: hand the list to the worker
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
        push_job(std::move(present));
    }
    if (g_options.capture_vblanks.empty()) return;
    const auto next = g_options.capture_vblanks.begin();
    if (vblank < *next) return;
    g_options.capture_vblanks.erase(next);
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
            << "GE OpenGL time: decode " << s.decode_ms / 1000.0 << " s, draw " << s.draw_ms / 1000.0 << " s (textures "
            << s.texture_ms / 1000.0 << " s), present " << s.present_ms / 1000.0 << " s\n";
    }
    out << "GE software render: " << g_primitives << " PRIM, " << g_failures << " failed"
        << (g_first_error.empty() ? "" : " (first: " + g_first_error + ")") << ", " << g_render_ms / 1000.0
        << " s in the rasterizer, " << g_captures << " captures\n"
        << "GE block transfers: " << g_transfers << " (" << g_transfer_bytes / 1024u << " KiB, " << g_transfers_to_ram
        << " into RAM, " << g_transfer_failures << " out of range)\n";
    return out.str();
}

} // namespace burnout::render
