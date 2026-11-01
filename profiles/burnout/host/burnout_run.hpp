#pragma once

#include "burnout_bench.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace burnout {

inline constexpr std::uint64_t kAutoMeasureStart = UINT64_MAX;

struct BenchOptions {
    std::filesystem::path elf;
    std::filesystem::path usrdir;
    std::filesystem::path disc_root;
    std::filesystem::path memstick;
    InputScript script;
    std::vector<std::filesystem::path> nid_tables;
    std::uint64_t max_vblanks{60u * 60u * 5u};
    std::uint64_t measure_start{kAutoMeasureStart};
    std::uint64_t measure_vblanks{3600u};      // 60 s of PSP time
    std::uint64_t auto_settle_vblanks{600u};   // race intro + countdown
    std::string race_marker{"/streamed.dat"};  // first opened when a race loads
    std::filesystem::path frames_csv;
    // Optional: before the measured window, every 60 vblanks, one line with the user
    // RAM hash and the hash of each 64 KiB block (to locate host divergences).
    std::filesystem::path hash_trace;
    // Append renderer/audio workload statistics for the window to the report.
    bool workload{};
    std::string title{"burnout-bench"};
    bool verbose{};
    bool trace_files{};
    std::atomic<bool> *measuring{};  // optional: set while the window is open (profiler)
};

struct BenchResult {
    std::string report;
    std::string stop_reason;
    bool complete{};
    double host_seconds{};
};

// Boots the recompiled game with the headless HLE and measures the race window.
[[nodiscard]] BenchResult run_benchmark(const BenchOptions &options);

} // namespace burnout
