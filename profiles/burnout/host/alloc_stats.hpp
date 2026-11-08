#pragma once

#include <string>

// Vita memory diagnostics (alloc_stats_vita.cpp, linked with --wrap=malloc
// etc.): bytes in use and each thread's net allocations, for the log.
namespace burnout::alloc_stats {

std::string report();

} // namespace burnout::alloc_stats
