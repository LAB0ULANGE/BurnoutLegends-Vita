// Headless Burnout Legends runner (PC): boots the recompiled executable with
// the burnout HLE, drives it with a scripted controller and reports host time
// per logical game frame. No audio output; --render draws the GE lists with the
// software rasterizer and --capture writes PNG frames.
#include "burnout_run.hpp"
#include "ge_render.hpp"

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
    burnout::render::Options render{false, {}, "out/burnout-frames"};
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
        else if (key == "--capture-dir") cli.render.capture_dir = value();
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
        std::cout << result.report;
        std::ofstream(cli.report) << result.report;
        return result.complete ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "burnout_headless error: " << e.what() << "\n";
        return 2;
    }
}
