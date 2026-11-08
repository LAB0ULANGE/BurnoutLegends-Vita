#pragma once
#define PPSSPP_ARCH(x) (PPSSPP_ARCH_##x)
#if defined(_M_X64) || defined(__x86_64__)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_SSE2 1
#elif defined(__arm__) && defined(__ARM_NEON)
#define PPSSPP_ARCH_ARM_NEON 1
#endif
#ifndef PPSSPP_ARCH_AMD64
#define PPSSPP_ARCH_AMD64 0
#endif
#ifndef PPSSPP_ARCH_SSE2
#define PPSSPP_ARCH_SSE2 0
#endif
#ifndef PPSSPP_ARCH_ARM_NEON
#define PPSSPP_ARCH_ARM_NEON 0
#endif
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_ARM64 0
