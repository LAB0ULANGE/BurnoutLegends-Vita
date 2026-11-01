#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

namespace burnout {

// Windows-only in-process sampling profiler: a helper thread suspends the
// calling thread about once per millisecond while `active` is true and records
// the instruction pointer plus a few unwound callers. Symbolised with DbgHelp
// (needs a PDB) when the report is written.
class SamplingProfiler {
public:
    explicit SamplingProfiler(const std::atomic<bool> &active);
    ~SamplingProfiler();
    void write_report(const std::filesystem::path &path);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace burnout
