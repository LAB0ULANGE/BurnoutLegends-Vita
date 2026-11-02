#include "sas_worker.hpp"

#include "burnout_bench.hpp"
#include "thread_sync.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

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

void post(std::function<void()> work) {
    if (!active()) {
        work();
        return;
    }
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(work));
    g_cv.notify_all();
}

void drain() {
    if (!active()) return;
    std::unique_lock lock(g_mutex);
    g_cv.wait(lock, [] { return g_queue.empty() && !g_running_work; });
}

double take_busy_ms() { return static_cast<double>(g_busy_us.exchange(0u, std::memory_order_relaxed)) / 1000.0; }

} // namespace burnout::sas_worker
