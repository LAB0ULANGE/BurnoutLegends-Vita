// Headless Burnout Legends runner (PC): boots the recompiled executable with
// the burnout HLE, drives it with a scripted controller and reports host time
// per logical game frame. No audio output; --render draws the GE lists with the
// software rasterizer and --capture writes PNG frames.
#include "trax.hpp"
#include "audio_out.hpp"
#include "texture_pack.hpp"
#include "bonus.hpp"
#include "bonus_menu.hpp"
#include "paints.hpp"
#include "overlay.hpp"
#include "game_setup.hpp"
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

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
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
        else if (key == "--psp-picture") {  // the Vita's DISPLAY: PSP setting (480x272 inside 960x544 targets)
            burnout::gl::set_valid_region(true);
            burnout::gl::set_dynamic_resolution(true, 1.5f);
            burnout::gl::set_picture_scale(1.0f);
        }
        else if (key == "--dynamic-pin") {  // the Vita's dynamic resolution held at SCALE (1.5: 720x408), valid + tight regions
            burnout::gl::set_valid_region(true);
            burnout::gl::set_tight_regions(true);
            burnout::gl::set_dynamic_resolution(true, 1.5f, std::stof(value()));
        }
        else if (key == "--capture-dir") cli.render.capture_dir = value();
        else if (key == "--frame-skip") cli.render.frame_skip = true;  // as on the Vita
        else if (key == "--skip-when-behind") cli.render.skip_when_behind = true;  // with --frame-skip
        else if (key == "--offscreen-1x") cli.render.offscreen_1x = true;  // effect buffers at 1x
        else if (key == "--cache-sublists") cli.render.cache_sublists = true;  // unchanged sub-lists from a copy
        else if (key == "--async-ge") o.async_ge = true;  // display lists on another thread, as on the Vita
        else if (key == "--custom-music") burnout::trax::configure(value(), "");  // DIR: MP3s added to EA Trax (trax.cpp)
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
        else if (key == "--dump-ram") {  // V1,V2,... DIR: guest RAM at those vblanks
            std::string list = value() + ",";
            for (std::size_t at = 0, comma; (comma = list.find(',', at)) != std::string::npos; at = comma + 1u)
                if (comma > at) o.ram_dump_vblanks.push_back(std::stoull(list.substr(at, comma - at)));
            o.ram_dump_dir = value();
        }
        else if (key == "--poke") {  // V:ADDR=WORD,... guest words written at every vblank from V (research)
            static std::vector<std::pair<std::uint32_t, std::uint32_t>> pokes;
            static std::uint64_t from = 0;
            std::string list = value() + ",";
            const auto colon = list.find(':');
            from = std::stoull(list.substr(0, colon));
            for (std::size_t at = colon + 1u, comma; (comma = list.find(',', at)) != std::string::npos; at = comma + 1u) {
                const std::string item = list.substr(at, comma - at);
                const auto eq = item.find('=');
                if (eq != std::string::npos)
                    pokes.emplace_back(static_cast<std::uint32_t>(std::stoul(item.substr(0, eq), nullptr, 16)),
                                       static_cast<std::uint32_t>(std::stoul(item.substr(eq + 1u), nullptr, 16)));
            }
            o.per_vblank = [](psprecomp::GuestMemory &memory) {
                static std::uint64_t vblank = 0;
                if (++vblank < from) return;
                // An odd address writes the half-word at address - 1.
                for (const auto &[address, word] : pokes)
                    if (std::uint8_t *p = memory.raw_pointer(address & ~1u, 4u)) std::memcpy(p, &word, (address & 1u) != 0u ? 2u : 4u);
            };
        }
        else if (key == "--make-postcards") {  // PICTURE DIR: every Bonus filter applied to PICTURE, DIR/<id>.ppm (needs --gl and a --capture)
            const std::string picture = value();
            burnout::gl::make_postcards(picture, value());
        }
        else if (key == "--antialias") {  // 0 off, 1 FXAA, 2 light (Vita default)
            const int mode = std::stoi(value());
            burnout::gl::set_antialias_lite(mode == 2);
            burnout::gl::set_antialias(mode != 0);
        }
        else if (key == "--bonus-menu") {  // the Bonus row in the pause menu (every filter available)
            static burnout::bonus::State state;
            {
                std::string font = __FILE__;
                font = font.substr(0, font.find_last_of("/\\") + 1u) + "third_party/fonts/Oswald-Bold.ttf";
                if (!burnout::overlay::init(font)) std::fprintf(stderr, "overlay: no font at %s\n", font.c_str());
                std::string cards = __FILE__;
                cards = cards.substr(0, cards.find_last_of("/\\") + 1u) + "../../../../original/postcards";
                burnout::gl::set_postcard_pictures(cards);
                std::string photos = __FILE__;
                photos = photos.substr(0, photos.find_last_of("/\\") + 1u) + "../../../../original/signatures";
                burnout::bonus_menu::set_signature_photos(photos, [](const std::string &path, float x, float y, float w, float h, float angle) {
                    burnout::gl::set_photo(path, x, y, w, h, angle);
                });
            }
            burnout::bonus_menu::configure({
                [] { return state.enabled; },
                [] { return static_cast<int>(burnout::filters::all().size()); },
                [](int i) { return burnout::filters::all()[static_cast<std::size_t>(i)].name; },
                [](int i) { return burnout::bonus::available(state, i); },
                [](int i) {
                    for (const auto &id : state.active)
                        if (burnout::filters::find(id) == i) return true;
                    return false;
                },
                [](int i) {
                    burnout::bonus::toggle(state, i);
                    burnout::gl::set_filters(burnout::bonus::applied(state));
                    std::fprintf(stderr, "[bonus] %s\n", burnout::filters::describe(burnout::bonus::applied(state)).c_str());
                },
                [](int i) {
                    return std::string("LOCKED: ") + burnout::bonus::signature_name(burnout::bonus::filter_signature(i));
                },
                [](int signature) {
                    const int filter = burnout::bonus::signature_filter(signature);
                    if (!burnout::bonus::unlock(state, filter)) return false;
                    std::fprintf(stderr, "[bonus] Signature Takedown %s: %s unlocked\n", burnout::bonus::signature_name(signature),
                                 burnout::filters::all()[static_cast<std::size_t>(filter)].name);
                    return true;
                },
                [](int i) {
                    const int signature = burnout::bonus::filter_signature(i);
                    return signature < 0 ? std::string() : std::string(burnout::bonus::signature_name(signature));
                },
                [](int i, float x, float y, float w, float h, float angle) {
                    burnout::gl::set_postcard(i >= 0 ? std::vector<int>{i} : std::vector<int>{}, x, y, w, h, angle);
                },
                [] { burnout::gl::postcard_photo(); },
        [](int i) {
                    const burnout::bonus::Reward reward = burnout::bonus::filter_reward(i);
                    return reward.source == burnout::bonus::Source::Free ? -1 : static_cast<int>(reward.source) * 100 + reward.index;
                },
                [](float mph) { burnout::gl::set_car_speed(mph); },
                [](int postcards, int headlines, int percent) {
                    const int unlocked = burnout::bonus::unlock_progress(state, postcards, headlines, percent);
                    if (unlocked > 0) std::fprintf(stderr, "[bonus] progress %d postcards, %d headlines, %d %%: %d filters unlocked\n", postcards,
                                                   headlines, percent, unlocked);
                    return unlocked;
                },
                {},  // no BONUS VITA / DISPLAY rows (as on the Vita)
                {},
                {},
                {},
                // BONUS 2.0's pictures: files of original/ (bonus/, signatures/).
                [](int slot, const std::string &path, float x, float y, float w, float h, float angle) {
                    static const std::string root = [] {
                        std::string here = __FILE__;
                        return here.substr(0, here.find_last_of("/\\") + 1u) + "../../../../original/";
                    }();
                    burnout::gl::set_picture(slot, path.empty() ? path : root + path, x, y, w, h, angle);
                },
                [](bool hide) { burnout::gl::set_hide_3d(hide); },
            });
            burnout::paints::set_texels_written([](std::uint32_t address, std::uint32_t bytes) { burnout::render::cpu_wrote_vram(address, bytes); });
            o.per_vblank = [](psprecomp::GuestMemory &memory) {
                burnout::bonus_menu::update(memory);
            };
            o.button_filter = [](std::uint32_t buttons) { return burnout::bonus_menu::filter_buttons(buttons); };
            o.guest_call = [](std::uint32_t &f, std::uint32_t &a0, std::uint32_t &a1) { return burnout::bonus_menu::take_guest_call(f, a0, a1); };
        }
        else if (key == "--paint") burnout::paints::set_chosen(std::stoi(value()));  // N: extra paint chosen at launch (as unlocks.bin)
        else if (key == "--force-signature") {
            // N[,N...]:V (after --bonus-menu): Signature Takedowns N (0-19,
            // Takedown Book order) marked done in the live profile from vblank
            // V, as if earned in the race (reward screen tests without playing).
            static std::vector<int> signatures;
            static std::uint64_t from = 0;
            static std::function<void(psprecomp::GuestMemory &)> before;
            const std::string v = value();
            const auto colon = v.find(':');
            std::istringstream list(v.substr(0, colon));
            for (std::string item; std::getline(list, item, ',');) signatures.push_back(std::stoi(item));
            from = colon == std::string::npos ? 0u : std::stoull(v.substr(colon + 1u));
            before = o.per_vblank;
            o.per_vblank = [](psprecomp::GuestMemory &memory) {
                static std::uint64_t vblank = 0;
                if (++vblank == from)
                    for (const int signature : signatures) {
                        if (signature < 0 || signature >= 20) continue;
                        constexpr std::uint32_t kProfileSignatures = 0x08DC31D0u + 0xF61Eu;
                        if (std::uint8_t *done = memory.raw_pointer(kProfileSignatures + static_cast<std::uint32_t>(signature), 1u)) {
                            std::fprintf(stderr, "[test] vblank %llu: Signature Takedown %d marked done (was %u)\n",
                                         static_cast<unsigned long long>(vblank), signature, *done);
                            *done = 1u;
                        }
                    }
                if (before) before(memory);
            };
        }
        else if (key == "--texture-pack") burnout::texture_pack::open(value());  // PPSSPP pack folder (textures.ini)
        else if (key == "--install-from-iso") {  // ISO OUTDIR: the Vita first-launch install, then exit
            const std::string image = value();
            std::string out = value();
            if (!out.empty() && out.back() != '/') out += '/';
            int last = -1;
            const auto outcome = burnout::setup::install_from(out, image, [&](const std::string &step, const std::string &detail, double f) {
                const int pct = static_cast<int>(f * 100.0);
                if (pct != last || step != "Copying the game files") std::fprintf(stderr, "[setup] %s %s %d%%\n", step.c_str(), detail.c_str(), pct);
                last = pct;
            });
            std::fprintf(stderr, "[setup] result %d: %s\n", static_cast<int>(outcome.result), outcome.message.c_str());
            std::exit(outcome.result == burnout::setup::Result::Ready ? 0 : 1);
        }
        else if (key == "--trace-draws") {  // FROM-TO vblanks; implies --render
            cli.render.enabled = true;
            const std::string range = value();
            const auto dash = range.find('-');
            cli.render.trace_from = std::stoull(range.substr(0, dash));
            cli.render.trace_to = dash == std::string::npos ? cli.render.trace_from : std::stoull(range.substr(dash + 1));
        }
        else if (key == "--capture") {  // comma-separated vblanks or FROM-TO/STEP ranges; implies --render
            cli.render.enabled = true;
            std::istringstream list(value());
            for (std::string item; std::getline(list, item, ',');) {
                const std::size_t dash = item.find('-');
                if (dash == std::string::npos) {
                    cli.render.capture_vblanks.insert(std::stoull(item));
                    continue;
                }
                const std::size_t slash = item.find('/');
                const std::uint64_t from = std::stoull(item.substr(0, dash));
                const std::uint64_t to = std::stoull(item.substr(dash + 1, slash == std::string::npos ? std::string::npos : slash - dash - 1));
                const std::uint64_t step = slash == std::string::npos ? 1u : std::max<std::uint64_t>(1u, std::stoull(item.substr(slash + 1)));
                for (std::uint64_t v = from; v <= to; v += step) cli.render.capture_vblanks.insert(v);
            }
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
