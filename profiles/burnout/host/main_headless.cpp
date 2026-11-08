// Headless Burnout Legends runner (PC): boots the recompiled executable with
// the burnout HLE, drives it with a scripted controller and reports host time
// per logical game frame. No audio output; --render draws the GE lists with the
// software rasterizer and --capture writes PNG frames.
#include "audio_out.hpp"
#include "texture_pack.hpp"
#include "music.hpp"
#include "sas_worker.hpp"
#include "burnout_run.hpp"
#include "ge_render.hpp"
#include "ge_gl.hpp"
#include "filters.hpp"

#include "psprecomp/common.hpp"

#if defined(_WIN32)
#include "sampling_profiler_win.hpp"
#endif

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

struct Cli {
    burnout::BenchOptions bench;
    std::filesystem::path script;
    std::filesystem::path report{"burnout-bench.txt"};
    std::filesystem::path profile;
    burnout::render::Options render = [] {
        burnout::render::Options options;
        options.capture_dir = "out/burnout-frames";
        return options;
    }();
};

Cli parse(int argc, char **argv) {
    Cli cli;
    auto &o = cli.bench;
    o.elf = "../original/EBOOT_DECRYPTED.ELF";
    o.usrdir = "../original/USRDIR";
    o.disc_root = "../original";
    o.memstick = "out/burnout-ms0";
    o.nid_tables = {"configs/nids.csv", "profiles/burnout/config/nids_burnout.csv"};
    o.title = "burnout-bench (PC headless)";
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw psprecomp::Error("missing value for " + key);
            return argv[++i];
        };
        if (key == "--elf") o.elf = value();
        else if (key == "--usrdir") o.usrdir = value();
        else if (key == "--disc") o.disc_root = value();
        else if (key == "--ms0") o.memstick = value();
        else if (key == "--save-slot") o.save_slot = static_cast<std::uint32_t>(std::stoul(value()));
        else if (key == "--script") cli.script = value();
        else if (key == "--report") cli.report = value();
        else if (key == "--frames-csv") o.frames_csv = value();
        else if (key == "--hash-trace") o.hash_trace = value();
        else if (key == "--workload") o.workload = true;
        else if (key == "--max-vblanks") o.max_vblanks = std::stoull(value());
        else if (key == "--measure-start") {
            const std::string v = value();
            o.measure_start = v == "auto" ? burnout::kAutoMeasureStart : std::stoull(v);
        }
        else if (key == "--measure-vblanks") o.measure_vblanks = std::stoull(value());
        else if (key == "--settle-vblanks") o.auto_settle_vblanks = std::stoull(value());
        else if (key == "--race-marker") o.race_marker = value();
        else if (key == "--profile") cli.profile = value();
        else if (key == "--verbose") o.verbose = true;
        else if (key == "--trace-files") o.trace_files = true;
        else if (key == "--render") cli.render.enabled = true;
        else if (key == "--gl") {  // OpenGL backend (hidden window) instead of the software rasterizer
            cli.render.enabled = true;
            cli.render.backend = burnout::render::Backend::OpenGL;
        }
        else if (key == "--capture-dir") cli.render.capture_dir = value();
        else if (key == "--frame-skip") cli.render.frame_skip = true;  // as on the Vita
        else if (key == "--skip-when-behind") cli.render.skip_when_behind = true;  // with --frame-skip
        else if (key == "--offscreen-1x") cli.render.offscreen_1x = true;  // effect buffers at 1x
        else if (key == "--cache-sublists") cli.render.cache_sublists = true;  // unchanged sub-lists from a copy
        else if (key == "--async-ge") o.async_ge = true;  // display lists on another thread, as on the Vita
        else if (key == "--sas-thread") burnout::sas_worker::start(0);  // SAS mixing on its own thread, as on the Vita
        else if (key == "--music-dir") {  // converted tracks (tools/convert_music.py)
            std::cerr << "music tracks: " << burnout::music::set_dir(value()) << "\n";
        }
        else if (key == "--sound-dump") {  // sound on, each audio channel written as raw s16 stereo to DIR
            std::string error;
            burnout::audio::start(0, value(), error);
        }
        else if (key == "--scale") cli.render.render_scale = std::stof(value());  // internal resolution x480x272
        else if (key == "--filters") {  // bonus filters by id, comma-separated (captures show them)
            std::vector<int> active;
            std::string list = value() + ",";
            for (std::size_t at = 0, comma; (comma = list.find(',', at)) != std::string::npos; at = comma + 1u)
                if (const int index = burnout::filters::find(list.substr(at, comma - at)); index >= 0) active.push_back(index);
            burnout::gl::set_filters(active);
        }
        else if (key == "--texture-pack") burnout::texture_pack::open(value());  // PPSSPP pack folder (textures.ini)
        else if (key == "--trace-draws") {  // FROM-TO vblanks; implies --render
            cli.render.enabled = true;
            const std::string range = value();
            const auto dash = range.find('-');
            cli.render.trace_from = std::stoull(range.substr(0, dash));
            cli.render.trace_to = dash == std::string::npos ? cli.render.trace_from : std::stoull(range.substr(dash + 1));
        }
        else if (key == "--capture") {  // comma-separated vblanks; implies --render
            cli.render.enabled = true;
            std::istringstream list(value());
            for (std::string item; std::getline(list, item, ',');) cli.render.capture_vblanks.insert(std::stoull(item));
        }
        else throw psprecomp::Error("unknown option " + key);
    }
    return cli;
}

} // namespace

// Block execution counts of a PSPRECOMP_BLOCK_COUNTS corpus (16 MiB of code
// space, one counter per instruction slot); written as the list of executed
// block addresses when PSPRECOMP_BLOCK_COUNTS_OUT names a file.
extern "C" {
std::uint32_t psprecomp_block_counts[1u << 22];
}

namespace {
void write_block_counts() {
    const char *path = std::getenv("PSPRECOMP_BLOCK_COUNTS_OUT");
    if (path == nullptr) return;
    std::ofstream out(path);
    for (std::uint32_t i = 0; i < (1u << 22); ++i)
        if (psprecomp_block_counts[i] != 0u) out << std::hex << (0x08800000u + i * 4u) << ' ' << std::dec << psprecomp_block_counts[i] << '\n';
}
} // namespace

int main(int argc, char **argv) {
    try {
        Cli cli = parse(argc, argv);
        if (!cli.script.empty()) cli.bench.script.load(cli.script);
#if defined(_WIN32)
        std::atomic<bool> measuring{false};
        std::unique_ptr<burnout::SamplingProfiler> profiler;
        if (!cli.profile.empty()) {
            cli.bench.measuring = &measuring;
            profiler = std::make_unique<burnout::SamplingProfiler>(measuring);
        }
#endif
        burnout::render::configure(cli.render);
        burnout::BenchResult result = burnout::run_benchmark(cli.bench);
        result.report += burnout::render::report();
#if defined(_WIN32)
        if (profiler) profiler->write_report(cli.profile);
#endif
        write_block_counts();
        std::cout << result.report;
        std::ofstream(cli.report) << result.report;
        return result.complete ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "burnout_headless error: " << e.what() << "\n";
        return 2;
    }
}
