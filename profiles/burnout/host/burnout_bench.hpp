#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace burnout {

// Monotonic host time in milliseconds (process time on Vita, steady_clock elsewhere).
[[nodiscard]] double host_now_ms();
void host_sleep_ms(double ms);

// Scripted controller input keyed on the PSP vblank counter.
//   hold  <from> <to> <BUTTONS...>                 buttons held for vblanks [from, to)
//   pulse <from> <to> <period> <length> <BUTTONS...> pressed for <length> every <period>
// Buttons: SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE.
class InputScript {
public:
    void load(const std::filesystem::path &path);
    void parse_line(const std::string &line);
    [[nodiscard]] std::uint32_t buttons_at(std::uint64_t vblank) const;
    [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }

private:
    struct Rule {
        std::uint64_t from{};
        std::uint64_t to{};
        std::uint64_t period{};  // 0 = continuous hold
        std::uint64_t length{};
        std::uint32_t buttons{};
    };
    std::vector<Rule> rules_;
};

// Measures host time per logical game frame. A logical frame ends when the
// game's main loop thread waits for vblank; idle virtual time costs no host time.
class FrameClock {
public:
    void on_wait_vblank(std::int32_t thread_uid, std::uint64_t vblank);
    void on_set_frame_buffer(std::uint64_t vblank);
    void set_window(std::uint64_t start_vblank, std::uint64_t length_vblanks);
    [[nodiscard]] bool window_open(std::uint64_t vblank) const noexcept;
    [[nodiscard]] bool window_done(std::uint64_t vblank) const noexcept;
    [[nodiscard]] std::uint64_t window_start() const noexcept { return start_; }

    [[nodiscard]] std::string report(const std::string &title) const;
    void write_frames_csv(const std::filesystem::path &path) const;

private:
    struct Sample {
        std::uint64_t vblank{};
        std::int32_t thread{};
        double host_ms{};
    };
    double origin_{host_now_ms()};
    std::vector<Sample> waits_;
    std::vector<Sample> flips_;
    std::uint64_t start_{UINT64_MAX};
    std::uint64_t length_{};
};

} // namespace burnout
