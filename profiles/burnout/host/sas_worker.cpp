#include "sas_worker.hpp"

#include "burnout_bench.hpp"
#include "thread_sync.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__vita__)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>

#include <algorithm>
#else
#include <thread>
#endif

namespace burnout::sas_worker {
namespace {

SyncMutex g_mutex{"sas_worker"};
SyncCondition g_cv{"sas_worker", g_mutex};
std::deque<std::function<void()>> g_queue;
// Emulation thread: voice changes not handed over yet (post_deferred). They
// join the queue with the next post, so a grain's setters cost one wake-up.
std::vector<std::function<void()>> g_deferred;
bool g_running_work{};  // the worker has taken work it has not finished
std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_busy_us{0u};

void run() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock lock(g_mutex);
            g_cv.wait(lock, [] { return !g_queue.empty(); });
            work = std::move(g_queue.front());
            g_queue.pop_front();
            g_running_work = true;
        }
        const double started = host_now_ms();
        work();
        g_busy_us.fetch_add(static_cast<std::uint64_t>((host_now_ms() - started) * 1000.0), std::memory_order_relaxed);
        std::lock_guard lock(g_mutex);
        g_running_work = false;
        g_cv.notify_all();
    }
}

#if defined(__vita__)
void *thread_main(void *core_mask) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), static_cast<int>(reinterpret_cast<std::uintptr_t>(core_mask)));
    // Above the GL thread sharing the core, so the game seldom waits for it.
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 80);
    run();
    return nullptr;
}
#endif

} // namespace

bool start(int core) {
    if (g_active) return true;
#if defined(__vita__)
    const int masks[3]{SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, thread_main,
                       reinterpret_cast<void *>(static_cast<std::uintptr_t>(masks[std::clamp(core, 0, 2)]))) != 0)
        return false;
    pthread_detach(thread);
#else
    (void)core;
    std::thread(run).detach();
#endif
    g_active = true;
    return true;
}

bool active() noexcept { return g_active.load(std::memory_order_relaxed); }

namespace {
// Under the lock: the deferred work goes first, in order.
void take_deferred_locked() {
    for (auto &work : g_deferred) g_queue.push_back(std::move(work));
    g_deferred.clear();
}
} // namespace

void post(std::function<void()> work) {
    if (!active()) {
        work();
        return;
    }
    std::lock_guard lock(g_mutex);
    take_deferred_locked();
    g_queue.push_back(std::move(work));
    g_cv.notify_all();
}

void post_deferred(std::function<void()> work) {
    if (!active()) {
        work();
        return;
    }
    g_deferred.push_back(std::move(work));
}

void flush() {
    if (!active() || g_deferred.empty()) return;
    std::lock_guard lock(g_mutex);
    take_deferred_locked();
    g_cv.notify_all();
}

std::atomic<std::uint64_t> g_wait_us{0u}, g_waits{0u};

void drain_if_behind(std::size_t items) {
    if (!active()) return;
    {
        std::lock_guard lock(g_mutex);
        if (g_queue.size() + g_deferred.size() <= items) return;
    }
    drain();
}

void drain() {
    if (!active()) return;
    std::unique_lock lock(g_mutex);
    take_deferred_locked();
    if (!g_queue.empty()) g_cv.notify_all();
    if (g_queue.empty() && !g_running_work) return;
    // Work still queued or running: the emulation waits for it (traced).
    const double started = host_now_ms();
    g_cv.wait(lock, [] { return g_queue.empty() && !g_running_work; });
    g_wait_us.fetch_add(static_cast<std::uint64_t>((host_now_ms() - started) * 1000.0), std::memory_order_relaxed);
    g_waits.fetch_add(1u, std::memory_order_relaxed);
}

double take_wait_ms(std::uint64_t &waits) {
    waits = g_waits.exchange(0u, std::memory_order_relaxed);
    return static_cast<double>(g_wait_us.exchange(0u, std::memory_order_relaxed)) / 1000.0;
}

double take_busy_ms() { return static_cast<double>(g_busy_us.exchange(0u, std::memory_order_relaxed)) / 1000.0; }

} // namespace burnout::sas_worker
