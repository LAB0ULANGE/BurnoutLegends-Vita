// Headless HLE for Burnout Legends (ULES00125).
//
// Threads are cooperative: every import runs at the outer dispatcher with the
// whole guest state in AllegrexContext, so a blocking kernel call saves the
// caller's context (resuming at $ra) and loads another thread's context.
//
// Time is virtual and deterministic. Guest work costs no virtual time; when no
// thread is runnable the clock jumps to the next event (vblank, delay expiry or
// audio buffer drain). Host time spent between two game frames is therefore the
// pure CPU cost of that frame, which is what the benchmark measures.
#include "burnout_hle.hpp"
#include "audio_out.hpp"
#include "burnout_bench.hpp"
#include "music.hpp"
#include "sas_core.hpp"

#include "workload_stats.hpp"
#if defined(BURNOUT_GE_RENDER)
#include "ge_gl.hpp"
#include "ge_render.hpp"
#endif

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <system_error>

#if defined(BURNOUT_VITA_PROFILE)
extern "C" volatile std::uint32_t psprecomp_profile_label;
#endif

namespace burnout {
namespace {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;
namespace fs = std::filesystem;

constexpr std::uint32_t kThreadReturnPc = 0x00000000u;
constexpr std::uint32_t kInterruptReturnPc = 0x00000004u;
constexpr std::uint32_t kGuestCallReturnPc = 0x00000008u;
constexpr std::uint32_t kUserRamEnd = 0x0A000000u;
constexpr std::uint32_t kVblankInterrupt = 30u;

constexpr std::uint32_t kErrorUnknownUid = 0x800200CBu;
constexpr std::uint32_t kErrorUnknownThread = 0x80020198u;
constexpr std::uint32_t kErrorUnknownSema = 0x80020199u;
constexpr std::uint32_t kErrorUnknownEventFlag = 0x8002019Au;
constexpr std::uint32_t kErrorNotDormant = 0x800201A4u;
constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrorSemaZero = 0x80020201u;
constexpr std::uint32_t kErrorSemaOverflow = 0x80020202u;
constexpr std::uint32_t kErrorEventFlagCondition = 0x8002021Cu;
constexpr std::uint32_t kErrorNoMemory = 0x80020190u;
constexpr std::uint32_t kErrorFileNotFound = 0x80010002u;
constexpr std::uint32_t kErrorBadFile = 0x80010009u;
constexpr std::uint32_t kErrorAudioBusy = 0x80260002u;
constexpr std::uint32_t kSavedataNoData = 0x80110307u;
constexpr std::uint32_t kUtilityUnsupported = 0x80110001u;

enum class ThreadState { Ready, Running, Waiting, Dormant };
enum class WaitKind { None, Delay, Sema, EventFlag, ThreadEnd, Vblank };

struct Thread {
    std::string name;
    std::uint32_t entry{};
    std::uint32_t priority{};
    std::uint32_t attr{};
    std::uint32_t stack_size{};
    std::uint32_t stack_block{};
    std::uint32_t stack_bottom{};
    std::uint32_t gp{};
    ThreadState state{ThreadState::Dormant};
    AllegrexContext ctx{};
    WaitKind wait{WaitKind::None};
    std::int32_t wait_id{};
    std::uint32_t wait_a{};
    std::uint32_t wait_b{};
    std::uint32_t wait_out{};
    std::uint32_t timeout_ptr{};
    std::uint64_t deadline{};  // 0 = none
    std::uint32_t exit_status{};
    double host_ms{};  // host time spent running this thread (thread_time_report)
    std::uint32_t wakeup_count{};
    std::int64_t ready_seq{};
};

struct Semaphore {
    std::string name;
    std::int32_t count{};
    std::int32_t max{};
};

struct EventFlag {
    std::string name;
    std::uint32_t attr{};
    std::uint32_t bits{};
};

struct MemoryBlock {
    std::uint32_t uid{};
    std::uint32_t address{};
    std::uint32_t size{};
    std::string name;
};

struct OpenFile {
    std::FILE *file{};
    std::string psp_path;
    std::int64_t async_result{};
};

struct DirectoryItem {
    std::string name;
    fs::path path;
    bool directory{};
    std::uint64_t size{};
};

struct OpenDirectory {
    std::vector<DirectoryItem> entries;
    std::size_t next{};
};

struct PendingInterrupt {
    std::uint32_t handler{};
    std::uint32_t a0{};
    std::uint32_t a1{};
    std::uint32_t a2{};
};

struct SubInterrupt {
    std::uint32_t handler{};
    std::uint32_t arg{};
    bool enabled{};
};

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint64_t busy_until{};
    bool mono{};
};

struct GeCallback {
    std::uint32_t signal_func{};
    std::uint32_t signal_arg{};
    std::uint32_t finish_func{};
    std::uint32_t finish_arg{};
};

struct GeList {
    std::uint32_t pc{};
    std::uint32_t stall{};
    std::int32_t callback{-1};
    std::uint32_t base{};
    std::uint32_t offset{};
    std::vector<std::uint32_t> call_stack;
    bool done{};
};

struct UtilityDialog {
    std::uint32_t status{};
    std::uint32_t params{};
    std::uint32_t result{};
};

// A guest function invoked from an import (e.g. the MPEG ringbuffer read
// callback). It runs on the calling thread; on return to kGuestCallReturnPc the
// caller resumes at its $ra with the import result computed from the callee's v0.
struct GuestCallFrame {
    AllegrexContext caller{};
    std::function<std::uint32_t(Runtime &, std::uint32_t callee_v0)> on_return;
};

struct MpegRingbuffer {
    std::uint32_t address{};
    bool end_of_stream{};
};

struct Kernel {
    HleOptions options;
    HleObserver observer;

    std::uint64_t time_us{};
    std::uint64_t vblanks{};
    std::uint64_t next_vblank_us{kVblankPeriodUs};
    std::uint64_t idle_vblanks{};

    std::int32_t next_uid{0x1000};
    std::map<std::int32_t, Thread> threads;
    std::int32_t current{-1};
    std::int64_t ready_seq{};
    std::int64_t preempt_seq{};

    std::map<std::int32_t, Semaphore> semaphores;
    std::map<std::int32_t, EventFlag> event_flags;
    std::set<std::int32_t> callbacks;

    std::vector<MemoryBlock> blocks;  // sorted by address
    std::uint32_t arena_low{};
    std::uint32_t arena_high{kUserRamEnd};
    std::uint32_t interrupt_stack_top{};

    std::map<std::int32_t, OpenFile> files;
    std::map<std::int32_t, OpenDirectory> directories;
    std::int32_t next_fd{3};
    // UMD directory entries carry the file's start sector in st_private[0] and
    // the game indexes its file table by it. Sectors are synthesized: unique,
    // stable for a run and laid out like a mastered disc.
    std::map<std::string, std::uint32_t> lba_by_path;
    std::map<std::uint32_t, fs::path> path_by_lba;
    std::uint32_t next_lba{0x8000u};

    std::deque<PendingInterrupt> pending_interrupts;
    std::map<std::uint32_t, std::map<std::uint32_t, SubInterrupt>> sub_interrupts;
    bool in_interrupt{};
    bool interrupted_valid{};
    AllegrexContext interrupted{};
    std::int32_t interrupted_uid{-1};

    std::array<AudioChannel, 8> audio{};
    std::uint32_t sas_grain{256u};

    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::map<std::int32_t, GeList> ge_lists;
    std::int32_t next_ge_callback{0};
    std::int32_t next_ge_list{0};
    std::uint64_t ge_lists_submitted{};
    std::uint64_t ge_commands_scanned{};

    UtilityDialog savedata;
    UtilityDialog msg_dialog;
    UtilityDialog netconf;
    UtilityDialog game_sharing;

    std::map<std::int32_t, std::vector<GuestCallFrame>> guest_calls;
    std::map<std::uint32_t, MpegRingbuffer> mpeg_ringbuffers;    // by ringbuffer address
    std::map<std::uint32_t, std::uint32_t> mpeg_ringbuffer_of;   // mpeg handle -> ringbuffer

    std::set<std::pair<std::string, std::uint32_t>> implemented;
    std::map<std::string, std::uint64_t> stub_calls;
};

Kernel k;

std::uint32_t arg(const AllegrexContext &ctx, unsigned index) {
    // Allegrex EABI: a0-a3 then t0-t3 carry the first eight words.
    return ctx.gpr[index < 4u ? 4u + index : 8u + (index - 4u)];
}

void ret(AllegrexContext &ctx, std::uint32_t value) { ctx.gpr[2] = value; }

void ret64(AllegrexContext &ctx, std::uint64_t value) {
    ctx.gpr[2] = static_cast<std::uint32_t>(value);
    ctx.gpr[3] = static_cast<std::uint32_t>(value >> 32u);
}

void store64(Runtime &rt, std::uint32_t address, std::uint64_t value) {
    rt.memory().store32(address, static_cast<std::uint32_t>(value));
    rt.memory().store32(address + 4u, static_cast<std::uint32_t>(value >> 32u));
}

void log_line(const std::string &line) {
    if (k.options.verbose) std::cerr << "[hle] " << line << "\n";
}

bool trace_hle_enabled() {
    static const bool enabled = std::getenv("BURNOUT_TRACE_HLE") != nullptr;
    return enabled;
}

void reg(Runtime &rt, const char *library, std::uint32_t nid, Runtime::HleFunction function) {
    k.implemented.emplace(library, nid);
    if (trace_hle_enabled()) {
        const std::string name = rt.nids().resolve(library, nid).value_or(psprecomp::hex32(nid));
        function = [name, inner = std::move(function)](Runtime &r, AllegrexContext &ctx) {
            const std::int32_t uid = k.current;
            std::cerr << "[call] vb=" << k.vblanks << " t=" << uid << " " << name << "("
                      << psprecomp::hex32(ctx.gpr[4]) << ", " << psprecomp::hex32(ctx.gpr[5]) << ", "
                      << psprecomp::hex32(ctx.gpr[6]) << ", " << psprecomp::hex32(ctx.gpr[7]) << ") ra="
                      << psprecomp::hex32(ctx.gpr[31]);
            inner(r, ctx);
            if (k.current == uid) std::cerr << " -> " << psprecomp::hex32(ctx.gpr[2]) << "\n";
            else std::cerr << " -> switch to " << k.current << "\n";
        };
    }
#if defined(BURNOUT_VITA_PROFILE)
    // Attribute samples taken inside an import to its NID, and the host work
    // after it (dispatch, scheduling) to a runtime marker, until the next block.
    function = [nid, inner = std::move(function)](Runtime &r, AllegrexContext &ctx) {
        psprecomp_profile_label = nid;
        inner(r, ctx);
        psprecomp_profile_label = 0xEEEEEEEEu;
    };
#endif
    rt.register_hle(library, nid, std::move(function));
}

void reg_value(Runtime &rt, const char *library, std::uint32_t nid, std::uint32_t value) {
    reg(rt, library, nid, [value](Runtime &, AllegrexContext &ctx) { ret(ctx, value); });
}

std::int32_t new_uid() { return k.next_uid++; }

Thread *find_thread(std::int32_t uid) {
    if (uid == 0) uid = k.current;
    const auto it = k.threads.find(uid);
    return it == k.threads.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- memory

std::optional<std::uint32_t> allocate(std::uint32_t size, std::uint32_t type, std::uint32_t hint,
                                      std::string name) {
    size = (size + 0xFFu) & ~0xFFu;
    if (size == 0u) size = 0x100u;
    struct Gap { std::uint32_t start, end; };
    std::vector<Gap> gaps;
    std::uint32_t cursor = k.arena_low;
    for (const auto &block : k.blocks) {
        if (block.address > cursor) gaps.push_back({cursor, block.address});
        cursor = std::max(cursor, block.address + block.size);
    }
    if (k.arena_high > cursor) gaps.push_back({cursor, k.arena_high});

    std::optional<std::uint32_t> address;
    if (type == 2u) {  // PSP_SMEM_Addr
        for (const auto &gap : gaps) {
            if (hint >= gap.start && hint + size <= gap.end) { address = hint; break; }
        }
    } else if (type == 1u) {  // PSP_SMEM_High
        for (auto it = gaps.rbegin(); it != gaps.rend(); ++it) {
            if (it->end - it->start >= size) { address = it->end - size; break; }
        }
    } else {
        for (const auto &gap : gaps) {
            if (gap.end - gap.start >= size) { address = gap.start; break; }
        }
    }
    if (!address) return std::nullopt;
    MemoryBlock block{static_cast<std::uint32_t>(new_uid()), *address, size, std::move(name)};
    k.blocks.insert(std::upper_bound(k.blocks.begin(), k.blocks.end(), block,
                                     [](const auto &a, const auto &b) { return a.address < b.address; }),
                    block);
    return *address;
}

std::uint32_t block_uid_at(std::uint32_t address) {
    for (const auto &block : k.blocks)
        if (block.address == address) return block.uid;
    return 0u;
}

void free_block(std::uint32_t uid) {
    k.blocks.erase(std::remove_if(k.blocks.begin(), k.blocks.end(),
                                  [uid](const auto &b) { return b.uid == uid; }),
                   k.blocks.end());
}

std::uint32_t max_free() {
    std::uint32_t best = 0u, cursor = k.arena_low;
    for (const auto &block : k.blocks) {
        if (block.address > cursor) best = std::max(best, block.address - cursor);
        cursor = std::max(cursor, block.address + block.size);
    }
    if (k.arena_high > cursor) best = std::max(best, k.arena_high - cursor);
    return best;
}

// ---------------------------------------------------------------- scheduler

void dispatch_next(Runtime &rt, AllegrexContext &ctx);

void make_ready(Thread &thread, std::uint32_t result) {
    thread.ctx.gpr[2] = result;
    thread.state = ThreadState::Ready;
    thread.wait = WaitKind::None;
    thread.deadline = 0u;
    thread.ready_seq = k.ready_seq++;
}

void suspend_current(const AllegrexContext &ctx, WaitKind wait, std::uint64_t deadline = 0u) {
    Thread *thread = find_thread(k.current);
    if (thread == nullptr) return;
    thread->ctx = ctx;
    thread->ctx.pc = ctx.gpr[31];
    thread->ctx.gpr[2] = 0u;
    thread->state = ThreadState::Waiting;
    thread->wait = wait;
    thread->deadline = deadline;
}

Thread *best_ready(std::int32_t *uid_out) {
    Thread *best = nullptr;
    for (auto &[uid, thread] : k.threads) {
        if (thread.state != ThreadState::Ready) continue;
        if (best == nullptr || thread.priority < best->priority ||
            (thread.priority == best->priority && thread.ready_seq < best->ready_seq)) {
            best = &thread;
            *uid_out = uid;
        }
    }
    return best;
}

// Interrupt handler time by handler address (thread_time_report).
struct InterruptTime {
    double ms{};
    std::uint32_t count{};
};
std::map<std::uint32_t, InterruptTime> g_interrupt_times;
std::uint32_t g_interrupt_handler{};

void start_interrupt(AllegrexContext &ctx) {
    const PendingInterrupt pending = k.pending_interrupts.front();
    g_interrupt_handler = pending.handler;
    ++g_interrupt_times[pending.handler].count;
    k.pending_interrupts.pop_front();
    k.in_interrupt = true;
    ctx.gpr[4] = pending.a0;
    ctx.gpr[5] = pending.a1;
    ctx.gpr[6] = pending.a2;
    ctx.gpr[28] = k.options.module_gp;
    ctx.gpr[29] = k.interrupt_stack_top;
    ctx.gpr[31] = kInterruptReturnPc;
    ctx.pc = pending.handler;
    psprecomp::set_runtime_thread_identity(-2, "interrupt");
}

void wake_wait_timeout(Thread &thread) {
    if (thread.timeout_ptr != 0u) thread.timeout_ptr = 0u;
    make_ready(thread, kErrorWaitTimeout);
}

void process_time(Runtime &rt) {
    while (k.next_vblank_us <= k.time_us) {
        ++k.vblanks;
        k.next_vblank_us += kVblankPeriodUs;
        for (auto &[uid, thread] : k.threads) {
            (void)uid;
            if (thread.state == ThreadState::Waiting && thread.wait == WaitKind::Vblank &&
                k.vblanks >= thread.wait_a)
                make_ready(thread, 0u);
        }
        if (const auto it = k.sub_interrupts.find(kVblankInterrupt); it != k.sub_interrupts.end()) {
            for (const auto &[sub, handler] : it->second) {
                if (handler.enabled && handler.handler != 0u && k.pending_interrupts.size() < 64u)
                    k.pending_interrupts.push_back({handler.handler, sub, handler.arg, 0u});
            }
        }
        if (k.observer.on_vblank) k.observer.on_vblank(k.vblanks);
        if (rt.stopped()) return;
    }
    for (auto &[uid, thread] : k.threads) {
        (void)uid;
        if (thread.state != ThreadState::Waiting || thread.deadline == 0u || thread.deadline > k.time_us)
            continue;
        if (thread.wait == WaitKind::Delay) {
            make_ready(thread, 0u);
        } else {
            if (thread.timeout_ptr != 0u) rt.memory().store32(thread.timeout_ptr, 0u);
            wake_wait_timeout(thread);
        }
    }
}

std::uint64_t next_event_time() {
    std::uint64_t next = k.next_vblank_us;
    for (const auto &[uid, thread] : k.threads) {
        (void)uid;
        if (thread.state == ThreadState::Waiting && thread.deadline != 0u)
            next = std::min(next, thread.deadline);
    }
    return std::max(next, k.time_us);
}

// Host time of the running guest thread, charged when it switches out.
double g_sas_ms{};  // host time in sas::mix (thread_time_report)
std::uint32_t g_sas_grains{};
double g_interrupt_ms{};  // host time in interrupt handlers (the slice uid kInterruptSlice)
constexpr std::int32_t kInterruptSlice = -2;

struct Slice {
    std::int32_t uid{-1};
    double started{};
} g_slice;

void close_slice() {
    if (g_slice.uid == kInterruptSlice) {
        const double ms = host_now_ms() - g_slice.started;
        g_interrupt_ms += ms;
        g_interrupt_times[g_interrupt_handler].ms += ms;
    }
    if (g_slice.uid < 0) return;
    const auto it = k.threads.find(g_slice.uid);
    if (it != k.threads.end()) it->second.host_ms += host_now_ms() - g_slice.started;
    g_slice.uid = -1;
}

void dispatch_next(Runtime &rt, AllegrexContext &ctx) {
    close_slice();
    for (;;) {
        if (rt.stopped()) return;
        if (!k.pending_interrupts.empty()) {
            k.interrupted_valid = false;
            k.current = -1;
            start_interrupt(ctx);
            g_slice = {kInterruptSlice, host_now_ms()};
            return;
        }
        std::int32_t uid = -1;
        if (Thread *thread = best_ready(&uid)) {
            thread->state = ThreadState::Running;
            k.current = uid;
            k.idle_vblanks = 0u;
            g_slice = {uid, host_now_ms()};
            ctx = thread->ctx;
            psprecomp::set_runtime_thread_identity(uid, thread->name);
            return;
        }
        // Nothing runnable: jump virtual time to the next event.
        const std::uint64_t before = k.vblanks;
        k.time_us = next_event_time();
        process_time(rt);
        if (k.vblanks != before && ++k.idle_vblanks > 60u * 30u) {
            rt.stop("PSP scheduler deadlock: no runnable thread for 30 s of virtual time");
            return;
        }
    }
}

// Called after a non-blocking import that may have readied a higher-priority
// thread: PSP preempts immediately.
void maybe_preempt(Runtime &rt, AllegrexContext &ctx) {
    if (k.in_interrupt) return;
    Thread *current = find_thread(k.current);
    if (current == nullptr) return;
    std::int32_t uid = -1;
    Thread *best = best_ready(&uid);
    if (best == nullptr || best->priority >= current->priority) return;
    current->ctx = ctx;
    current->ctx.pc = ctx.gpr[31];
    current->state = ThreadState::Ready;
    current->wait = WaitKind::None;
    current->ready_seq = --k.preempt_seq;  // preempted thread resumes first in its band
    dispatch_next(rt, ctx);
}

void block_and_switch(Runtime &rt, AllegrexContext &ctx, WaitKind wait, std::uint64_t deadline = 0u) {
    if (k.in_interrupt) {
        rt.stop("blocking kernel call inside an interrupt handler");
        return;
    }
    suspend_current(ctx, wait, deadline);
    dispatch_next(rt, ctx);
}

void exit_thread(Runtime &rt, AllegrexContext &ctx, std::uint32_t status, bool remove) {
    const std::int32_t uid = k.current;
    if (Thread *thread = find_thread(uid)) {
        thread->state = ThreadState::Dormant;
        thread->exit_status = status;
        for (auto &[other_uid, other] : k.threads) {
            (void)other_uid;
            if (other.state == ThreadState::Waiting && other.wait == WaitKind::ThreadEnd && other.wait_id == uid)
                make_ready(other, status);
        }
        log_line("thread exit uid=" + std::to_string(uid) + " name=" + thread->name);
        if (remove) {
            free_block(thread->stack_block);
            k.threads.erase(uid);
        }
    }
    k.current = -1;
    dispatch_next(rt, ctx);
}

std::uint64_t timeout_deadline(Runtime &rt, std::uint32_t timeout_ptr) {
    if (timeout_ptr == 0u) return 0u;
    return k.time_us + std::max<std::uint32_t>(1u, rt.memory().load32(timeout_ptr));
}

// ---------------------------------------------------------------- semaphores / event flags

void wake_semaphore_waiters(std::int32_t sema_uid) {
    auto &sema = k.semaphores[sema_uid];
    std::vector<Thread *> waiters;
    for (auto &[uid, thread] : k.threads) {
        (void)uid;
        if (thread.state == ThreadState::Waiting && thread.wait == WaitKind::Sema && thread.wait_id == sema_uid)
            waiters.push_back(&thread);
    }
    std::sort(waiters.begin(), waiters.end(), [](auto *a, auto *b) { return a->ready_seq < b->ready_seq; });
    for (Thread *thread : waiters) {
        const auto need = static_cast<std::int32_t>(thread->wait_a);
        if (sema.count < need) break;
        sema.count -= need;
        make_ready(*thread, 0u);
    }
}

bool event_flag_matches(std::uint32_t bits, std::uint32_t pattern, std::uint32_t mode) {
    return (mode & 1u) != 0u ? (bits & pattern) != 0u : (bits & pattern) == pattern;
}

void apply_event_flag_clear(EventFlag &flag, std::uint32_t pattern, std::uint32_t mode) {
    if ((mode & 0x10u) != 0u) flag.bits = 0u;            // PSP_EVENT_WAITCLEARALL
    else if ((mode & 0x20u) != 0u) flag.bits &= ~pattern;  // PSP_EVENT_WAITCLEAR
}

void wake_event_flag_waiters(Runtime &rt, std::int32_t flag_uid) {
    auto &flag = k.event_flags[flag_uid];
    for (auto &[uid, thread] : k.threads) {
        (void)uid;
        if (thread.state != ThreadState::Waiting || thread.wait != WaitKind::EventFlag || thread.wait_id != flag_uid)
            continue;
        if (!event_flag_matches(flag.bits, thread.wait_a, thread.wait_b)) continue;
        if (thread.wait_out != 0u) rt.memory().store32(thread.wait_out, flag.bits);
        apply_event_flag_clear(flag, thread.wait_a, thread.wait_b);
        make_ready(thread, 0u);
    }
}

// ---------------------------------------------------------------- files

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const fs::path kVirtualRoot{"<umd>"};
const fs::path kVirtualPspGame{"<umd>/PSP_GAME"};
const fs::path kVirtualSysdir{"<umd>/PSP_GAME/SYSDIR"};

std::optional<fs::path> map_path(const std::string &psp_path) {
    std::string device, rest = psp_path;
    if (const auto colon = psp_path.find(':'); colon != std::string::npos) {
        device = lower(psp_path.substr(0, colon));
        rest = psp_path.substr(colon + 1u);
    }
    std::replace(rest.begin(), rest.end(), '\\', '/');
    while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
    fs::path clean;
    for (const auto &part : fs::path(rest)) {
        if (part == "..") return std::nullopt;
        if (part != "." && !part.empty()) clean /= part;
    }
    if (device == "ms0" || device == "fatms0") return k.options.memstick / clean;
    if (device.empty()) return k.options.usrdir / clean;  // relative path

    // UMD layout: the root holds PSP_GAME/{PARAM.SFO,SYSDIR,USRDIR}. Only USRDIR
    // and the loose disc files exist on the host; the directories above are
    // synthesized so the game's whole-disc file table sees real UMD paths.
    std::vector<fs::path> parts(clean.begin(), clean.end());
    const auto part_is = [&](std::size_t index, const char *name) {
        return parts.size() > index && lower(parts[index].string()) == name;
    };
    const auto tail = [&](std::size_t from) {
        fs::path out;
        for (std::size_t i = from; i < parts.size(); ++i) out /= parts[i];
        return out;
    };
    if (parts.empty()) return kVirtualRoot;
    if (!part_is(0, "psp_game")) return k.options.disc_root / clean;
    if (parts.size() == 1u) return kVirtualPspGame;
    if (part_is(1, "usrdir")) return k.options.usrdir / tail(2);
    if (part_is(1, "sysdir")) return parts.size() == 2u ? kVirtualSysdir : k.options.disc_root / tail(2);
    return k.options.disc_root / tail(1);
}

bool is_virtual_directory(const fs::path &path) {
    return path == kVirtualRoot || path == kVirtualPspGame || path == kVirtualSysdir;
}

std::vector<DirectoryItem> virtual_directory_items(const fs::path &path) {
    std::vector<DirectoryItem> items;
    std::error_code ec;
    if (path == kVirtualRoot) {
        items.push_back({"PSP_GAME", kVirtualPspGame, true, 2048u});
    } else if (path == kVirtualPspGame) {
        const fs::path sfo = k.options.disc_root / "PARAM.SFO";
        if (fs::exists(sfo, ec)) items.push_back({"PARAM.SFO", sfo, false, fs::file_size(sfo, ec)});
        items.push_back({"SYSDIR", kVirtualSysdir, true, 2048u});
        items.push_back({"USRDIR", k.options.usrdir, true, 2048u});
    }
    return items;
}

void write_datetime(Runtime &rt, std::uint32_t address) {
    rt.memory().store16(address + 0u, 2005u);
    rt.memory().store16(address + 2u, 8u);
    rt.memory().store16(address + 4u, 3u);
    rt.memory().store16(address + 6u, 12u);
    rt.memory().store16(address + 8u, 0u);
    rt.memory().store16(address + 10u, 0u);
    rt.memory().store32(address + 12u, 0u);
}

void write_stat(Runtime &rt, std::uint32_t address, bool directory, std::uint64_t size) {
    rt.memory().zero(address, 88u);
    rt.memory().store32(address + 0u, directory ? 0x11FFu : 0x21FFu);
    rt.memory().store32(address + 4u, directory ? 0x10u : 0x20u);
    store64(rt, address + 8u, size);
    write_datetime(rt, address + 16u);
    write_datetime(rt, address + 32u);
    write_datetime(rt, address + 48u);
}

std::uint32_t lba_for(const fs::path &host, std::uint64_t size) {
    const std::string key = lower(host.lexically_normal().generic_string());
    if (const auto it = k.lba_by_path.find(key); it != k.lba_by_path.end()) return it->second;
    const std::uint32_t lba = k.next_lba;
    k.next_lba += static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, (size + 2047u) / 2048u));
    k.lba_by_path.emplace(key, lba);
    k.path_by_lba.emplace(lba, host);
    return lba;
}

// "disc0:/sce_lbn0x<sector>_size0x<bytes>" opens raw sectors. Returns the file
// containing that sector and the byte offset inside it.
std::optional<std::pair<fs::path, std::uint64_t>> map_lbn_path(const std::string &psp_path) {
    const std::string lowered = lower(psp_path);
    const auto at = lowered.find("sce_lbn");
    if (at == std::string::npos) return std::nullopt;
    // Both numbers are hexadecimal, with or without a 0x prefix.
    const char *digits = lowered.c_str() + at + 7u;
    if (digits[0] == '0' && digits[1] == 'x') digits += 2;
    const auto sector = static_cast<std::uint32_t>(std::strtoul(digits, nullptr, 16));
    auto it = k.path_by_lba.upper_bound(sector);
    if (it == k.path_by_lba.begin()) return std::nullopt;
    --it;
    return std::make_pair(it->second, static_cast<std::uint64_t>(sector - it->first) * 2048u);
}

std::int64_t file_read(Runtime &rt, std::int32_t fd, std::uint32_t buffer, std::uint32_t size) {
    const auto it = k.files.find(fd);
    if (it == k.files.end() || it->second.file == nullptr) return static_cast<std::int32_t>(kErrorBadFile);
    std::uint8_t *host = rt.memory().raw_pointer(buffer, size);
    if (host == nullptr && size != 0u) return static_cast<std::int32_t>(kErrorBadFile);
    const double started = host_now_ms();
    const auto read = static_cast<std::int64_t>(std::fread(host, 1u, size, it->second.file));
    const double took = host_now_ms() - started;
    gl::io_read_ms += took;
    gl::io_read_max_ms = std::max(gl::io_read_max_ms, took);
    return read;
}

// ---------------------------------------------------------------- GE

void ge_queue_callback(std::int32_t callback_id, bool finish, std::uint32_t value) {
    const auto it = k.ge_callbacks.find(callback_id);
    if (it == k.ge_callbacks.end()) return;
    const std::uint32_t func = finish ? it->second.finish_func : it->second.signal_func;
    const std::uint32_t data = finish ? it->second.finish_arg : it->second.signal_arg;
    if (func != 0u) k.pending_interrupts.push_back({func, value & 0xFFFFu, data, 0u});
}

// The opcodes ge_scan acts on: JUMP, CALL, RET, END, SIGNAL, FINISH, BASE,
// OFFSET_ADDR and ORIGIN_ADDR.
constexpr bool ge_is_control(std::uint8_t op) noexcept {
    return op == 0x08u || op == 0x0Au || op == 0x0Bu || op == 0x0Cu || op == 0x0Eu || op == 0x0Fu || op == 0x10u ||
           op == 0x13u || op == 0x14u;
}

// Walks a display list only far enough to honour SIGNAL/FINISH callbacks; the
// commands themselves are discarded (no renderer in the headless build).
void ge_scan(Runtime &rt, GeList &list) {
#if defined(BURNOUT_GE_RENDER)
    struct ScanTimer {
        double started = host_now_ms();
        ScanTimer() { gl::emulation_phase = 1u; }
        ~ScanTimer() {
            gl::emulation_phase = 0u;
            render::account_scan(host_now_ms() - started);
        }
    } timer;
#endif
    constexpr std::uint64_t kCommandLimit = 4u * 1024u * 1024u;
    // Display lists are long and GuestMemory::load32 reads byte by byte.
    const auto memory = rt.memory().aot_fast_view();
    // Without workload statistics only the control commands below matter, so
    // runs of other commands (nearly all of a list) are skipped in place.
#if defined(BURNOUT_GE_RENDER)
    // With the renderer, such runs go to it in one call instead.
    const bool render_runs = !workload::collecting() && render::active();
    const bool skip_plain = !workload::collecting();
#else
    const bool skip_plain = !workload::collecting();
#endif
    for (std::uint64_t count = 0; count < kCommandLimit && !list.done; ++count) {
        const std::uint32_t pc = list.pc & 0x0FFFFFFFu;
        if (list.stall != 0u && pc == (list.stall & 0x0FFFFFFFu)) return;
        if (skip_plain) {
            std::uint64_t span = std::min<std::uint64_t>(4096u, kCommandLimit - count);
            const std::uint32_t stall = list.stall & 0x0FFFFFFFu;
            if (list.stall != 0u && stall > pc) span = std::min<std::uint64_t>(span, (stall - pc) / 4u);
            if (const std::uint8_t *words = rt.memory().raw_pointer(pc, static_cast<std::size_t>(span) * 4u)) {
                std::uint32_t plain = 0u;
#if defined(BURNOUT_GE_RENDER)
                if (render_runs) {
                    // One pass finds the run's end and hands it to the renderer.
                    plain = render::ge_plain_commands(rt.memory(), words, static_cast<std::uint32_t>(span), list.base,
                                                      list.offset);
                } else
#endif
                    while (plain < span && !ge_is_control(words[plain * 4u + 3u])) ++plain;
                if (plain != 0u) {
                    list.pc = pc + plain * 4u;
                    k.ge_commands_scanned += plain;
                    count += plain - 1u;
                    continue;
                }
            }
        }
        if (!rt.memory().contains(pc, 4u)) { list.done = true; return; }
        const std::uint32_t word = memory.aot_load32(pc);
        const std::uint32_t op = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        list.pc = pc + 4u;
        ++k.ge_commands_scanned;
        workload::ge_command(rt.memory(), pc, word, list.base, list.offset);
#if defined(BURNOUT_GE_RENDER)
        render::ge_command(rt.memory(), pc, word, list.base, list.offset);
#endif
        const auto target = [&]() { return ((list.base | data) + list.offset) & 0x0FFFFFFCu; };
        switch (op) {
        case 0x08u: list.pc = target(); break;                         // JUMP
        case 0x0Au: list.call_stack.push_back(list.pc); list.pc = target(); break;  // CALL
        case 0x0Bu:                                                     // RET
            if (!list.call_stack.empty()) { list.pc = list.call_stack.back(); list.call_stack.pop_back(); }
            break;
        case 0x0Cu: list.done = true; break;                           // END
        case 0x0Eu: ge_queue_callback(list.callback, false, data); break;  // SIGNAL
        case 0x0Fu: ge_queue_callback(list.callback, true, data); break;   // FINISH
        case 0x10u: list.base = (data << 8u) & 0x0F000000u; break;     // BASE
        case 0x13u: list.offset = data << 8u; break;                   // OFFSET_ADDR
        case 0x14u: list.offset = pc; break;                           // ORIGIN_ADDR
        default: break;
        }
    }
    list.done = true;
}

std::uint32_t ge_enqueue(Runtime &rt, const AllegrexContext &ctx) {
    const std::int32_t id = k.next_ge_list++;
    GeList list;
    list.pc = arg(ctx, 0);
    list.stall = arg(ctx, 1);
    list.callback = static_cast<std::int32_t>(arg(ctx, 2));
    ++k.ge_lists_submitted;
    workload::ge_list_submitted();
    ge_scan(rt, list);
    if (!list.done) k.ge_lists[id] = std::move(list);
    return static_cast<std::uint32_t>(id);
}

// ---------------------------------------------------------------- utility dialogs

void utility_init(UtilityDialog &dialog, std::uint32_t params, std::uint32_t result) {
    dialog.status = 1u;
    dialog.params = params;
    dialog.result = result;
}

std::uint32_t utility_status(Runtime &rt, UtilityDialog &dialog) {
    const std::uint32_t status = dialog.status;
    if (dialog.status == 1u) {
        dialog.status = 2u;
    } else if (dialog.status == 2u) {
        if (dialog.params != 0u) rt.memory().store32(dialog.params + 28u, dialog.result);
        dialog.status = 3u;
    } else if (dialog.status == 4u) {
        dialog.status = 0u;
    }
    return status;
}

// ---------------------------------------------------------------- registration

void install_threadman(Runtime &rt) {
    constexpr const char *lib = "ThreadManForUser";
    reg(rt, lib, 0x446D8DE6u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelCreateThread
        Thread thread;
        thread.name = arg(ctx, 0) ? r.memory().read_c_string(arg(ctx, 0), 32u) : "thread";
        thread.entry = arg(ctx, 1);
        thread.priority = arg(ctx, 2);
        thread.stack_size = std::max<std::uint32_t>((arg(ctx, 3) + 0xFFu) & ~0xFFu, 0x1000u);
        thread.attr = arg(ctx, 4);
        thread.gp = ctx.gpr[28];
        const auto stack = allocate(thread.stack_size, 1u, 0u, "stack:" + thread.name);
        if (!stack) { ret(ctx, kErrorNoMemory); return; }
        thread.stack_bottom = *stack;
        thread.stack_block = block_uid_at(*stack);
        const std::int32_t uid = new_uid();
        log_line("create thread uid=" + std::to_string(uid) + " name=" + thread.name + " entry=" +
                 psprecomp::hex32(thread.entry) + " prio=" + std::to_string(thread.priority) +
                 " stack=" + psprecomp::hex32(thread.stack_size));
        k.threads.emplace(uid, std::move(thread));
        ret(ctx, static_cast<std::uint32_t>(uid));
    });
    reg(rt, lib, 0xF475845Du, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelStartThread
        Thread *thread = find_thread(static_cast<std::int32_t>(arg(ctx, 0)));
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        if (thread->state != ThreadState::Dormant) { ret(ctx, kErrorNotDormant); return; }
        const std::uint32_t top = thread->stack_bottom + thread->stack_size;
        r.memory().zero(thread->stack_bottom, thread->stack_size);
        const std::uint32_t kernel_context = top - 0x100u;
        r.memory().store32(kernel_context + 0xC8u, thread->stack_bottom);
        r.memory().store32(kernel_context + 0xF8u, 0xFFFFFFFFu);
        r.memory().store32(kernel_context + 0xFCu, 0xFFFFFFFFu);
        std::uint32_t sp = kernel_context;
        const std::uint32_t arg_length = arg(ctx, 1);
        const std::uint32_t arg_pointer = arg(ctx, 2);
        std::uint32_t copied = 0u;
        if (arg_pointer != 0u && arg_length != 0u) {
            sp = (sp - arg_length) & ~0xFu;
            std::vector<std::uint8_t> bytes(arg_length);
            r.memory().copy_out(arg_pointer, bytes);
            r.memory().copy_in(sp, bytes);
            copied = sp;
        }
        thread->ctx = ctx;
        thread->ctx.gpr.fill(0u);
        thread->ctx.gpr[4] = arg_length;
        thread->ctx.gpr[5] = copied;
        thread->ctx.gpr[26] = kernel_context;
        thread->ctx.gpr[28] = thread->gp;
        thread->ctx.gpr[29] = sp - 0x40u;
        thread->ctx.gpr[31] = kThreadReturnPc;
        thread->ctx.pc = thread->entry;
        make_ready(*thread, 0u);
        thread->ctx.gpr[2] = 0u;
        ret(ctx, 0u);
        maybe_preempt(r, ctx);
    });
    reg(rt, lib, 0xAA73C935u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelExitThread
        exit_thread(r, ctx, arg(ctx, 0), false);
    });
    reg(rt, lib, 0x809CE29Bu, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelExitDeleteThread
        exit_thread(r, ctx, arg(ctx, 0), true);
    });
    reg(rt, lib, 0x9FA03CD3u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelDeleteThread
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        Thread *thread = find_thread(uid);
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        if (thread->state != ThreadState::Dormant) { ret(ctx, kErrorNotDormant); return; }
        free_block(thread->stack_block);
        k.threads.erase(uid);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x293B45B8u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelGetThreadId
        ret(ctx, static_cast<std::uint32_t>(k.current));
    });
    reg(rt, lib, 0xEA748E31u, [](Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); });  // ChangeCurrentThreadAttr
    reg(rt, lib, 0x71BC9871u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelChangeThreadPriority
        Thread *thread = find_thread(static_cast<std::int32_t>(arg(ctx, 0)));
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        if (arg(ctx, 1) != 0u) thread->priority = arg(ctx, 1);
        ret(ctx, 0u);
        maybe_preempt(r, ctx);
    });
    reg(rt, lib, 0x278C0DF5u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelWaitThreadEnd
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        Thread *thread = find_thread(uid);
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        if (thread->state == ThreadState::Dormant) { ret(ctx, thread->exit_status); return; }
        const std::uint32_t timeout = arg(ctx, 1);
        const std::uint64_t deadline = timeout_deadline(r, timeout);
        Thread *self = find_thread(k.current);
        if (self) { self->wait_id = uid; self->timeout_ptr = timeout; }
        block_and_switch(r, ctx, WaitKind::ThreadEnd, deadline);
    });
    reg(rt, lib, 0x3B183E26u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelGetThreadExitStatus
        Thread *thread = find_thread(static_cast<std::int32_t>(arg(ctx, 0)));
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        ret(ctx, thread->state == ThreadState::Dormant ? thread->exit_status : kErrorNotDormant);
    });
    const auto delay = [](Runtime &r, AllegrexContext &ctx) {  // sceKernelDelayThread(CB)
        const std::uint64_t us = std::max<std::uint32_t>(arg(ctx, 0), 1u);
        block_and_switch(r, ctx, WaitKind::Delay, k.time_us + us);
    };
    reg(rt, lib, 0xCEADEB47u, delay);
    reg(rt, lib, 0x82826F70u, delay);
    reg(rt, lib, 0xD59EAD2Fu, [](Runtime &, AllegrexContext &ctx) {  // sceKernelWakeupThread
        Thread *thread = find_thread(static_cast<std::int32_t>(arg(ctx, 0)));
        if (thread == nullptr) { ret(ctx, kErrorUnknownThread); return; }
        ++thread->wakeup_count;
        ret(ctx, 0u);
    });

    reg(rt, lib, 0xD6DA4BA1u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelCreateSema
        const std::int32_t uid = new_uid();
        Semaphore sema;
        sema.name = arg(ctx, 0) ? r.memory().read_c_string(arg(ctx, 0), 32u) : "sema";
        sema.count = static_cast<std::int32_t>(arg(ctx, 2));
        sema.max = static_cast<std::int32_t>(arg(ctx, 3));
        k.semaphores.emplace(uid, std::move(sema));
        ret(ctx, static_cast<std::uint32_t>(uid));
    });
    reg(rt, lib, 0x28B6489Cu, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelDeleteSema
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        if (k.semaphores.erase(uid) == 0u) { ret(ctx, kErrorUnknownSema); return; }
        for (auto &[tid, thread] : k.threads) {
            (void)tid;
            if (thread.state == ThreadState::Waiting && thread.wait == WaitKind::Sema && thread.wait_id == uid)
                make_ready(thread, 0x800201B5u);  // SCE_KERNEL_ERROR_WAIT_DELETE
        }
        ret(ctx, 0u);
        maybe_preempt(r, ctx);
    });
    reg(rt, lib, 0x3F53E640u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelSignalSema
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        const auto it = k.semaphores.find(uid);
        if (it == k.semaphores.end()) { ret(ctx, kErrorUnknownSema); return; }
        const auto signal = static_cast<std::int32_t>(arg(ctx, 1));
        if (it->second.max > 0 && it->second.count + signal > it->second.max) { ret(ctx, kErrorSemaOverflow); return; }
        it->second.count += signal;
        wake_semaphore_waiters(uid);
        ret(ctx, 0u);
        maybe_preempt(r, ctx);
    });
    reg(rt, lib, 0x4E3A1105u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelWaitSema
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        const auto it = k.semaphores.find(uid);
        if (it == k.semaphores.end()) { ret(ctx, kErrorUnknownSema); return; }
        const auto need = static_cast<std::int32_t>(arg(ctx, 1));
        if (it->second.count >= need) { it->second.count -= need; ret(ctx, 0u); return; }
        Thread *self = find_thread(k.current);
        if (self) { self->wait_id = uid; self->wait_a = arg(ctx, 1); self->timeout_ptr = arg(ctx, 2); }
        block_and_switch(r, ctx, WaitKind::Sema, timeout_deadline(r, arg(ctx, 2)));
    });
    reg(rt, lib, 0x58B1F937u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelPollSema
        const auto it = k.semaphores.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.semaphores.end()) { ret(ctx, kErrorUnknownSema); return; }
        const auto need = static_cast<std::int32_t>(arg(ctx, 1));
        if (it->second.count < need) { ret(ctx, kErrorSemaZero); return; }
        it->second.count -= need;
        ret(ctx, 0u);
    });

    reg(rt, lib, 0x55C20A00u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelCreateEventFlag
        const std::int32_t uid = new_uid();
        EventFlag flag;
        flag.name = arg(ctx, 0) ? r.memory().read_c_string(arg(ctx, 0), 32u) : "evf";
        flag.attr = arg(ctx, 1);
        flag.bits = arg(ctx, 2);
        k.event_flags.emplace(uid, std::move(flag));
        ret(ctx, static_cast<std::uint32_t>(uid));
    });
    reg(rt, lib, 0xEF9E4C70u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelDeleteEventFlag
        ret(ctx, k.event_flags.erase(static_cast<std::int32_t>(arg(ctx, 0))) ? 0u : kErrorUnknownEventFlag);
    });
    reg(rt, lib, 0x1FB15A32u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelSetEventFlag
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        const auto it = k.event_flags.find(uid);
        if (it == k.event_flags.end()) { ret(ctx, kErrorUnknownEventFlag); return; }
        it->second.bits |= arg(ctx, 1);
        wake_event_flag_waiters(r, uid);
        ret(ctx, 0u);
        maybe_preempt(r, ctx);
    });
    reg(rt, lib, 0x812346E4u, [](Runtime &, AllegrexContext &ctx) {  // sceKernelClearEventFlag
        const auto it = k.event_flags.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.event_flags.end()) { ret(ctx, kErrorUnknownEventFlag); return; }
        it->second.bits &= arg(ctx, 1);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x402FCF22u, [](Runtime &r, AllegrexContext &ctx) {  // sceKernelWaitEventFlag
        const auto uid = static_cast<std::int32_t>(arg(ctx, 0));
        const auto it = k.event_flags.find(uid);
        if (it == k.event_flags.end()) { ret(ctx, kErrorUnknownEventFlag); return; }
        const std::uint32_t pattern = arg(ctx, 1), mode = arg(ctx, 2), out = arg(ctx, 3);
        if (event_flag_matches(it->second.bits, pattern, mode)) {
            if (out != 0u) r.memory().store32(out, it->second.bits);
            apply_event_flag_clear(it->second, pattern, mode);
            ret(ctx, 0u);
            return;
        }
        Thread *self = find_thread(k.current);
        if (self) {
            self->wait_id = uid; self->wait_a = pattern; self->wait_b = mode;
            self->wait_out = out; self->timeout_ptr = arg(ctx, 4);
        }
        block_and_switch(r, ctx, WaitKind::EventFlag, timeout_deadline(r, arg(ctx, 4)));
    });

    reg(rt, lib, 0xE81CAF8Fu, [](Runtime &, AllegrexContext &ctx) {  // sceKernelCreateCallback
        const std::int32_t uid = new_uid();
        k.callbacks.insert(uid);
        ret(ctx, static_cast<std::uint32_t>(uid));
    });
    reg_value(rt, lib, 0xEDBA5844u, 0u);  // sceKernelDeleteCallback
    reg_value(rt, lib, 0x349D6D6Cu, 0u);  // sceKernelCheckCallback
    reg(rt, lib, 0x82BC5777u, [](Runtime &, AllegrexContext &ctx) { ret64(ctx, k.time_us); });
    reg(rt, lib, 0x369ED59Du, [](Runtime &, AllegrexContext &ctx) {
        ret(ctx, static_cast<std::uint32_t>(k.time_us));
    });
}

void install_kernel_misc(Runtime &rt) {
    reg(rt, "SysMemUserForUser", 0x237DBD4Fu, [](Runtime &r, AllegrexContext &ctx) {  // AllocPartitionMemory
        const std::string name = arg(ctx, 1) ? r.memory().read_c_string(arg(ctx, 1), 32u) : "block";
        const auto address = allocate(arg(ctx, 3), arg(ctx, 2), arg(ctx, 4), name);
        if (!address) { ret(ctx, kErrorNoMemory); return; }
        log_line("alloc " + name + " size=" + psprecomp::hex32(arg(ctx, 3)) + " at " + psprecomp::hex32(*address));
        ret(ctx, block_uid_at(*address));
    });
    reg(rt, "SysMemUserForUser", 0xB6D61D02u, [](Runtime &, AllegrexContext &ctx) {
        free_block(arg(ctx, 0));
        ret(ctx, 0u);
    });
    reg(rt, "SysMemUserForUser", 0x9D9A5BA1u, [](Runtime &, AllegrexContext &ctx) {  // GetBlockHeadAddr
        for (const auto &block : k.blocks)
            if (block.uid == arg(ctx, 0)) { ret(ctx, block.address); return; }
        ret(ctx, kErrorUnknownUid);
    });
    reg(rt, "SysMemUserForUser", 0xA291F107u, [](Runtime &, AllegrexContext &ctx) { ret(ctx, max_free()); });

    reg(rt, "UtilsForUser", 0x91E4F6A7u, [](Runtime &, AllegrexContext &ctx) {  // LibcClock
        ret(ctx, static_cast<std::uint32_t>(k.time_us));
    });
    reg(rt, "UtilsForUser", 0x27CC57F0u, [](Runtime &r, AllegrexContext &ctx) {  // LibcTime
        const auto seconds = static_cast<std::uint32_t>(1136073600u + k.time_us / 1000000u);  // 2006-01-01
        if (arg(ctx, 0) != 0u) r.memory().store32(arg(ctx, 0), seconds);
        ret(ctx, seconds);
    });
    reg(rt, "UtilsForUser", 0x71EC4271u, [](Runtime &r, AllegrexContext &ctx) {  // LibcGettimeofday
        if (arg(ctx, 0) != 0u) {
            r.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(1136073600u + k.time_us / 1000000u));
            r.memory().store32(arg(ctx, 0) + 4u, static_cast<std::uint32_t>(k.time_us % 1000000u));
        }
        ret(ctx, 0u);
    });
    reg_value(rt, "UtilsForUser", 0x6AD345D7u, 0u);  // SetGPO

    reg_value(rt, "sceSuspendForUser", 0x090CCB3Fu, 0u);  // PowerTick
    reg(rt, "sceSuspendForUser", 0x3E0271D3u, [](Runtime &r, AllegrexContext &ctx) {  // VolatileMemLock
        if (arg(ctx, 1) != 0u) r.memory().store32(arg(ctx, 1), 0x08400000u);
        if (arg(ctx, 2) != 0u) r.memory().store32(arg(ctx, 2), 0x00400000u);
        ret(ctx, 0u);
    });
    reg_value(rt, "sceSuspendForUser", 0xA569E425u, 0u);

    reg_value(rt, "LoadExecForUser", 0x4AC57943u, 0u);  // RegisterExitCallback
    reg(rt, "LoadExecForUser", 0x05572A5Fu, [](Runtime &r, AllegrexContext &) { r.stop("sceKernelExitGame"); });

    std::int32_t *next_module = new std::int32_t(0x500);
    const auto load_module = [next_module](Runtime &r, AllegrexContext &ctx) {
        log_line("load module " + (arg(ctx, 0) ? r.memory().read_c_string(arg(ctx, 0), 128u) : std::string("<buffer>")));
        ret(ctx, static_cast<std::uint32_t>((*next_module)++));
    };
    reg(rt, "ModuleMgrForUser", 0x977DE386u, load_module);
    reg(rt, "ModuleMgrForUser", 0xF9275D98u, load_module);
    reg(rt, "ModuleMgrForUser", 0x50F0C1ECu, [](Runtime &r, AllegrexContext &ctx) {  // StartModule
        if (arg(ctx, 3) != 0u) r.memory().store32(arg(ctx, 3), 0u);
        ret(ctx, arg(ctx, 0));
    });
    reg(rt, "ModuleMgrForUser", 0xD675EBB8u, [](Runtime &r, AllegrexContext &) { r.stop("sceKernelSelfStopUnloadModule"); });

    reg_value(rt, "StdioForUser", 0x172D316Eu, 0u);
    reg_value(rt, "StdioForUser", 0xA6BAB2E9u, 1u);
    reg_value(rt, "StdioForUser", 0xF78BA90Au, 2u);

    reg(rt, "InterruptManager", 0xCA04A2B9u, [](Runtime &, AllegrexContext &ctx) {  // RegisterSubIntrHandler
        auto &handler = k.sub_interrupts[arg(ctx, 0)][arg(ctx, 1)];
        handler.handler = arg(ctx, 2);
        handler.arg = arg(ctx, 3);
        log_line("sub interrupt " + std::to_string(arg(ctx, 0)) + "/" + std::to_string(arg(ctx, 1)) +
                 " handler=" + psprecomp::hex32(arg(ctx, 2)));
        ret(ctx, 0u);
    });
    reg(rt, "InterruptManager", 0xD61E6961u, [](Runtime &, AllegrexContext &ctx) {
        k.sub_interrupts[arg(ctx, 0)].erase(arg(ctx, 1));
        ret(ctx, 0u);
    });
    reg(rt, "InterruptManager", 0xFB8E22ECu, [](Runtime &, AllegrexContext &ctx) {
        k.sub_interrupts[arg(ctx, 0)][arg(ctx, 1)].enabled = true;
        ret(ctx, 0u);
    });
}

void install_io(Runtime &rt) {
    constexpr const char *lib = "IoFileMgrForUser";
    reg(rt, lib, 0x109F50BCu, [](Runtime &r, AllegrexContext &ctx) {  // sceIoOpen
        const std::string psp_path = r.memory().read_c_string(arg(ctx, 0), 256u);
        const std::uint32_t flags = arg(ctx, 1);
        const auto lbn = map_lbn_path(psp_path);
        const auto host = lbn ? std::optional<fs::path>(lbn->first) : map_path(psp_path);
        if (k.observer.on_file_open) k.observer.on_file_open(lbn ? host->generic_string() : psp_path);
        if (!host) { ret(ctx, kErrorFileNotFound); return; }
        const bool write = (flags & 2u) != 0u;
        std::FILE *file = nullptr;
        if (!write) {
            file = std::fopen(host->string().c_str(), "rb");
            if (file != nullptr && lbn) std::fseek(file, static_cast<long>(lbn->second), SEEK_SET);
        } else {
            std::error_code ec;
            fs::create_directories(host->parent_path(), ec);
            const bool exists = fs::exists(*host, ec);
            const char *mode = (flags & 0x400u) != 0u || !exists ? ((flags & 0x200u) != 0u || exists ? "w+b" : nullptr)
                                                                 : "r+b";
            if (mode != nullptr) file = std::fopen(host->string().c_str(), mode);
            if (file != nullptr && (flags & 0x100u) != 0u) std::fseek(file, 0, SEEK_END);
        }
        log_line("open " + psp_path + (file ? "" : " -> not found"));
        if (file == nullptr) { ret(ctx, kErrorFileNotFound); return; }
        const std::int32_t fd = k.next_fd++;
        k.files[fd] = OpenFile{file, psp_path, 0};
        ret(ctx, static_cast<std::uint32_t>(fd));
    });
    reg(rt, lib, 0x810C4BC3u, [](Runtime &, AllegrexContext &ctx) {  // sceIoClose
        const auto it = k.files.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.files.end()) { ret(ctx, kErrorBadFile); return; }
        std::fclose(it->second.file);
        k.files.erase(it);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x6A638D83u, [](Runtime &r, AllegrexContext &ctx) {  // sceIoRead
        ret(ctx, static_cast<std::uint32_t>(file_read(r, static_cast<std::int32_t>(arg(ctx, 0)), arg(ctx, 1), arg(ctx, 2))));
    });
    reg(rt, lib, 0xA0B5A7C2u, [](Runtime &r, AllegrexContext &ctx) {  // sceIoReadAsync
        const auto fd = static_cast<std::int32_t>(arg(ctx, 0));
        const std::int64_t result = file_read(r, fd, arg(ctx, 1), arg(ctx, 2));
        if (const auto it = k.files.find(fd); it != k.files.end()) it->second.async_result = result;
        ret(ctx, result < 0 ? static_cast<std::uint32_t>(result) : 0u);
    });
    const auto async_result = [](Runtime &r, AllegrexContext &ctx) {  // PollAsync / WaitAsyncCB
        const auto it = k.files.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.files.end()) { ret(ctx, kErrorBadFile); return; }
        if (arg(ctx, 1) != 0u) store64(r, arg(ctx, 1), static_cast<std::uint64_t>(it->second.async_result));
        ret(ctx, 0u);
    };
    reg(rt, lib, 0x3251EA56u, async_result);
    reg(rt, lib, 0x35DBD746u, async_result);
    reg(rt, lib, 0x42EC03ACu, [](Runtime &r, AllegrexContext &ctx) {  // sceIoWrite
        const auto fd = static_cast<std::int32_t>(arg(ctx, 0));
        const std::uint32_t size = arg(ctx, 2);
        const std::uint8_t *data = r.memory().raw_pointer(arg(ctx, 1), size);
        if (data == nullptr) { ret(ctx, kErrorBadFile); return; }
        if (fd == 1 || fd == 2) {
            if (k.options.verbose) std::cerr.write(reinterpret_cast<const char *>(data), size);
            ret(ctx, size);
            return;
        }
        const auto it = k.files.find(fd);
        if (it == k.files.end()) { ret(ctx, kErrorBadFile); return; }
        ret(ctx, static_cast<std::uint32_t>(std::fwrite(data, 1u, size, it->second.file)));
    });
    reg(rt, lib, 0x27EB27B8u, [](Runtime &, AllegrexContext &ctx) {  // sceIoLseek
        const auto it = k.files.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.files.end()) { ret64(ctx, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(kErrorBadFile)))); return; }
        const auto offset = static_cast<std::int64_t>(static_cast<std::uint64_t>(arg(ctx, 2)) |
                                                      (static_cast<std::uint64_t>(arg(ctx, 3)) << 32u));
        const int whence = arg(ctx, 4) == 1u ? SEEK_CUR : arg(ctx, 4) == 2u ? SEEK_END : SEEK_SET;
#if defined(_WIN32)
        _fseeki64(it->second.file, offset, whence);
        ret64(ctx, static_cast<std::uint64_t>(_ftelli64(it->second.file)));
#else
        std::fseek(it->second.file, static_cast<long>(offset), whence);
        ret64(ctx, static_cast<std::uint64_t>(std::ftell(it->second.file)));
#endif
    });
    reg(rt, lib, 0xACE946E8u, [](Runtime &r, AllegrexContext &ctx) {  // sceIoGetstat
        const auto host = map_path(r.memory().read_c_string(arg(ctx, 0), 256u));
        std::error_code ec;
        if (host && is_virtual_directory(*host)) { write_stat(r, arg(ctx, 1), true, 2048u); ret(ctx, 0u); return; }
        if (!host || !fs::exists(*host, ec)) { ret(ctx, kErrorFileNotFound); return; }
        const bool directory = fs::is_directory(*host, ec);
        write_stat(r, arg(ctx, 1), directory, directory ? 0u : fs::file_size(*host, ec));
        ret(ctx, 0u);
    });
    reg(rt, lib, 0xB29DDF9Cu, [](Runtime &r, AllegrexContext &ctx) {  // sceIoDopen
        const std::string psp_path = r.memory().read_c_string(arg(ctx, 0), 256u);
        const auto host = map_path(psp_path);
        std::error_code ec;
        log_line("dopen " + psp_path);
        if (!host || (!is_virtual_directory(*host) && !fs::is_directory(*host, ec))) {
            ret(ctx, kErrorFileNotFound);
            return;
        }
        // Match a mastered UMD: case-insensitive order. The game recurses into
        // every directory entry, so "." and ".." are not reported.
        OpenDirectory directory;
        std::vector<DirectoryItem> items = virtual_directory_items(*host);
        if (!is_virtual_directory(*host)) for (const auto &entry : fs::directory_iterator(*host, ec)) {
            const bool is_directory = entry.is_directory(ec);
            items.push_back({entry.path().filename().string(), entry.path(), is_directory,
                             is_directory ? 2048u : entry.file_size(ec)});
        }
        std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return lower(a.name) < lower(b.name); });
        directory.entries.insert(directory.entries.end(), items.begin(), items.end());
        const std::int32_t fd = k.next_fd++;
        k.directories[fd] = std::move(directory);
        ret(ctx, static_cast<std::uint32_t>(fd));
    });
    reg(rt, lib, 0xE3EB004Cu, [](Runtime &r, AllegrexContext &ctx) {  // sceIoDread
        const auto it = k.directories.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it == k.directories.end()) { ret(ctx, kErrorBadFile); return; }
        if (it->second.next >= it->second.entries.size()) { ret(ctx, 0u); return; }
        const auto &entry = it->second.entries[it->second.next++];
        const std::uint32_t dirent = arg(ctx, 1);
        write_stat(r, dirent, entry.directory, entry.size);
        r.memory().store32(dirent + 64u, lba_for(entry.path, entry.size));  // st_private[0]: start sector
        r.memory().zero(dirent + 88u, 256u);
        std::vector<std::uint8_t> bytes(entry.name.begin(), entry.name.end());
        if (bytes.size() > 255u) bytes.resize(255u);
        r.memory().copy_in(dirent + 88u, bytes);
        ret(ctx, 1u);
    });
    reg(rt, lib, 0xEB092469u, [](Runtime &, AllegrexContext &ctx) {  // sceIoDclose
        ret(ctx, k.directories.erase(static_cast<std::int32_t>(arg(ctx, 0))) ? 0u : kErrorBadFile);
    });
    reg(rt, lib, 0x54F5FB11u, [](Runtime &r, AllegrexContext &ctx) {  // sceIoDevctl
        const std::uint32_t command = arg(ctx, 1);
        const std::uint32_t out = arg(ctx, 4), out_length = arg(ctx, 5);
        if (command == 0x01F20001u && out_length >= 8u && r.memory().contains(out, 8u)) {  // UMD disc type
            r.memory().store32(out + 0u, 0u);
            r.memory().store32(out + 4u, 0x10u);  // PSP game disc
        }
        if (command == 0x02425818u && arg(ctx, 2) != 0u) {  // ms0 free space
            const std::uint32_t info = r.memory().load32(arg(ctx, 2));
            if (r.memory().contains(info, 20u)) {
                r.memory().store32(info + 0u, 0x10000u);
                r.memory().store32(info + 4u, 0x8000u);
                r.memory().store32(info + 8u, 0x8000u);
                r.memory().store32(info + 12u, 512u);
                r.memory().store32(info + 16u, 64u);
            }
        }
        ret(ctx, 0u);
    });
    reg_value(rt, lib, 0xB293727Fu, 0u);  // ChangeAsyncPriority
    reg_value(rt, lib, 0x779103A0u, 0u);  // Rename
}

void install_display_input(Runtime &rt) {
    reg_value(rt, "sceDisplay", 0x0E20F177u, 0u);  // SetMode
    reg(rt, "sceDisplay", 0x289D82FEu, [](Runtime &r, AllegrexContext &ctx) {  // SetFrameBuf
        workload::end_frame(r.memory());
#if defined(BURNOUT_GE_RENDER)
        render::frame_buffer_set(r.memory(), arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), k.vblanks);
#endif
        if (k.observer.on_set_frame_buffer) k.observer.on_set_frame_buffer(arg(ctx, 0), k.vblanks);
        ret(ctx, 0u);
    });
    reg(rt, "sceDisplay", 0x8EB9EC49u, [](Runtime &r, AllegrexContext &ctx) {  // WaitVblankCB
        if (k.observer.on_wait_vblank) k.observer.on_wait_vblank(k.current, k.vblanks);
        if (Thread *self = find_thread(k.current)) self->wait_a = static_cast<std::uint32_t>(k.vblanks + 1u);
        block_and_switch(r, ctx, WaitKind::Vblank);
    });

    reg(rt, "sceCtrl", 0x1F4011E6u, [](Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); });  // SetSamplingMode
    reg(rt, "sceCtrl", 0x1F803938u, [](Runtime &r, AllegrexContext &ctx) {  // ReadBufferPositive
        const std::uint32_t buttons = k.observer.controller_buttons ? k.observer.controller_buttons(k.vblanks) : 0u;
        const std::uint32_t count = std::max<std::uint32_t>(arg(ctx, 1), 1u);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t pad = arg(ctx, 0) + i * 16u;
            r.memory().zero(pad, 16u);
            r.memory().store32(pad + 0u, static_cast<std::uint32_t>(k.time_us));
            r.memory().store32(pad + 4u, buttons);
            const std::uint32_t analog = k.observer.controller_analog ? k.observer.controller_analog() : 0x8080u;
            r.memory().store8(pad + 8u, static_cast<std::uint8_t>(analog & 0xFFu));
            r.memory().store8(pad + 9u, static_cast<std::uint8_t>((analog >> 8u) & 0xFFu));
        }
        ret(ctx, count);
    });
}

void install_ge(Runtime &rt) {
    constexpr const char *lib = "sceGe_user";
    reg_value(rt, lib, 0xE47E40E4u, 0x04000000u);  // EdramGetAddr
    reg(rt, lib, 0xAB49E76Au, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, ge_enqueue(r, ctx)); });
    reg(rt, lib, 0x1C0D95A6u, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, ge_enqueue(r, ctx)); });
    reg(rt, lib, 0xE0D68148u, [](Runtime &r, AllegrexContext &ctx) {  // UpdateStallAddr
        const auto it = k.ge_lists.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it != k.ge_lists.end()) {
            it->second.stall = arg(ctx, 1);
            ge_scan(r, it->second);
            if (it->second.done) k.ge_lists.erase(it);
        }
        ret(ctx, 0u);
    });
    reg_value(rt, lib, 0x03444EB4u, 0u);  // ListSync: every accepted list is complete
    reg_value(rt, lib, 0xB287BD61u, 0u);  // DrawSync
    reg_value(rt, lib, 0xB448EC0Du, 0u);  // Break
    reg_value(rt, lib, 0x4C06E472u, 0u);  // Continue
    reg(rt, lib, 0xA4FC06A4u, [](Runtime &r, AllegrexContext &ctx) {  // SetCallback
        const std::uint32_t data = arg(ctx, 0);
        GeCallback callback{r.memory().load32(data), r.memory().load32(data + 4u),
                            r.memory().load32(data + 8u), r.memory().load32(data + 12u)};
        const std::int32_t id = k.next_ge_callback++;
        k.ge_callbacks[id] = callback;
        log_line("ge callback " + std::to_string(id) + " signal=" + psprecomp::hex32(callback.signal_func) +
                 " finish=" + psprecomp::hex32(callback.finish_func));
        ret(ctx, static_cast<std::uint32_t>(id));
    });
    reg(rt, lib, 0x05DB22CEu, [](Runtime &, AllegrexContext &ctx) {
        k.ge_callbacks.erase(static_cast<std::int32_t>(arg(ctx, 0)));
        ret(ctx, 0u);
    });
}

// Hands one output buffer of a channel to the host sound output.
void audio_submit(Runtime &r, std::uint32_t channel, std::uint32_t left, std::uint32_t right, std::uint32_t buffer) {
    if (!audio::enabled() || channel >= k.audio.size() || buffer == 0u) return;
    const auto &ch = k.audio[channel];
    const std::uint32_t bytes = ch.samples * (ch.mono ? 2u : 4u);
    if (const std::uint8_t *data = r.memory().raw_pointer(buffer, bytes))
        audio::submit(channel, reinterpret_cast<const std::int16_t *>(data), ch.samples, !ch.mono, left, right);
}

// Virtual time one output call of `channel` lasts: the samples' real length,
// scaled so the game keeps the host queue fed when it runs slower than real
// time (see audio::pace).
std::uint64_t audio_duration(std::uint32_t channel) {
    const double us = static_cast<double>(k.audio[channel].samples) * 1000000.0 / 44100.0;
    return static_cast<std::uint64_t>(us * (audio::enabled() ? audio::pace(channel) : 1.0));
}

void audio_block(Runtime &r, AllegrexContext &ctx, std::uint32_t channel) {
    if (channel >= k.audio.size()) { ret(ctx, 0x80260003u); return; }
    auto &ch = k.audio[channel];
    const std::uint64_t duration = audio_duration(channel);
    const std::uint64_t start = std::max(k.time_us, ch.busy_until);
    ch.busy_until = start + duration;
    if (start <= k.time_us) { ret(ctx, 0u); return; }
    block_and_switch(r, ctx, WaitKind::Delay, start);
}

void install_audio(Runtime &rt) {
    constexpr const char *lib = "sceAudio";
    reg(rt, lib, 0x5EC81C55u, [](Runtime &, AllegrexContext &ctx) {  // ChReserve
        auto channel = static_cast<std::int32_t>(arg(ctx, 0));
        if (channel < 0) {
            channel = -1;
            for (std::size_t i = 0; i < k.audio.size(); ++i)
                if (!k.audio[i].reserved) { channel = static_cast<std::int32_t>(i); break; }
        }
        if (channel < 0 || channel >= static_cast<std::int32_t>(k.audio.size())) { ret(ctx, 0x80260002u); return; }
        k.audio[static_cast<std::size_t>(channel)] = AudioChannel{true, arg(ctx, 1), k.time_us, arg(ctx, 2) == 0x10u};
        ret(ctx, static_cast<std::uint32_t>(channel));
    });
    reg(rt, lib, 0x6FC46853u, [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < k.audio.size()) k.audio[arg(ctx, 0)] = AudioChannel{};
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x136CAF51u, [](Runtime &r, AllegrexContext &ctx) {  // OutputBlocking(channel, volume, buffer)
        audio_submit(r, arg(ctx, 0), arg(ctx, 1), arg(ctx, 1), arg(ctx, 2));
        audio_block(r, ctx, arg(ctx, 0));
    });
    reg(rt, lib, 0x13F592BCu, [](Runtime &r, AllegrexContext &ctx) {  // OutputPannedBlocking(channel, left, right, buffer)
        audio_submit(r, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        audio_block(r, ctx, arg(ctx, 0));
    });
    reg(rt, lib, 0xE2D56B2Du, [](Runtime &r, AllegrexContext &ctx) {  // OutputPanned (non-blocking)
        if (arg(ctx, 0) >= k.audio.size()) { ret(ctx, 0x80260003u); return; }
        auto &ch = k.audio[arg(ctx, 0)];
        const std::uint64_t duration = audio_duration(arg(ctx, 0));
        if (ch.busy_until > k.time_us + duration) { ret(ctx, kErrorAudioBusy); return; }
        ch.busy_until = std::max(k.time_us, ch.busy_until) + duration;
        audio_submit(r, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        ret(ctx, 0u);
    });
    reg(rt, lib, 0xB011922Fu, [](Runtime &, AllegrexContext &ctx) {  // GetChannelRestLength
        if (arg(ctx, 0) >= k.audio.size()) { ret(ctx, 0x80260003u); return; }
        const auto &ch = k.audio[arg(ctx, 0)];
        const std::uint64_t rest_us = ch.busy_until > k.time_us ? ch.busy_until - k.time_us : 0u;
        ret(ctx, static_cast<std::uint32_t>(rest_us * 44100u / 1000000u));
    });
    reg(rt, lib, 0xCB2E439Eu, [](Runtime &, AllegrexContext &ctx) {  // SetChannelDataLen
        if (arg(ctx, 0) < k.audio.size()) k.audio[arg(ctx, 0)].samples = arg(ctx, 1);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x95FD0C2Du, [](Runtime &, AllegrexContext &ctx) {  // ChangeChannelConfig(channel, format)
        if (arg(ctx, 0) < k.audio.size()) k.audio[arg(ctx, 0)].mono = arg(ctx, 1) == 0x10u;
        ret(ctx, 0u);
    });
    reg_value(rt, lib, 0xB7E1D8E7u, 0u);

    constexpr const char *sas = "sceSasCore";
    // With sound off, init fails as it did while the import stubs were not
    // dispatched (the wrapper's own 0x80420000): the game then leaves its SAS
    // voices alone, which saves it real CPU time and keeps the PC validation
    // runs as they were.
    reg(rt, sas, 0x42778A9Fu, [](Runtime &, AllegrexContext &ctx) {  // __sceSasInit(core, grain, voices, mode, rate)
        if (!audio::enabled()) { ret(ctx, 0x80420000u); return; }
        k.sas_grain = std::max<std::uint32_t>(arg(ctx, 1), 64u);
        sas::init(k.sas_grain);
        ret(ctx, 0u);
    });
    reg(rt, sas, 0xA3589D81u, [](Runtime &r, AllegrexContext &ctx) {  // __sceSasCore(core, out)
        workload::sas_core(k.time_us);
        if (!r.memory().contains(arg(ctx, 1), k.sas_grain * 4u)) { ret(ctx, 0u); return; }
        if (audio::enabled()) {
            const double started = host_now_ms();
            sas::mix(reinterpret_cast<std::int16_t *>(r.memory().raw_pointer(arg(ctx, 1), k.sas_grain * 4u)));
            g_sas_ms += host_now_ms() - started;
            ++g_sas_grains;
        }
        else r.memory().zero(arg(ctx, 1), k.sas_grain * 4u);
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x68A46B95u, [](Runtime &, AllegrexContext &ctx) {  // GetEndFlag
        ret(ctx, audio::enabled() ? sas::end_flags() : 0xFFFFFFFFu);
    });
    const auto voice = [](const AllegrexContext &ctx) { return static_cast<int>(arg(ctx, 1)); };
    reg(rt, sas, 0x99944089u, [voice](Runtime &r, AllegrexContext &ctx) {  // SetVoice(core, voice, vag, size, loop)
        workload::sas_set_voice(arg(ctx, 1), arg(ctx, 3), arg(ctx, 4) != 0u);
        sas::set_voice(voice(ctx), r.memory().raw_pointer(arg(ctx, 2), arg(ctx, 3)), arg(ctx, 3), arg(ctx, 4) != 0u);
        ret(ctx, 0u);
    });
    reg(rt, sas, 0xAD84D37Fu, [voice](Runtime &, AllegrexContext &ctx) {  // SetPitch
        workload::sas_set_pitch(arg(ctx, 1), arg(ctx, 2));
        sas::set_pitch(voice(ctx), arg(ctx, 2));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x76F01ACAu, [voice](Runtime &, AllegrexContext &ctx) {  // SetKeyOn
        workload::sas_key_on(arg(ctx, 1), k.time_us);
        sas::key_on(voice(ctx));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0xA0CF2FA4u, [voice](Runtime &, AllegrexContext &ctx) {  // SetKeyOff
        workload::sas_key_off(arg(ctx, 1));
        sas::key_off(voice(ctx));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x440CA7D8u, [voice](Runtime &, AllegrexContext &ctx) {  // SetVolume(core, voice, l, r, effect l, effect r)
        sas::set_volume(voice(ctx), static_cast<std::int32_t>(arg(ctx, 2)), static_cast<std::int32_t>(arg(ctx, 3)));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0xB7660A23u, [voice](Runtime &, AllegrexContext &ctx) {  // SetNoise(core, voice, frequency)
        sas::set_noise(voice(ctx), arg(ctx, 2));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x019B25EBu, [voice](Runtime &, AllegrexContext &ctx) {  // SetADSR(core, voice, flags, a, d, s, r)
        sas::set_adsr(voice(ctx), arg(ctx, 2), static_cast<std::int32_t>(arg(ctx, 3)), static_cast<std::int32_t>(arg(ctx, 4)),
                      static_cast<std::int32_t>(arg(ctx, 5)), static_cast<std::int32_t>(arg(ctx, 6)));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x9EC3676Au, [voice](Runtime &, AllegrexContext &ctx) {  // SetADSRmode(core, voice, flags, a, d, s, r)
        sas::set_adsr_mode(voice(ctx), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4), arg(ctx, 5), arg(ctx, 6));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x5F9529F6u, [voice](Runtime &, AllegrexContext &ctx) {  // SetSL(core, voice, level)
        sas::set_sustain_level(voice(ctx), static_cast<std::int32_t>(arg(ctx, 2)));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0xCBCD4F79u, [voice](Runtime &, AllegrexContext &ctx) {  // SetSimpleADSR(core, voice, env1, env2)
        sas::set_simple_adsr(voice(ctx), arg(ctx, 2), arg(ctx, 3));
        ret(ctx, 0u);
    });
    reg(rt, sas, 0x74AE582Au, [voice](Runtime &, AllegrexContext &ctx) {  // GetEnvelopeHeight(core, voice)
        ret(ctx, audio::enabled() ? static_cast<std::uint32_t>(sas::envelope_height(voice(ctx))) : 0u);
    });
    // Reverb (RevVON, RevEVOL, RevType, RevParam): not modelled.
    for (const std::uint32_t nid : {0xF983B186u, 0xD5A229C9u, 0x33D4AB37u, 0x267A6DD2u}) reg_value(rt, sas, nid, 0u);

    constexpr const char *atrac = "sceAtrac3plus";
    // Music: with sound on, the RIFF header the game hands over picks a
    // pre-converted track (see music.hpp); otherwise silent frames, as before.
    struct AtracStream {
        std::uint32_t buffer{};
        int track{-1};
        std::int32_t loops{};
    };
    std::int32_t *next_atrac = new std::int32_t(0);
    auto *streams = new std::map<std::int32_t, AtracStream>();
    reg(rt, atrac, 0x7A20E7AFu, [next_atrac, streams](Runtime &r, AllegrexContext &ctx) {  // SetDataAndGetID(buffer, size)
        workload::atrac_set_data(r.memory(), arg(ctx, 0), arg(ctx, 1));
        const std::int32_t id = (*next_atrac)++ % 6;
        AtracStream &stream = (*streams)[id];
        music::close(stream.track);
        stream = AtracStream{arg(ctx, 0)};
        if (audio::enabled()) stream.track = music::open(r.memory().raw_pointer(arg(ctx, 0), 8u), 8u);
        ret(ctx, static_cast<std::uint32_t>(id));
    });
    reg(rt, atrac, 0x61EB33F5u, [streams](Runtime &, AllegrexContext &ctx) {  // ReleaseAtracID
        const auto it = streams->find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it != streams->end()) {
            music::close(it->second.track);
            streams->erase(it);
        }
        ret(ctx, 0u);
    });
    reg(rt, atrac, 0x6A8C3CD5u, [streams](Runtime &r, AllegrexContext &ctx) {  // DecodeData(id, out, samples, finish, remain)
        constexpr std::uint32_t kSamples = 2048u;
        workload::atrac_decode();
        const auto it = streams->find(static_cast<std::int32_t>(arg(ctx, 0)));
        std::uint32_t finished = 0u;
        if (!r.memory().contains(arg(ctx, 1), kSamples * 4u)) {
        } else if (it != streams->end() && it->second.track >= 0) {
            auto *out = reinterpret_cast<std::int16_t *>(r.memory().raw_pointer(arg(ctx, 1), kSamples * 4u));
            if (!music::decode(it->second.track, out, kSamples, it->second.loops)) finished = 1u;
        } else {
            r.memory().zero(arg(ctx, 1), kSamples * 4u);
        }
        if (arg(ctx, 2) != 0u) r.memory().store32(arg(ctx, 2), kSamples);
        if (arg(ctx, 3) != 0u) r.memory().store32(arg(ctx, 3), finished);
        if (arg(ctx, 4) != 0u) r.memory().store32(arg(ctx, 4), 0xFFFFFFFFu);
        ret(ctx, 0u);
    });
    reg(rt, atrac, 0x5D268707u, [streams](Runtime &r, AllegrexContext &ctx) {  // GetStreamDataInfo
        const auto it = streams->find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (arg(ctx, 1) != 0u) r.memory().store32(arg(ctx, 1), it == streams->end() ? 0u : it->second.buffer);
        if (arg(ctx, 2) != 0u) r.memory().store32(arg(ctx, 2), 0u);
        if (arg(ctx, 3) != 0u) r.memory().store32(arg(ctx, 3), 0u);
        ret(ctx, 0u);
    });
    reg_value(rt, atrac, 0x7DB31251u, 0u);  // AddStreamData
    reg(rt, atrac, 0x868120B5u, [streams](Runtime &, AllegrexContext &ctx) {  // SetLoopNum(id, loops)
        const auto it = streams->find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (it != streams->end()) it->second.loops = static_cast<std::int32_t>(arg(ctx, 1));
        ret(ctx, 0u);
    });
}

void call_guest(AllegrexContext &ctx, std::uint32_t function, std::initializer_list<std::uint32_t> args,
                std::function<std::uint32_t(Runtime &, std::uint32_t)> on_return) {
    GuestCallFrame frame;
    frame.caller = ctx;
    frame.caller.pc = ctx.gpr[31];
    frame.on_return = std::move(on_return);
    k.guest_calls[k.current].push_back(std::move(frame));
    unsigned index = 0;
    for (const std::uint32_t value : args) ctx.gpr[4u + index++] = value;
    ctx.gpr[29] = (ctx.gpr[29] - 0x100u) & ~0xFu;
    ctx.gpr[31] = kGuestCallReturnPc;
    ctx.pc = function;
}

void guest_call_return(Runtime &rt, AllegrexContext &ctx) {
    auto &frames = k.guest_calls[k.current];
    if (frames.empty()) { rt.stop("guest call return without a pending frame"); return; }
    GuestCallFrame frame = std::move(frames.back());
    frames.pop_back();
    const std::uint32_t callee_v0 = ctx.gpr[2];
    ctx = frame.caller;
    ctx.gpr[2] = frame.on_return(rt, callee_v0);
}

// SceMpegRingbuffer: packets, packetsRead, packetsWritten, packetsFree,
// packetSize, data, callback, cbArg, dataUpperBound, semaID, mpeg.
void install_mpeg(Runtime &rt) {
    constexpr const char *lib = "sceMpeg";
    reg(rt, lib, 0x37295ED8u, [](Runtime &r, AllegrexContext &ctx) {  // RingbufferConstruct
        const std::uint32_t rb = arg(ctx, 0), packets = arg(ctx, 1);
        r.memory().zero(rb, 44u);
        r.memory().store32(rb + 0u, packets);
        r.memory().store32(rb + 12u, packets);
        r.memory().store32(rb + 16u, 2048u);
        r.memory().store32(rb + 20u, arg(ctx, 2));
        r.memory().store32(rb + 24u, arg(ctx, 4));
        r.memory().store32(rb + 28u, arg(ctx, 5));
        r.memory().store32(rb + 32u, arg(ctx, 2) + packets * 2048u);
        k.mpeg_ringbuffers[rb] = MpegRingbuffer{rb, false};
        ret(ctx, 0u);
    });
    reg(rt, lib, 0xD8C5F121u, [](Runtime &, AllegrexContext &ctx) {  // sceMpegCreate
        k.mpeg_ringbuffer_of[arg(ctx, 0)] = arg(ctx, 3);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0xB5F6DC87u, [](Runtime &r, AllegrexContext &ctx) {  // RingbufferAvailableSize
        ret(ctx, r.memory().load32(arg(ctx, 0) + 12u));
    });
    reg(rt, lib, 0xB240A59Eu, [](Runtime &r, AllegrexContext &ctx) {  // RingbufferPut: runs the game's reader
        const std::uint32_t rb = arg(ctx, 0);
        const std::uint32_t packets = r.memory().load32(rb + 0u);
        const std::uint32_t written = r.memory().load32(rb + 8u);
        const std::uint32_t free = r.memory().load32(rb + 12u);
        const std::uint32_t index = packets ? written % packets : 0u;
        const std::uint32_t count = std::min({arg(ctx, 1), arg(ctx, 2), free, packets - index});
        const std::uint32_t callback = r.memory().load32(rb + 24u);
        if (count == 0u || callback == 0u) { ret(ctx, 0u); return; }
        const std::uint32_t data = r.memory().load32(rb + 20u) + index * 2048u;
        call_guest(ctx, callback, {data, count, r.memory().load32(rb + 28u)},
                   [rb](Runtime &rr, std::uint32_t read) -> std::uint32_t {
                       if (static_cast<std::int32_t>(read) <= 0) {
                           k.mpeg_ringbuffers[rb].end_of_stream = true;
                           return read;
                       }
                       rr.memory().store32(rb + 4u, rr.memory().load32(rb + 4u) + read);
                       rr.memory().store32(rb + 8u, rr.memory().load32(rb + 8u) + read);
                       rr.memory().store32(rb + 12u, rr.memory().load32(rb + 12u) - std::min(read, rr.memory().load32(rb + 12u)));
                       return read;
                   });
    });
    // Headless: nothing is decoded. Each access unit request drains whatever the
    // game has streamed in; once its reader hits EOF the movie ends.
    reg(rt, lib, 0xFE246728u, [](Runtime &r, AllegrexContext &ctx) {  // sceMpegGetAvcAu
        const auto mapping = k.mpeg_ringbuffer_of.find(arg(ctx, 0));
        if (mapping == k.mpeg_ringbuffer_of.end()) { ret(ctx, 0x80618001u); return; }
        const std::uint32_t rb = mapping->second;
        const std::uint32_t packets = r.memory().load32(rb + 0u);
        if (r.memory().load32(rb + 12u) < packets) {
            r.memory().store32(rb + 12u, packets);
            ret(ctx, 0u);
            return;
        }
        ret(ctx, 0x80618001u);  // SCE_MPEG_ERROR_NO_DATA
    });
}

// SceUtilitySavedataParam (FW 1.x layout): mode @48, gameName[13] @60,
// saveName[20] @76, saveNameList @96, fileName[13] @100, dataBuf @116,
// dataBufSize @120, dataSize @124. Saves are stored unencrypted under ms0:,
// which is also how a save prepared with PPSSPP's EncryptSave=False looks.
std::uint32_t savedata_execute(Runtime &r, std::uint32_t params) {
    auto &memory = r.memory();
    const std::uint32_t mode = memory.load32(params + 48u);
    const std::string game = memory.read_c_string(params + 60u, 13u);
    std::string save = memory.read_c_string(params + 76u, 20u);
    const std::string file = memory.read_c_string(params + 100u, 13u);
    const fs::path root = k.options.memstick / "PSP" / "SAVEDATA";
    std::error_code ec;

    const bool list_mode = mode == 4u || mode == 5u;
    if (list_mode && memory.load32(params + 96u) != 0u) {
        // List dialogs: the player picks the first entry that exists (load) or
        // the first entry offered (save), as if pressing CROSS on it.
        const std::uint32_t list = memory.load32(params + 96u);
        std::string chosen;
        for (std::uint32_t i = 0; i < 64u; ++i) {
            const std::string candidate = memory.read_c_string(list + i * 20u, 20u);
            if (candidate.empty()) break;
            if (mode == 5u || fs::exists(root / (game + candidate) / file, ec)) { chosen = candidate; break; }
        }
        if (chosen.empty()) {
            log_line("savedata mode=" + std::to_string(mode) + " " + game + ": no listed save");
            return kSavedataNoData;
        }
        save = chosen;
        std::vector<std::uint8_t> name(20u, 0u);
        std::copy_n(save.begin(), std::min<std::size_t>(save.size(), 19u), name.begin());
        memory.copy_in(params + 76u, name);
    }
    const fs::path path = root / (game + save) / file;
    const std::uint32_t buffer = memory.load32(params + 116u);
    const std::uint32_t buffer_size = memory.load32(params + 120u);

    if (mode == 0u || mode == 2u || mode == 4u) {  // AUTOLOAD / LOAD / LISTLOAD
        std::FILE *in = std::fopen(path.string().c_str(), "rb");
        if (in == nullptr) {
            log_line("savedata load " + path.generic_string() + ": no data");
            return kSavedataNoData;
        }
        std::uint8_t *host = memory.raw_pointer(buffer, buffer_size);
        const std::size_t read = host ? std::fread(host, 1u, buffer_size, in) : 0u;
        std::fclose(in);
        memory.store32(params + 124u, static_cast<std::uint32_t>(read));
        log_line("savedata load " + path.generic_string() + " " + std::to_string(read) + " bytes");
        return 0u;
    }
    if (mode == 1u || mode == 3u || mode == 5u) {  // AUTOSAVE / SAVE / LISTSAVE
        const std::uint32_t size = std::min(memory.load32(params + 124u), buffer_size);
        const std::uint8_t *host = memory.raw_pointer(buffer, size);
        fs::create_directories(path.parent_path(), ec);
        if (std::FILE *out = std::fopen(path.string().c_str(), "wb")) {
            if (host != nullptr) std::fwrite(host, 1u, size, out);
            std::fclose(out);
        }
        log_line("savedata save " + path.generic_string() + " " + std::to_string(size) + " bytes");
        return 0u;
    }
    log_line("savedata mode=" + std::to_string(mode) + " (not emulated)");
    return 0u;
}

void install_utility(Runtime &rt) {
    constexpr const char *lib = "sceUtility";
    reg(rt, lib, 0x50C4CD57u, [](Runtime &r, AllegrexContext &ctx) {  // SavedataInitStart
        const std::uint32_t params = arg(ctx, 0);
        utility_init(k.savedata, params, savedata_execute(r, params));
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x9790B33Cu, [](Runtime &, AllegrexContext &ctx) { k.savedata.status = 4u; ret(ctx, 0u); });
    reg_value(rt, lib, 0xD4B95FFBu, 0u);
    reg(rt, lib, 0x8874DBE0u, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, utility_status(r, k.savedata)); });

    reg(rt, lib, 0x2AD8E239u, [](Runtime &r, AllegrexContext &ctx) {  // MsgDialogInitStart
        const std::uint32_t params = arg(ctx, 0);
        if (r.memory().contains(params + 576u, 4u)) r.memory().store32(params + 576u, 1u);  // "Yes"
        log_line("msg dialog");
        utility_init(k.msg_dialog, params, 0u);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x67AF3428u, [](Runtime &, AllegrexContext &ctx) { k.msg_dialog.status = 4u; ret(ctx, 0u); });
    reg_value(rt, lib, 0x95FC253Bu, 0u);
    reg(rt, lib, 0x9A1C91D7u, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, utility_status(r, k.msg_dialog)); });

    reg(rt, lib, 0x4DB1E739u, [](Runtime &, AllegrexContext &ctx) { utility_init(k.netconf, arg(ctx, 0), kUtilityUnsupported); ret(ctx, 0u); });
    reg(rt, lib, 0xF88155F6u, [](Runtime &, AllegrexContext &ctx) { k.netconf.status = 4u; ret(ctx, 0u); });
    reg_value(rt, lib, 0x91E70E35u, 0u);
    reg(rt, lib, 0x6332AA39u, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, utility_status(r, k.netconf)); });
    reg(rt, lib, 0xC492F751u, [](Runtime &, AllegrexContext &ctx) { utility_init(k.game_sharing, arg(ctx, 0), kUtilityUnsupported); ret(ctx, 0u); });
    reg(rt, lib, 0xEFC6F80Fu, [](Runtime &, AllegrexContext &ctx) { k.game_sharing.status = 4u; ret(ctx, 0u); });
    reg_value(rt, lib, 0x7853182Du, 0u);
    reg(rt, lib, 0x946963F3u, [](Runtime &r, AllegrexContext &ctx) { ret(ctx, utility_status(r, k.game_sharing)); });

    reg(rt, lib, 0xA5DA2406u, [](Runtime &r, AllegrexContext &ctx) {  // GetSystemParamInt
        std::uint32_t value = 0u;
        switch (arg(ctx, 0)) {
        case 8u: value = 1u; break;  // language: English
        case 9u: value = 1u; break;  // confirm button: cross
        default: break;
        }
        if (arg(ctx, 1) != 0u) r.memory().store32(arg(ctx, 1), value);
        ret(ctx, 0u);
    });
    reg(rt, lib, 0x34B78343u, [](Runtime &r, AllegrexContext &ctx) {  // GetSystemParamString
        const std::string nickname = "PLAYER";
        std::vector<std::uint8_t> bytes(nickname.begin(), nickname.end());
        bytes.push_back(0u);
        if (arg(ctx, 2) >= bytes.size()) r.memory().copy_in(arg(ctx, 1), bytes);
        ret(ctx, 0u);
    });

    reg_value(rt, "sceUmdUser", 0x46EBB729u, 1u);  // CheckMedium
    for (const std::uint32_t nid : {0xC6183D47u, 0x8EF08FCEu, 0x20628E6Fu, 0xAEE7404Du, 0xBD2BDE07u})
        reg_value(rt, "sceUmdUser", nid, 0u);
    reg_value(rt, "scePower", 0x04B7766Eu, 0u);
    reg(rt, "sceRtc", 0x3F7AD767u, [](Runtime &r, AllegrexContext &ctx) {  // GetCurrentTick
        constexpr std::uint64_t kTicks2006 = 63271180800ull * 1000000ull;
        if (arg(ctx, 0) != 0u) store64(r, arg(ctx, 0), kTicks2006 + k.time_us);
        ret(ctx, 0u);
    });
    reg_value(rt, "sceWlanDrv", 0xD7763699u, 0u);
}

void thread_return(Runtime &rt, AllegrexContext &ctx) { exit_thread(rt, ctx, ctx.gpr[2], false); }

void interrupt_return(Runtime &rt, AllegrexContext &ctx) {
    if (!k.pending_interrupts.empty()) {
        close_slice();
        start_interrupt(ctx);
        g_slice = {kInterruptSlice, host_now_ms()};
        return;
    }
    k.in_interrupt = false;
    if (k.interrupted_valid) {
        k.interrupted_valid = false;
        ctx = k.interrupted;
        k.current = k.interrupted_uid;
        if (Thread *thread = find_thread(k.current)) psprecomp::set_runtime_thread_identity(k.current, thread->name);
        return;
    }
    dispatch_next(rt, ctx);
}

} // namespace

std::uint64_t virtual_time_us() noexcept { return k.time_us; }
std::uint64_t vblank_count() noexcept { return k.vblanks; }

void install_hle(Runtime &runtime, const HleOptions &options, HleObserver observer) {
    k = Kernel{};
    k.options = options;
    k.observer = std::move(observer);
    k.arena_low = (options.user_arena_start + 0xFFu) & ~0xFFu;
    const auto interrupt_stack = allocate(0x10000u, 1u, 0u, "interrupt stack");
    k.interrupt_stack_top = interrupt_stack.value_or(kUserRamEnd) + 0x10000u - 0x40u;

    install_threadman(runtime);
    install_kernel_misc(runtime);
    install_io(runtime);
    install_display_input(runtime);
    install_ge(runtime);
    install_audio(runtime);
    install_utility(runtime);
    install_mpeg(runtime);
    runtime.register_function(kThreadReturnPc, &thread_return, "psp_thread_return");
    runtime.register_function(kInterruptReturnPc, &interrupt_return, "psp_interrupt_return");
    runtime.register_function(kGuestCallReturnPc, &guest_call_return, "psp_guest_call_return");
}

void install_fallback_stubs(Runtime &runtime, const std::vector<std::pair<std::string, std::uint32_t>> &imports) {
    for (const auto &[library, nid] : imports) {
        if (k.implemented.contains({library, nid})) continue;
        const std::string name = library + "::" +
            runtime.nids().resolve(library, nid).value_or(psprecomp::hex32(nid));
        runtime.register_hle(library, nid, [name](Runtime &, AllegrexContext &ctx) {
            const std::uint64_t calls = ++k.stub_calls[name];
            if (calls <= 3u)
                std::cerr << "[stub] " << name << " a0=" << psprecomp::hex32(ctx.gpr[4])
                          << " a1=" << psprecomp::hex32(ctx.gpr[5]) << " a2=" << psprecomp::hex32(ctx.gpr[6])
                          << " vblank=" << k.vblanks << "\n";
            ret(ctx, 0u);
        });
    }
}

void prepare_boot_thread(Runtime &runtime, const std::string &boot_path) {
    Thread thread;
    thread.name = "user_main";
    thread.priority = 0x20u;
    thread.stack_size = 0x40000u;
    thread.gp = k.options.module_gp;
    const auto stack = allocate(thread.stack_size, 1u, 0u, "stack:user_main");
    if (!stack) throw psprecomp::Error("cannot allocate the boot thread stack");
    thread.stack_bottom = *stack;
    thread.stack_block = block_uid_at(*stack);
    const std::uint32_t top = thread.stack_bottom + thread.stack_size;
    runtime.memory().zero(thread.stack_bottom, thread.stack_size);
    const std::uint32_t kernel_context = top - 0x100u;
    runtime.memory().store32(kernel_context + 0xC8u, thread.stack_bottom);
    runtime.memory().store32(kernel_context + 0xF8u, 0xFFFFFFFFu);
    runtime.memory().store32(kernel_context + 0xFCu, 0xFFFFFFFFu);
    std::vector<std::uint8_t> argument(boot_path.begin(), boot_path.end());
    argument.push_back(0u);
    const std::uint32_t args = (kernel_context - static_cast<std::uint32_t>(argument.size())) & ~0xFu;
    runtime.memory().copy_in(args, argument);

    auto &cpu = runtime.cpu();
    cpu.gpr[4] = static_cast<std::uint32_t>(argument.size());
    cpu.gpr[5] = args;
    cpu.gpr[26] = kernel_context;
    cpu.gpr[28] = thread.gp;
    cpu.gpr[29] = args - 0x40u;
    cpu.gpr[31] = kThreadReturnPc;
    thread.state = ThreadState::Running;
    const std::int32_t uid = new_uid();
    k.threads.emplace(uid, std::move(thread));
    k.current = uid;
    psprecomp::set_runtime_thread_identity(uid, "user_main");
}

std::string thread_time_report(double frames) {
    const std::int32_t running = g_slice.uid;
    close_slice();
    if (running != -1) g_slice = {running, host_now_ms()};
    std::string report;
    static double last_report = host_now_ms();
    const double now = host_now_ms();
    double accounted = g_interrupt_ms;
    for (auto &[uid, thread] : k.threads) {
        (void)uid;
        accounted += thread.host_ms;
        if (thread.host_ms / frames >= 0.05) {
            char item[96];
            std::snprintf(item, sizeof(item), "%s%s %.1f", report.empty() ? "" : ", ", thread.name.c_str(),
                          thread.host_ms / frames);
            report += item;
        }
        thread.host_ms = 0.0;
    }
    if (g_sas_grains != 0u) {
        char item[96];
        std::snprintf(item, sizeof(item), "; sas mix %.1f (%.1f grains)", g_sas_ms / frames, g_sas_grains / frames);
        report += item;
        g_sas_ms = 0.0;
        g_sas_grains = 0u;
    }
    char item[96];
    std::snprintf(item, sizeof(item), "; interrupts %.1f, outside threads %.1f", g_interrupt_ms / frames,
                  (now - last_report - accounted) / frames);
    report += item;
    for (auto &[handler, time] : g_interrupt_times) {
        if (time.ms / frames < 0.1) continue;
        std::snprintf(item, sizeof(item), " [%08X: %.1f ms, %.1f/flip]", static_cast<unsigned>(handler), time.ms / frames,
                      time.count / frames);
        report += item;
    }
    g_interrupt_times.clear();
    g_interrupt_ms = 0.0;
    last_report = now;
    return report;
}

void print_hle_summary() {
    std::cerr << "[hle] virtual time " << k.time_us / 1000u << " ms, vblanks " << k.vblanks
              << ", GE lists " << k.ge_lists_submitted << " (" << k.ge_commands_scanned << " commands scanned)\n";
    for (const auto &[uid, thread] : k.threads) {
        const char *state = thread.state == ThreadState::Running ? "running"
            : thread.state == ThreadState::Ready ? "ready"
            : thread.state == ThreadState::Waiting ? "waiting" : "dormant";
        std::cerr << "[hle] thread " << uid << " " << thread.name << " prio=" << thread.priority
                  << " " << state << " wait=" << static_cast<int>(thread.wait)
                  << " pc=" << psprecomp::hex32(thread.ctx.pc) << "\n";
    }
    for (const auto &[name, calls] : k.stub_calls) std::cerr << "[hle] stub " << name << " x" << calls << "\n";
}

} // namespace burnout
