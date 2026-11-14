// Hot game functions rewritten as host code (tools/patch_native.py puts a
// call at the function's first block; a false return runs the translated
// code instead). Each one does the same float operations in the same order
// as the game's code, so the results are bit-identical; only the scratch
// stores to the game's stack frame (dead after the return) are skipped.
//
// PC: PSPRECOMP_BURNOUT_NATIVE_CHECK runs the translated code too and
// compares the return value with the host one at the function's end;
// PSPRECOMP_BURNOUT_NO_NATIVE turns the host versions off.
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

#if defined(__vita__)
constexpr bool kCheck = false;
// ux0:data/burnout/no_native.txt: the game's own code everywhere (A/B
// measurements in one build).
const bool kNative = [] {
    std::FILE *f = std::fopen("ux0:data/burnout/no_native.txt", "rb");
    if (f != nullptr) std::fclose(f);
    return f == nullptr;
}();
#else
const bool kCheck = std::getenv("PSPRECOMP_BURNOUT_NATIVE_CHECK") != nullptr;
const bool kNative = std::getenv("PSPRECOMP_BURNOUT_NO_NATIVE") == nullptr;
#endif

struct Check {
    const char *name;
    std::uint32_t expected{};
    bool pending{};
    std::uint64_t calls{}, mismatches{};
};

Check g_08A3FCD4{"08A3FCD4 box visibility"};

void report(Check &check, std::uint32_t actual) {
    if (!check.pending) return;
    check.pending = false;
    ++check.calls;
    if (actual != check.expected) {
        ++check.mismatches;
        if (check.mismatches <= 20u)
            std::fprintf(stderr, "[native] %s: game %u, host %u (call %llu)\n", check.name, actual, check.expected,
                         static_cast<unsigned long long>(check.calls));
    }
    if ((check.calls & (check.calls - 1u)) == 0u && check.calls >= 1024u)
        std::fprintf(stderr, "[native] %s: %llu calls checked, %llu mismatches\n", check.name,
                     static_cast<unsigned long long>(check.calls), static_cast<unsigned long long>(check.mismatches));
}

float word(const std::uint8_t *p, std::uint32_t offset) noexcept {
    float value;
    std::memcpy(&value, p + offset, 4u);
    return value;
}

// 0x08A3FCD4 (continues at 0x08A40000): is a box in view?
// a0: a 4x4 matrix at +32 (rows 16 bytes apart) and two planes at +0/+16
// (x, y, z, distance); a1: the box's 8 corners (16 bytes each, w unused).
// v0: 0 outside, 1 partly in view, 2 inside.
std::uint32_t box_visibility(const std::uint8_t *m, const std::uint8_t *corners) noexcept {
    float row[4][4];
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 4; ++j) row[k][j] = word(m, 32u + 16u * k + 4u * j);
    std::uint32_t all = 15u;
    bool inside = true;
    for (std::uint32_t i = 0; i < 8u; ++i) {
        const std::uint8_t *p = corners + 16u * i;
        const float x = word(p, 0u), y = word(p, 4u), z = word(p, 8u);
        std::uint32_t mask = 0u;
        for (int j = 0; j < 4; ++j) {
            float a = row[0][j] * x;
            const float b = row[1][j] * y;
            a = a + b;
            const float c = row[2][j] * z;
            a = a + c;
            a = a - row[3][j];
            if (!(a <= 0.0f)) mask |= 1u << j;
        }
        if (mask != 0u) inside = false;
        all &= mask;
    }
    if (all != 0u) return 0u;
    for (std::uint32_t k = 0; k < 2u; ++k) {
        const std::uint8_t *plane = m + 16u * k;
        const float nx = word(plane, 0u), ny = word(plane, 4u), nz = word(plane, 8u), d = word(plane, 12u);
        bool all_out = true;
        for (std::uint32_t i = 0; i < 8u; ++i) {
            const std::uint8_t *p = corners + 16u * i;
            // VDOT.T as the runtime does it: lanes in order, then the zero fourth lane.
            float dot = word(p, 0u) * nx;
            const float dy = word(p, 4u) * ny;
            dot = dot + dy;
            const float dz = word(p, 8u) * nz;
            dot = dot + dz;
            const float zero = 0.0f * 0.0f;
            dot = dot + zero;
            const float v = dot - d;
            if (v <= 0.0f) all_out = false;
            else inside = false;
        }
        if (all_out) return 0u;
    }
    return inside ? 2u : 1u;
}

} // namespace

bool burnout_native_08A3FCD4(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    if (!kNative) return false;
    const std::uint8_t *m = memory.raw_pointer(ctx.gpr[4], 96u);
    const std::uint8_t *corners = memory.raw_pointer(ctx.gpr[5], 128u);
    if (m == nullptr || corners == nullptr) return false;
    const std::uint32_t result = box_visibility(m, corners);
    if (kCheck) {
        g_08A3FCD4.expected = result;
        g_08A3FCD4.pending = true;
        return false;  // the game's code runs and its result is compared
    }
    ctx.gpr[2] = result;
    return true;
}

void burnout_native_08A3FCD4_end(std::uint32_t v0) {
    if (kCheck) report(g_08A3FCD4, v0);
}

// 0x08A99388 __udivdi3 and 0x08A998FC __umoddi3 (libgcc, statically linked):
// a0:a1 / a2:a3 (low word first), unsigned; v0:v1 the quotient or the
// remainder. ~5.5 % of the game code on the Vita (2026-11-13 profile), called
// from 0x08A39C48's number to digits loop among others. A zero divisor runs
// the game's code (its own behaviour).
namespace {
Check g_udivdi3{"08A99388 __udivdi3"}, g_umoddi3{"08A998FC __umoddi3"};
std::uint32_t g_udivdi3_hi{}, g_umoddi3_hi{};

bool divide64(psprecomp::AllegrexContext &ctx, bool remainder, Check &check, std::uint32_t &expected_hi) {
    if (!kNative) return false;
    const std::uint64_t a = static_cast<std::uint64_t>(ctx.gpr[5]) << 32u | ctx.gpr[4];
    const std::uint64_t b = static_cast<std::uint64_t>(ctx.gpr[7]) << 32u | ctx.gpr[6];
    if (b == 0u) return false;
    const std::uint64_t result = remainder ? a % b : a / b;
    if (kCheck) {
        check.expected = static_cast<std::uint32_t>(result);
        expected_hi = static_cast<std::uint32_t>(result >> 32u);
        check.pending = true;
        return false;
    }
    ctx.gpr[2] = static_cast<std::uint32_t>(result);
    ctx.gpr[3] = static_cast<std::uint32_t>(result >> 32u);
    return true;
}

void divide64_end(Check &check, std::uint32_t expected_hi, std::uint32_t v0, std::uint32_t v1) {
    if (!kCheck || !check.pending) return;
    // Both words: a high-word mismatch shows as a low-word one.
    report(check, v1 != expected_hi ? ~check.expected : v0);
}
} // namespace

bool burnout_native_08A99388(psprecomp::GuestMemory &, psprecomp::AllegrexContext &ctx) {
    return divide64(ctx, false, g_udivdi3, g_udivdi3_hi);
}
void burnout_native_08A99388_end(std::uint32_t v0, std::uint32_t v1) { divide64_end(g_udivdi3, g_udivdi3_hi, v0, v1); }
bool burnout_native_08A998FC(psprecomp::GuestMemory &, psprecomp::AllegrexContext &ctx) {
    return divide64(ctx, true, g_umoddi3, g_umoddi3_hi);
}
void burnout_native_08A998FC_end(std::uint32_t v0, std::uint32_t v1) { divide64_end(g_umoddi3, g_umoddi3_hi, v0, v1); }
