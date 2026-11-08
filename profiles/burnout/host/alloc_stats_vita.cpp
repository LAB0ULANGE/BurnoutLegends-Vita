// Vita memory diagnostics: every malloc / free of the program (ours, the C++
// runtime's, vitaGL's) goes through these wrappers (linker --wrap). They keep
// the bytes in use, overall and by the allocating thread's net balance, so a
// growing heap tells a leak (bytes in use climb) from fragmentation (they do
// not). gl-103j: the heap top rose ~68 MiB in a 4 min race, our own tables
// staying flat.
#include "alloc_stats.hpp"

#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <malloc.h>

extern "C" {
void *__real_malloc(std::size_t size);
void __real_free(void *pointer);
void *__real_calloc(std::size_t count, std::size_t size);
void *__real_realloc(void *pointer, std::size_t size);
void *__real_memalign(std::size_t alignment, std::size_t size);
}

namespace {

struct Slot {
    std::atomic<int> thread{0};
    std::atomic<long long> net{0};      // bytes allocated minus freed by this thread
    std::atomic<long long> allocated{0};  // bytes allocated (window)
};

constexpr int kSlots = 48;
Slot g_slots[kSlots];
std::atomic<long long> g_live{0};

Slot *slot() {
    const int id = sceKernelGetThreadId();
    for (auto &s : g_slots) {
        const int owner = s.thread.load(std::memory_order_relaxed);
        if (owner == id) return &s;
        if (owner == 0) {
            int expected = 0;
            if (s.thread.compare_exchange_strong(expected, id)) return &s;
            if (expected == id) return &s;
        }
    }
    return nullptr;
}

void count(void *pointer, long long sign) {
    if (pointer == nullptr) return;
    const long long bytes = static_cast<long long>(malloc_usable_size(pointer)) * sign;
    g_live.fetch_add(bytes, std::memory_order_relaxed);
    if (Slot *s = slot()) {
        s->net.fetch_add(bytes, std::memory_order_relaxed);
        if (sign > 0) s->allocated.fetch_add(bytes, std::memory_order_relaxed);
    }
}

} // namespace

extern "C" {
void *__wrap_malloc(std::size_t size) {
    void *p = __real_malloc(size);
    count(p, 1);
    return p;
}
void __wrap_free(void *pointer) {
    count(pointer, -1);
    __real_free(pointer);
}
void *__wrap_calloc(std::size_t count_, std::size_t size) {
    void *p = __real_calloc(count_, size);
    count(p, 1);
    return p;
}
void *__wrap_realloc(void *pointer, std::size_t size) {
    count(pointer, -1);
    void *p = __real_realloc(pointer, size);
    count(p != nullptr ? p : pointer, 1);
    return p;
}
void *__wrap_memalign(std::size_t alignment, std::size_t size) {
    void *p = __real_memalign(alignment, size);
    count(p, 1);
    return p;
}
}

namespace burnout::alloc_stats {

std::string report() {
    char line[1024];
    int at = std::snprintf(line, sizeof(line), "allocations: in use %lld KiB; by thread (net / allocated since last):",
                           g_live.load(std::memory_order_relaxed) / 1024);
    for (auto &s : g_slots) {
        const int id = s.thread.load(std::memory_order_relaxed);
        if (id == 0 || at >= static_cast<int>(sizeof(line)) - 64) continue;
        const long long net = s.net.load(std::memory_order_relaxed);
        const long long allocated = s.allocated.exchange(0, std::memory_order_relaxed);
        if (net / 1024 == 0 && allocated / 1024 == 0) continue;
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        const char *name = sceKernelGetThreadInfo(id, &info) >= 0 ? info.name : "?";
        at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), " %s %lld/%lld", name, net / 1024, allocated / 1024);
    }
    return line;
}

} // namespace burnout::alloc_stats
