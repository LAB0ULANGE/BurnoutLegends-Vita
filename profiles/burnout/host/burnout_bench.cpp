#include "burnout_bench.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <thread>
#endif

namespace burnout {

namespace {

std::uint32_t button_mask(const std::string &name) {
    static const std::map<std::string, std::uint32_t> buttons{
        {"SELECT", 0x0001u}, {"START", 0x0008u}, {"UP", 0x0010u}, {"RIGHT", 0x0020u},
        {"DOWN", 0x0040u}, {"LEFT", 0x0080u}, {"L", 0x0100u}, {"R", 0x0200u},
        {"TRIANGLE", 0x1000u}, {"CIRCLE", 0x2000u}, {"CROSS", 0x4000u}, {"SQUARE", 0x8000u},
    };
    const auto it = buttons.find(name);
    if (it == buttons.end()) throw std::runtime_error("unknown button in input script: " + name);
    return it->second;
}

struct Stats {
    std::size_t count{};
    double min{}, mean{}, p50{}, p99{}, max{};
};

Stats summarize(std::vector<double> values) {
    Stats stats;
    if (values.empty()) return stats;
    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();
    stats.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    const auto percentile = [&](double p) {
        const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1u) + 0.5);
        return values[std::min(index, values.size() - 1u)];
    };
    stats.p50 = percentile(0.50);
    stats.p99 = percentile(0.99);
    return stats;
}

std::string format_stats(const char *label, const Stats &s) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%-28s frames=%lu  ms/frame min=%.3f avg=%.3f p50=%.3f p99=%.3f max=%.3f\n",
                  label, static_cast<unsigned long>(s.count), s.min, s.mean, s.p50, s.p99, s.max);
    return line;
}

} // namespace

double host_now_ms() {
#if defined(__vita__)
    return static_cast<double>(sceKernelGetProcessTimeWide()) / 1000.0;
#else
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
#endif
}

void host_sleep_ms(double ms) {
    if (ms <= 0.0) return;
#if defined(__vita__)
    sceKernelDelayThread(static_cast<SceUInt>(ms * 1000.0));
#else
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
#endif
}

void InputScript::load(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open input script " + path.string());
    std::string line;
    while (std::getline(in, line)) parse_line(line);
}

void InputScript::parse_line(const std::string &raw) {
    const std::string line = raw.substr(0, raw.find('#'));
    std::istringstream words(line);
    std::string verb;
    if (!(words >> verb)) return;
    Rule rule;
    if (verb == "hold") {
        words >> rule.from >> rule.to;
    } else if (verb == "pulse") {
        words >> rule.from >> rule.to >> rule.period >> rule.length;
        if (rule.period == 0u) throw std::runtime_error("pulse period must be non-zero: " + raw);
    } else {
        throw std::runtime_error("unknown input script verb: " + raw);
    }
    std::string button;
    while (words >> button) rule.buttons |= button_mask(button);
    rules_.push_back(rule);
}

std::uint32_t InputScript::buttons_at(std::uint64_t vblank) const {
    std::uint32_t buttons = 0u;
    for (const auto &rule : rules_) {
        if (vblank < rule.from || vblank >= rule.to) continue;
        if (rule.period == 0u || (vblank - rule.from) % rule.period < rule.length) buttons |= rule.buttons;
    }
    return buttons;
}

void FrameClock::on_wait_vblank(std::int32_t thread_uid, std::uint64_t vblank) {
    const double ms = host_now_ms() - origin_;
    waits_.push_back({vblank, thread_uid, ms});
}

void FrameClock::on_set_frame_buffer(std::uint64_t vblank) {
    const double ms = host_now_ms() - origin_;
    flips_.push_back({vblank, 0, ms});
}

void FrameClock::set_window(std::uint64_t start_vblank, std::uint64_t length_vblanks) {
    start_ = start_vblank;
    length_ = length_vblanks;
}

bool FrameClock::window_open(std::uint64_t vblank) const noexcept {
    return start_ != UINT64_MAX && vblank >= start_ && vblank < start_ + length_;
}

bool FrameClock::window_done(std::uint64_t vblank) const noexcept {
    return start_ != UINT64_MAX && vblank >= start_ + length_;
}

std::string FrameClock::report(const std::string &title) const {
    std::ostringstream out;
    out << title << "\n";
    const std::uint64_t end = start_ == UINT64_MAX ? 0u : start_ + length_;
    out << "window: vblanks [" << (start_ == UINT64_MAX ? 0u : start_) << ", " << end << ")  ("
        << static_cast<double>(length_) / 59.94 << " s of PSP time)\n";

    // The main loop thread is the one that waits for vblank most often.
    std::map<std::int32_t, std::size_t> per_thread;
    for (const auto &w : waits_)
        if (window_open(w.vblank)) ++per_thread[w.thread];
    std::int32_t main_thread = -1;
    std::size_t most = 0u;
    for (const auto &[uid, count] : per_thread)
        if (count > most) { most = count; main_thread = uid; }

    // A logical frame = interval between two consecutive vblank waits of the main
    // loop thread; its vblank cadence tells how many PSP refreshes it spans.
    std::vector<double> frame_ms;
    std::map<std::uint64_t, std::size_t> cadence;
    const Sample *previous = nullptr;
    for (const auto &w : waits_) {
        if (w.thread != main_thread || !window_open(w.vblank)) continue;
        if (previous != nullptr) {
            frame_ms.push_back(w.host_ms - previous->host_ms);
            ++cadence[w.vblank - previous->vblank];
        }
        previous = &w;
    }
    out << "main loop thread uid=" << main_thread << "\n";
    out << format_stats("main-loop frame (host)", summarize(frame_ms));
    out << "vblanks per main-loop frame:";
    for (const auto &[vblanks, count] : cadence) out << "  " << vblanks << "x" << count;
    out << "\n";

    std::vector<double> flip_ms;
    const Sample *last_flip = nullptr;
    for (const auto &f : flips_) {
        if (!window_open(f.vblank)) continue;
        if (last_flip != nullptr) flip_ms.push_back(f.host_ms - last_flip->host_ms);
        last_flip = &f;
    }
    out << format_stats("frame-buffer flip (host)", summarize(flip_ms));
    out << "target: 33.333 ms/frame (30 fps)\n";
    return out.str();
}

void FrameClock::write_frames_csv(const std::filesystem::path &path) const {
    std::ofstream out(path);
    out << "kind,vblank,thread,host_ms\n";
    for (const auto &w : waits_) out << "wait," << w.vblank << ',' << w.thread << ',' << w.host_ms << '\n';
    for (const auto &f : flips_) out << "flip," << f.vblank << ",0," << f.host_ms << '\n';
}

} // namespace burnout
