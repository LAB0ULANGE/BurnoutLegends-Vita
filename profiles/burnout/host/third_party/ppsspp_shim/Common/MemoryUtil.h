#pragma once
#include <cstdlib>
#if defined(_MSC_VER)
#include <malloc.h>
inline void *AllocateAlignedMemory(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
inline void FreeAlignedMemory(void *p) { _aligned_free(p); }
#else
#include <malloc.h>
inline void *AllocateAlignedMemory(size_t size, size_t alignment) { return memalign(alignment, size); }
inline void FreeAlignedMemory(void *p) { free(p); }
#endif
