#pragma once

#include <atomic>
#include <functional>
#include <utility>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#else
#include <thread>
#endif

namespace burnout {

// Starts `body` on a detached thread. On the Vita std::thread throws
// ("Enable multithreading to use std::thread"): pthreads are used there,
// as for the other threads of the port. On the Vita the thread runs at the
// lowest user priority on core 2 (the GL thread's, the least loaded): the
// kernel does not time-slice threads of equal priority, so a long CPU job
// (texture pack decoding, movie demuxing) at the default priority stalled
// the emulation or the decode thread for hundreds of milliseconds (0.0.4).
inline void start_detached(std::function<void()> body, unsigned stack_bytes = 256u * 1024u) {
#if defined(__vita__)
    auto *task = new std::function<void()>(std::move(body));
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack_bytes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int result = pthread_create(
        &thread, &attributes,
        [](void *argument) -> void * {
            auto *function = static_cast<std::function<void()> *>(argument);
            sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
            sceKernelChangeThreadPriority(sceKernelGetThreadId(), 191);
            (*function)();
            delete function;
            return nullptr;
        },
        task);
    pthread_attr_destroy(&attributes);
    if (result != 0) delete task;
#else
    (void)stack_bytes;
    std::thread(std::move(body)).detach();
#endif
}

// A lock that works on the Vita too: there libstdc++'s threads support is
// off (no pthread key symbols linked), so std::mutex locks nothing and
// std::condition_variable::wait returns at once (a waiting thread then spins
// at full speed: the 0.0.2 test's hang on the vitaGL logo). Held only for a
// few instructions.
class SpinLock {
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

} // namespace burnout
