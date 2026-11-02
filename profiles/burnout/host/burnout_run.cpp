#include "burnout_run.hpp"

#include "burnout_hle.hpp"
#include "sas_worker.hpp"
#include "workload_stats.hpp"
#if defined(BURNOUT_GE_RENDER)
#include "ge_render.hpp"
#endif

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>

// Written by every generated block when the corpus is built with
// PSPRECOMP_PROFILE_LABELS=1; read by the Vita sampling profiler.
extern "C" volatile std::uint32_t psprecomp_profile_label = 0u;

namespace burnout {

namespace {

// FNV-1a over the 24 MiB user partition; optionally also per 64 KiB block.
std::uint64_t hash_user_ram(psprecomp::GuestMemory &memory, std::vector<std::uint64_t> *blocks) {
    constexpr std::uint32_t kBlock = 64u * 1024u;
    std::uint64_t total = 0xCBF29CE484222325ull;
    std::vector<std::uint8_t> chunk(kBlock);
    for (std::uint32_t address = 0x08800000u; address < 0x0A000000u; address += kBlock) {
        memory.copy_out(address, chunk);
        std::uint64_t block = 0xCBF29CE484222325ull;
        for (const std::uint8_t byte : chunk) {
            total = (total ^ byte) * 0x100000001B3ull;
            block = (block ^ byte) * 0x100000001B3ull;
        }
        if (blocks != nullptr) blocks->push_back(block);
    }
    return total;
}

std::string hex64(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%08lx%08lx", static_cast<unsigned long>(value >> 32u),
                  static_cast<unsigned long>(value & 0xFFFFFFFFu));
    return text;
}

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

BenchResult run_benchmark(const BenchOptions &options) {
    auto elf = psprecomp::Elf32Image::from_file(options.elf);
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    for (const auto &table : options.nid_tables)
        if (std::filesystem::exists(table)) runtime.nids().load_csv(table);
    (void)elf.load_and_relocate(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
    std::uint64_t image_end = 0u;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &segment = elf.segments()[i];
        if (segment.type != 1u) continue;
        image_end = std::max<std::uint64_t>(
            image_end, elf.segment_runtime_address(i, psprecomp::kDefaultPspUserLoadBase) + segment.memory_size);
    }
    const auto module = elf.find_module_info(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
    if (!module) throw psprecomp::Error("PSP module info not found");
    psprecomp::register_generated_functions(runtime);

    FrameClock clock;
    if (options.measure_start != kAutoMeasureStart) clock.set_window(options.measure_start, options.measure_vblanks);
    std::set<std::string> opened;
    std::uint64_t race_vblank = 0u;
    const std::string marker = lower(options.race_marker);

    HleOptions hle;
    hle.usrdir = options.usrdir;
    hle.disc_root = options.disc_root;
    hle.memstick = options.memstick;
    hle.user_arena_start = static_cast<std::uint32_t>(image_end);
    hle.module_gp = module->gp;
    hle.verbose = options.verbose;

    HleObserver observer;
    observer.on_wait_vblank = [&](std::int32_t uid, std::uint64_t vblank) { clock.on_wait_vblank(uid, vblank); };
    observer.on_set_frame_buffer = [&](std::uint32_t, std::uint64_t vblank) { clock.on_set_frame_buffer(vblank); };
    observer.controller_buttons = [&](std::uint64_t vblank) {
        return options.script.buttons_at(vblank) | (options.live_buttons ? options.live_buttons() : 0u);
    };
    if (options.live_analog) observer.controller_analog = options.live_analog;
    observer.on_file_open = [&](const std::string &path) {
        const std::string key = lower(path);
        const std::uint64_t vblank = vblank_count();
        if (opened.insert(key).second && options.trace_files)
            std::cerr << "[file] vblank=" << vblank << " " << path << "\n";
        if (race_vblank == 0u && key.find(marker) != std::string::npos) {
            race_vblank = vblank;
            std::cerr << "[bench] race data first opened at vblank " << vblank << ": " << path << "\n";
            if (options.measure_start == kAutoMeasureStart)
                clock.set_window(vblank + options.auto_settle_vblanks, options.measure_vblanks);
        }
    };
    const double started = host_now_ms();
    double pace_start = started;
    std::uint64_t pace_vblank = 0u;
    observer.on_vblank = [&](std::uint64_t vblank) {
        if (options.measuring != nullptr) options.measuring->store(clock.window_open(vblank), std::memory_order_relaxed);
        if (vblank % 600u == 0u) {
            std::cerr << "[bench] vblank " << vblank << " (" << vblank / 60u << " s PSP) host "
                      << (host_now_ms() - started) / 1000.0 << " s"
                      << (clock.window_open(vblank) ? " [measuring]" : "") << std::endl;
        }
        if (options.realtime) {
            // 59.94 Hz: wait while the guest is ahead of the wall clock. A guest
            // more than half a second late (slow frames, the console suspended)
            // restarts from now, so it never runs flat out to catch up.
            const double due = pace_start + static_cast<double>(vblank - pace_vblank) * 1001.0 / 60.0;
            double now = host_now_ms();
            if (now - due > 500.0) {
                pace_start = now;
                pace_vblank = vblank;
            }
            for (; now < due; now = host_now_ms()) host_sleep_ms(due - now);
        }
        if (options.quit_requested && options.quit_requested()) runtime.stop("quit requested");
        else if (!options.realtime && clock.window_done(vblank)) runtime.stop("benchmark window complete");
        else if (vblank >= options.max_vblanks) runtime.stop("max vblanks reached");
    };

    std::ofstream hash_trace;
    if (!options.hash_trace.empty()) hash_trace.open(options.hash_trace, std::ios::trunc);
    workload::set_collecting(options.workload);
    observer.on_vblank = [&, handler = std::move(observer.on_vblank)](std::uint64_t vblank) {
        workload::set_window_open(clock.window_open(vblank));
        if (hash_trace.is_open() && vblank % 60u == 0u && !clock.window_open(vblank) && !clock.window_done(vblank)) {
            std::vector<std::uint64_t> blocks;
            hash_trace << vblank << " " << hex64(hash_user_ram(runtime.memory(), &blocks));
            for (const auto block : blocks) hash_trace << " " << hex64(block);
            hash_trace << "\n" << std::flush;
        }
        handler(vblank);
    };
    install_hle(runtime, hle, std::move(observer));
    std::vector<std::pair<std::string, std::uint32_t>> imports;
    for (const auto &import : elf.scan_imports(runtime.memory(), *module)) imports.emplace_back(import.library, import.nid);
    install_fallback_stubs(runtime, imports);
    prepare_boot_thread(runtime, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");

    std::cerr << "[bench] functions " << runtime.function_count() << ", imports " << imports.size()
              << ", entry " << psprecomp::hex32(elf.runtime_entry()) << ", arena "
              << psprecomp::hex32(static_cast<std::uint32_t>(image_end)) << std::endl;
    if (options.async_ge && ge_front_start(runtime)) std::cerr << "[bench] display lists walked on another core" << std::endl;
    runtime.run(elf.runtime_entry(), UINT64_MAX);
    sas_worker::drain();
#if defined(BURNOUT_GE_RENDER)
    ge_front_drain();
    render::finish();
#endif

    BenchResult result;
    result.host_seconds = (host_now_ms() - started) / 1000.0;
    result.stop_reason = runtime.stop_reason();
    result.complete = result.stop_reason == "benchmark window complete";
    std::cerr << "[bench] stopped: " << result.stop_reason << " after " << result.host_seconds << " s host, "
              << vblank_count() << " vblanks" << std::endl;
    print_hle_summary();
    // Virtual time is deterministic, so two builds that run the guest identically
    // print the same hash.
    const std::string ram_hash_text = hex64(hash_user_ram(runtime.memory(), nullptr));
    result.report = clock.report(options.title) + "race data at vblank " + std::to_string(race_vblank) +
                    "\nstop: " + result.stop_reason + "\nuser RAM hash: " + ram_hash_text + "\n" + workload::report();
    if (!options.frames_csv.empty()) clock.write_frames_csv(options.frames_csv);
    return result;
}

} // namespace burnout
