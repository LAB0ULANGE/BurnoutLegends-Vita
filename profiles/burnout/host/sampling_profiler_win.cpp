#include "sampling_profiler_win.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace burnout {

namespace {

constexpr int kDepth = 4;  // leaf + 3 callers

struct Sample {
    DWORD64 frames[kDepth];
};

// Unwinds `context` in place; must not allocate (the target thread is suspended
// and may hold the heap lock).
int unwind(CONTEXT context, DWORD64 *frames) {
    int count = 0;
    while (count < kDepth && context.Rip != 0u) {
        frames[count++] = context.Rip;
        DWORD64 image_base = 0u;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // Leaf function without unwind data: the return address is at [rsp].
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8u;
            continue;
        }
        void *handler_data = nullptr;
        DWORD64 establisher = 0u;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                         nullptr);
    }
    for (int i = count; i < kDepth; ++i) frames[i] = 0u;
    return count;
}

void safe_unwind(const CONTEXT &context, DWORD64 *frames) {
    __try {
        (void)unwind(context, frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

struct SamplingProfiler::State {
    const std::atomic<bool> &active;
    HANDLE target{};
    std::atomic<bool> quit{false};
    std::vector<Sample> samples;
    std::thread worker;

    explicit State(const std::atomic<bool> &flag) : active(flag) {}

    void run() {
        timeBeginPeriod(1u);
        CONTEXT context{};
        while (!quit.load(std::memory_order_relaxed)) {
            Sleep(1u);
            if (!active.load(std::memory_order_relaxed)) continue;
            if (SuspendThread(target) == static_cast<DWORD>(-1)) continue;
            context.ContextFlags = CONTEXT_FULL;
            Sample sample{};
            if (GetThreadContext(target, &context)) safe_unwind(context, sample.frames);
            ResumeThread(target);
            if (sample.frames[0] != 0u) samples.push_back(sample);
        }
        timeEndPeriod(1u);
    }
};

SamplingProfiler::SamplingProfiler(const std::atomic<bool> &active) : state_(std::make_unique<State>(active)) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &state_->target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0u);
    state_->samples.reserve(1u << 20);
    state_->worker = std::thread([this] { state_->run(); });
}

SamplingProfiler::~SamplingProfiler() {
    state_->quit = true;
    if (state_->worker.joinable()) state_->worker.join();
    CloseHandle(state_->target);
}

void SamplingProfiler::write_report(const std::filesystem::path &path) {
    state_->quit = true;
    if (state_->worker.joinable()) state_->worker.join();

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    std::map<DWORD64, std::string> names;
    const auto name_of = [&](DWORD64 address) -> const std::string & {
        auto it = names.find(address);
        if (it != names.end()) return it->second;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0u;
        std::string name = SymFromAddr(process, address, &displacement, symbol) ? std::string(symbol->Name) : "?";
        return names.emplace(address, std::move(name)).first->second;
    };

    std::map<std::string, std::size_t> self, caller_of_leaf;
    for (const auto &s : state_->samples) {
        const std::string &leaf = name_of(s.frames[0]);
        ++self[leaf];
        const std::string &parent = s.frames[1] != 0u ? name_of(s.frames[1]) : std::string("-");
        ++caller_of_leaf[leaf + "  <-  " + parent];
    }
    const double total = static_cast<double>(std::max<std::size_t>(state_->samples.size(), 1u));
    const auto dump = [&](std::ofstream &out, const std::map<std::string, std::size_t> &table, std::size_t limit) {
        std::vector<std::pair<std::size_t, std::string>> rows;
        for (const auto &[name, count] : table) rows.emplace_back(count, name);
        std::sort(rows.rbegin(), rows.rend());
        for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i) {
            char pct[32];
            std::snprintf(pct, sizeof(pct), "%6.2f%% %8zu  ", 100.0 * static_cast<double>(rows[i].first) / total,
                          rows[i].first);
            out << pct << rows[i].second << "\n";
        }
    };
    // Leaf source lines, so hot code inside the giant generated units can be
    // traced back to guest labels.
    std::map<std::string, std::size_t> lines;
    for (const auto &s : state_->samples) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD displacement = 0u;
        if (!SymGetLineFromAddr64(process, s.frames[0], &displacement, &line)) continue;
        std::string file = line.FileName;
        if (const auto slash = file.find_last_of("\\/"); slash != std::string::npos) file.erase(0, slash + 1u);
        ++lines[file + ":" + std::to_string(line.LineNumber)];
    }
    std::ofstream out(path);
    out << "samples: " << state_->samples.size() << "\n\n== self time by function ==\n";
    dump(out, self, 200);
    out << "\n== leaf <- caller ==\n";
    dump(out, caller_of_leaf, 200);
    out << "\n== leaf source line ==\n";
    dump(out, lines, 400);
    SymCleanup(process);
}

} // namespace burnout
