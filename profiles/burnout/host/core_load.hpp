#pragma once

#include <string>

// Vita: how full each of the three user cores is. Threads register
// themselves; the report reads each one's kernel run time (runClocks) and
// adds it to the core it is pinned to. Elsewhere: no-ops.
namespace burnout::core_load {

// From inside the thread, after its affinity is set. The name is copied.
void register_current_thread(const char *name);

// Since the previous call: per core, % of wall time run by our threads, then
// each thread's share. Empty on the first call and off the Vita.
[[nodiscard]] std::string report();

// PC (Windows): the thread PSPRECOMP_PROFILE_THREAD names, once it registered
// (a HANDLE), for the sampling profiler; null otherwise.
[[nodiscard]] void *profile_target();

} // namespace burnout::core_load
