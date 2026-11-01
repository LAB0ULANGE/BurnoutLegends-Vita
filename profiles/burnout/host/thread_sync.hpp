#pragma once

// Mutex and condition variable for the renderer's thread handshakes. On the
// Vita, pthread-embedded's mutexes do not order the SCE main thread (the
// emulation) against a pthread, which corrupted the job queue once, so kernel
// objects are used there. Both satisfy BasicLockable for std::unique_lock.

#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#else
#include <condition_variable>
#endif

namespace burnout {

#if defined(__vita__)

class SyncMutex {
public:
    explicit SyncMutex(const char *name) : id_(sceKernelCreateMutex(name, 0, 0, nullptr)) {}
    void lock() { sceKernelLockMutex(id_, 1, nullptr); }
    void unlock() { sceKernelUnlockMutex(id_, 1); }
    SceUID id() const { return id_; }

private:
    SceUID id_;
};

class SyncCondition {
public:
    SyncCondition(const char *name, const SyncMutex &mutex) : id_(sceKernelCreateCond(name, 0, mutex.id(), nullptr)) {}
    template <typename Predicate> void wait(std::unique_lock<SyncMutex> &, Predicate ready) {
        while (!ready()) sceKernelWaitCond(id_, nullptr);
    }
    void notify_all() { sceKernelSignalCondAll(id_); }

private:
    SceUID id_;
};

#else

class SyncMutex {
public:
    explicit SyncMutex(const char *) {}
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

private:
    std::mutex mutex_;
};

class SyncCondition {
public:
    SyncCondition(const char *, const SyncMutex &) {}
    template <typename Predicate> void wait(std::unique_lock<SyncMutex> &lock, Predicate ready) {
        condition_.wait(lock, ready);
    }
    void notify_all() { condition_.notify_all(); }

private:
    std::condition_variable_any condition_;
};

#endif

} // namespace burnout
