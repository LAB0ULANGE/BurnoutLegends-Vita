#pragma once
// Logging of the PPSSPP sources compiled here: dropped.
namespace Log { enum Type { Loader, ME, HLE, Common }; }
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#define VERBOSE_LOG(...) ((void)0)
#define _assert_(x) ((void)0)
#define _assert_msg_(x, ...) ((void)0)
#define _dbg_assert_(x) ((void)0)
#define _dbg_assert_msg_(x, ...) ((void)0)
