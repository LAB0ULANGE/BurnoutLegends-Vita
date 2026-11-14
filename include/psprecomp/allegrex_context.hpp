#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace psprecomp {

#if defined(_MSC_VER)
#define PSPRECOMP_CONTEXT_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PSPRECOMP_CONTEXT_FORCEINLINE inline __attribute__((always_inline))
#else
#define PSPRECOMP_CONTEXT_FORCEINLINE inline
#endif

// Transcendentals go through double so the float result does not depend on the
// host libm (MSVC and newlib sinf differ in the last ulp, which made PC and Vita
// runs drift apart once the race physics starts).
inline float vfpu_sin(float x) noexcept { return static_cast<float>(std::sin(static_cast<double>(x))); }
inline float vfpu_cos(float x) noexcept { return static_cast<float>(std::cos(static_cast<double>(x))); }
inline float vfpu_asin(float x) noexcept { return static_cast<float>(std::asin(static_cast<double>(x))); }
inline float vfpu_exp2(float x) noexcept { return static_cast<float>(std::exp2(static_cast<double>(x))); }
inline float vfpu_log2(float x) noexcept { return static_cast<float>(std::log2(static_cast<double>(x))); }

// MUL.S: Allegrex returns the default NaN 0x7FC00000 for inf * 0. ARM VFP does
// that natively; x86 SSE returns 0xFFC00000, so only x86 needs the check.
PSPRECOMP_CONTEXT_FORCEINLINE float fpu_mul(float fs, float ft) noexcept {
#if defined(__arm__) || defined(__aarch64__)
    return fs * ft;
#else
    if ((std::isinf(fs) && ft == 0.0f) || (std::isinf(ft) && fs == 0.0f)) return std::bit_cast<float>(0x7FC00000u);
    return fs * ft;
#endif
}

struct alignas(16) AllegrexContext {
    std::array<std::uint32_t, 32> gpr{};
    std::uint32_t hi{};
    std::uint32_t lo{};
    std::uint32_t pc{};
    std::array<float, 32> fpr{};
    std::uint32_t fcr31{};

    // VFPU is represented as 128 scalar lanes for now. The physical PSP
    // register views overlap; the final lowering layer will provide S/V/M views.
    std::array<float, 128> vfpu{};
    std::array<std::uint32_t, 16> vfpu_ctrl{};


    [[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t fpr_bits(std::uint32_t index) const noexcept {
        return index < fpr.size() ? std::bit_cast<std::uint32_t>(fpr[index]) : 0u;
    }

    PSPRECOMP_CONTEXT_FORCEINLINE void set_fpr_bits(std::uint32_t index, std::uint32_t value) noexcept {
        if (index < fpr.size()) {
            fpr[index] = std::bit_cast<float>(value);
        }
    }

    [[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE bool fpu_condition() const noexcept {
        return (fcr31 & (1u << 23u)) != 0u;
    }

    PSPRECOMP_CONTEXT_FORCEINLINE void set_fpu_condition(bool value) noexcept {
        if (value) fcr31 |= (1u << 23u);
        else fcr31 &= ~(1u << 23u);
    }

    static std::int32_t clamp_fpu_word(double value) noexcept {
        constexpr double min_value = static_cast<double>(std::numeric_limits<std::int32_t>::min());
        constexpr double max_value = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        if (value <= min_value) return std::numeric_limits<std::int32_t>::min();
        if (value >= max_value) return std::numeric_limits<std::int32_t>::max();
        return static_cast<std::int32_t>(value);
    }

    static double round_ties_to_even(double value) noexcept {
        const double lower = std::floor(value);
        const double fraction = value - lower;
        if (fraction < 0.5) return lower;
        if (fraction > 0.5) return lower + 1.0;
        const auto integral = static_cast<std::int64_t>(lower);
        return (integral & 1LL) == 0LL ? lower : lower + 1.0;
    }

    // fmin(1, fmax(low, value)) as newlib computes it (NaN gives low; -0.0
    // stays), without the library calls ARMv7 makes for fminf/fmaxf.
    static PSPRECOMP_CONTEXT_FORCEINLINE float vfpu_clamp(float value, float low) noexcept {
        const float raised = std::isnan(value) ? low : (low > value ? low : value);
        return 1.0f < raised ? 1.0f : raised;
    }

    // VF2IN/VF2IZ/VF2IU/VF2ID lane conversion (Mode 16..19) of an already
    // scaled value: NaN gives INT_MAX, out-of-range values saturate.
    template <std::uint32_t Mode>
    [[nodiscard]] static PSPRECOMP_CONTEXT_FORCEINLINE std::int32_t vfpu_float_to_int(float scaled) noexcept {
        if (std::isnan(scaled)) return std::numeric_limits<std::int32_t>::max();
#if defined(__arm__)
        if constexpr (Mode == 17u) {
            // VCVT truncates and saturates exactly like the double path below.
            float converted;
            __asm__("vcvt.s32.f32 %0, %1" : "=t"(converted) : "t"(scaled));
            return std::bit_cast<std::int32_t>(converted);
        } else if constexpr (Mode == 16u) {
            // VCVTR rounds with the FPSCR mode (round to nearest, ties to
            // even: never changed here) and saturates like the path below.
            float converted;
            __asm__("vcvtr.s32.f32 %0, %1" : "=t"(converted) : "t"(scaled));
            return std::bit_cast<std::int32_t>(converted);
        } else {
            // Ceil/floor: the truncation moved by one where it went the wrong
            // way (exact: a float that large is already an integer; the
            // saturated ends stay).
            float truncated;
            __asm__("vcvt.s32.f32 %0, %1" : "=t"(truncated) : "t"(scaled));
            std::int32_t value = std::bit_cast<std::int32_t>(truncated);
            if constexpr (Mode == 18u) {
                if (static_cast<float>(value) < scaled && value != std::numeric_limits<std::int32_t>::max()) ++value;
            } else {
                if (static_cast<float>(value) > scaled && value != std::numeric_limits<std::int32_t>::min()) --value;
            }
            return value;
        }
#endif
        const double value = static_cast<double>(scaled);
        if (value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            return std::numeric_limits<std::int32_t>::max();
        if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min()))
            return std::numeric_limits<std::int32_t>::min();
        if constexpr (Mode == 16u) return static_cast<std::int32_t>(round_ties_to_even(value));
        else if constexpr (Mode == 17u) return static_cast<std::int32_t>(std::trunc(value));
        else if constexpr (Mode == 18u) return static_cast<std::int32_t>(std::ceil(value));
        else return static_cast<std::int32_t>(std::floor(value));
    }

    [[nodiscard]] std::uint32_t fpu_float_to_word(float value, std::uint32_t mode) const noexcept {
        if (!std::isfinite(value)) {
            const std::int32_t result = std::isinf(value) && std::signbit(value)
                ? std::numeric_limits<std::int32_t>::min()
                : std::numeric_limits<std::int32_t>::max();
            return static_cast<std::uint32_t>(result);
        }

        double converted{};
        switch (mode) {
        case 0u: converted = std::floor(static_cast<double>(value) + 0.5); break; // ROUND.W.S
        case 1u: converted = std::trunc(static_cast<double>(value)); break;       // TRUNC.W.S
        case 2u: converted = std::ceil(static_cast<double>(value)); break;        // CEIL.W.S
        case 3u: converted = std::floor(static_cast<double>(value)); break;       // FLOOR.W.S
        default:
            switch (fcr31 & 3u) {
            case 0u: converted = round_ties_to_even(static_cast<double>(value)); break;
            case 1u: converted = std::trunc(static_cast<double>(value)); break;
            case 2u: converted = std::ceil(static_cast<double>(value)); break;
            default: converted = std::floor(static_cast<double>(value)); break;
            }
            break;
        }
        return static_cast<std::uint32_t>(clamp_fpu_word(converted));
    }


    // Generated ROUND/TRUNC/CEIL/FLOOR instructions carry a literal conversion
    // mode. Make it a template parameter so the hot AOT path has no mode switch;
    // TRUNC (a common generated-code case) also avoids a redundant std::trunc.
    template <std::uint32_t Mode>
    [[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t fpu_float_to_word_ct(float value) const noexcept {
        static_assert(Mode <= 3u);
#if defined(__arm__)
        if constexpr (Mode == 1u) {
            // VCVT truncates and saturates like TRUNC.W.S (infinities included);
            // only NaN differs (0 on ARM, INT_MAX on the PSP).
            if (std::isnan(value)) return static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            float converted;
            __asm__("vcvt.s32.f32 %0, %1" : "=t"(converted) : "t"(value));
            return std::bit_cast<std::uint32_t>(converted);
        }
#endif
        if (!std::isfinite(value)) {
            const std::int32_t result = std::isinf(value) && std::signbit(value)
                ? std::numeric_limits<std::int32_t>::min()
                : std::numeric_limits<std::int32_t>::max();
            return static_cast<std::uint32_t>(result);
        }
        const double input = static_cast<double>(value);
        if constexpr (Mode == 1u) {
            constexpr double min_value = static_cast<double>(std::numeric_limits<std::int32_t>::min());
            constexpr double max_value = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            if (input <= min_value) return static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::min());
            if (input >= max_value) return static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(input));
        } else if constexpr (Mode == 0u) {
            return static_cast<std::uint32_t>(clamp_fpu_word(std::floor(input + 0.5)));
        } else if constexpr (Mode == 2u) {
            return static_cast<std::uint32_t>(clamp_fpu_word(std::ceil(input)));
        } else {
            return static_cast<std::uint32_t>(clamp_fpu_word(std::floor(input)));
        }
    }

    static constexpr std::size_t vfpu_scalar_index(std::uint32_t scalar_register) noexcept {
        // PSP scalar register encoding is 0YYMMMXX.  The storage below uses
        // contiguous 4x4 matrices: matrix * 16 + Y * 4 + X.
        const std::uint32_t matrix = (scalar_register >> 2u) & 7u;
        const std::uint32_t x = (scalar_register >> 5u) & 3u;
        const std::uint32_t y = scalar_register & 3u;
        return static_cast<std::size_t>(matrix * 16u + y * 4u + x);
    }

    PSPRECOMP_CONTEXT_FORCEINLINE void set_vfpu_scalar_bits(std::uint32_t scalar_register, std::uint32_t value) noexcept {
        if (scalar_register < 128u) {
            vfpu[vfpu_scalar_index(scalar_register)] = std::bit_cast<float>(value);
        } else if (scalar_register < 144u) {
            vfpu_ctrl[scalar_register - 128u] = value;
        }
    }

    [[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t vfpu_scalar_bits(std::uint32_t scalar_register) const noexcept {
        if (scalar_register < 128u) {
            return std::bit_cast<std::uint32_t>(vfpu[vfpu_scalar_index(scalar_register)]);
        }
        if (scalar_register < 144u) {
            return vfpu_ctrl[scalar_register - 128u];
        }
        return 0u;
    }

    // Generated MTV/MFV/LVS/SVS always carry a literal encoded scalar register.
    // Make that fact visible to the host compiler so both the register-class
    // branch and the PSP VFPU lane shuffle disappear from the AOT hot path.
    template <std::uint32_t ScalarRegister>
    PSPRECOMP_CONTEXT_FORCEINLINE void set_vfpu_scalar_bits_ct(std::uint32_t value) noexcept {
        static_assert(ScalarRegister < 144u);
        if constexpr (ScalarRegister < 128u) {
            constexpr std::size_t index = vfpu_scalar_index(ScalarRegister);
            vfpu[index] = std::bit_cast<float>(value);
        } else {
            vfpu_ctrl[ScalarRegister - 128u] = value;
        }
    }

    template <std::uint32_t ScalarRegister>
    [[nodiscard]] PSPRECOMP_CONTEXT_FORCEINLINE std::uint32_t vfpu_scalar_bits_ct() const noexcept {
        static_assert(ScalarRegister < 144u);
        if constexpr (ScalarRegister < 128u) {
            constexpr std::size_t index = vfpu_scalar_index(ScalarRegister);
            return std::bit_cast<std::uint32_t>(vfpu[index]);
        } else {
            return vfpu_ctrl[ScalarRegister - 128u];
        }
    }

    // AOT compile-time VFPU lane access. Generated code always
    // knows the encoded vector register and vector length.  The old generic
    // helpers recomputed row/column/transpose and ran tiny loops at runtime;
    // on giant generated units MSVC/Clang often kept the prefix helpers as
    // real calls.  These templates turn the lane mapping into constants and
    // fully unroll 1..4 lanes at the call site.
    template <std::uint32_t VectorRegister, std::uint32_t Length>
    PSPRECOMP_CONTEXT_FORCEINLINE void read_vfpu_vector_ct(float *destination) const noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        if constexpr (Length == 1u) {
            constexpr std::size_t index = vfpu_scalar_index(VectorRegister & 0x7Fu);
            destination[0] = vfpu[index];
        } else {
            constexpr std::uint32_t row = Length == 3u ? ((VectorRegister >> 6u) & 1u)
                                                        : ((VectorRegister >> 5u) & 2u);
            constexpr bool transpose = ((VectorRegister >> 5u) & 1u) != 0u;
            constexpr std::uint32_t matrix_base = ((VectorRegister << 2u) & 0x70u);
            constexpr std::uint32_t column = VectorRegister & 3u;
            if constexpr (transpose) {
                constexpr std::uint32_t base = matrix_base + column;
                destination[0] = vfpu[base + ((row + 0u) & 3u) * 4u];
                if constexpr (Length >= 2u) destination[1] = vfpu[base + ((row + 1u) & 3u) * 4u];
                if constexpr (Length >= 3u) destination[2] = vfpu[base + ((row + 2u) & 3u) * 4u];
                if constexpr (Length >= 4u) destination[3] = vfpu[base + ((row + 3u) & 3u) * 4u];
            } else {
                constexpr std::uint32_t base = matrix_base + column * 4u;
                destination[0] = vfpu[base + ((row + 0u) & 3u)];
                if constexpr (Length >= 2u) destination[1] = vfpu[base + ((row + 1u) & 3u)];
                if constexpr (Length >= 3u) destination[2] = vfpu[base + ((row + 2u) & 3u)];
                if constexpr (Length >= 4u) destination[3] = vfpu[base + ((row + 3u) & 3u)];
            }
        }
    }

    template <std::uint32_t Length, std::uint32_t ControlIndex>
    PSPRECOMP_CONTEXT_FORCEINLINE void apply_vfpu_source_prefix_ct(float *value) const noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        static_assert(ControlIndex < 2u);
        const std::uint32_t prefix = vfpu_ctrl[ControlIndex];
        if (prefix == 0xE4u) return;
        const float o0 = value[0];
        const float o1 = Length >= 2u ? value[1] : 0.0f;
        const float o2 = Length >= 3u ? value[2] : 0.0f;
        const float o3 = Length >= 4u ? value[3] : 0.0f;
        const float original[4]{o0, o1, o2, o3};
        static constexpr float constants[8] = {
            0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f,
        };
        auto lane_value = [&](std::uint32_t i) {
            const std::uint32_t lane = (prefix >> (i * 2u)) & 3u;
            const bool absolute = ((prefix >> (8u + i)) & 1u) != 0u;
            const bool use_constant = ((prefix >> (12u + i)) & 1u) != 0u;
            const bool negate = ((prefix >> (16u + i)) & 1u) != 0u;
            float out = use_constant ? constants[lane + (absolute ? 4u : 0u)]
                                     : (lane < Length ? original[lane] : 0.0f);
            if (!use_constant && absolute) out = std::fabs(out);
            if (negate) out = std::bit_cast<float>(std::bit_cast<std::uint32_t>(out) ^ 0x80000000u);
            return out;
        };
        value[0] = lane_value(0u);
        if constexpr (Length >= 2u) value[1] = lane_value(1u);
        if constexpr (Length >= 3u) value[2] = lane_value(2u);
        if constexpr (Length >= 4u) value[3] = lane_value(3u);
    }

    // apply_vfpu_source_prefix with a prefix the code generator knows (it
    // tracks the vpfxs/vpfxt values inside a block): every lane decision is a
    // constant, as are the forced prefixes the special instructions derive.
    template <std::uint32_t Prefix, std::uint32_t Length>
    PSPRECOMP_CONTEXT_FORCEINLINE static void apply_vfpu_prefix_const(float *value) noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        if constexpr (Prefix != 0xE4u) {
            const float original[4]{value[0], Length >= 2u ? value[1] : 0.0f, Length >= 3u ? value[2] : 0.0f,
                                    Length >= 4u ? value[3] : 0.0f};
            value[0] = vfpu_prefix_lane<Prefix, Length, 0u>(original);
            if constexpr (Length >= 2u) value[1] = vfpu_prefix_lane<Prefix, Length, 1u>(original);
            if constexpr (Length >= 3u) value[2] = vfpu_prefix_lane<Prefix, Length, 2u>(original);
            if constexpr (Length >= 4u) value[3] = vfpu_prefix_lane<Prefix, Length, 3u>(original);
        }
    }

    template <std::uint32_t Prefix, std::uint32_t Length, std::uint32_t I>
    PSPRECOMP_CONTEXT_FORCEINLINE static float vfpu_prefix_lane(const float *original) noexcept {
        constexpr float constants[8] = {
            0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f,
        };
        constexpr std::uint32_t lane = (Prefix >> (I * 2u)) & 3u;
        constexpr bool absolute = ((Prefix >> (8u + I)) & 1u) != 0u;
        constexpr bool use_constant = ((Prefix >> (12u + I)) & 1u) != 0u;
        constexpr bool negate = ((Prefix >> (16u + I)) & 1u) != 0u;
        float out;
        if constexpr (use_constant) out = constants[lane + (absolute ? 4u : 0u)];
        else if constexpr (lane < Length) out = absolute ? std::fabs(original[lane]) : original[lane];
        else out = 0.0f;
        if constexpr (negate) out = std::bit_cast<float>(std::bit_cast<std::uint32_t>(out) ^ 0x80000000u);
        return out;
    }

    template <std::uint32_t VectorRegister, std::uint32_t Length, std::uint32_t Prefix>
    PSPRECOMP_CONTEXT_FORCEINLINE void read_vfpu_vector_prefix_const(float *destination) const noexcept {
        read_vfpu_vector_ct<VectorRegister, Length>(destination);
        apply_vfpu_prefix_const<Prefix, Length>(destination);
    }

    template <std::uint32_t VectorRegister, std::uint32_t Length, std::uint32_t DestinationPrefix, std::uint32_t I>
    PSPRECOMP_CONTEXT_FORCEINLINE void write_vfpu_lane_dprefix_const(float value) noexcept {
        constexpr std::uint32_t saturation = (DestinationPrefix >> (I * 2u)) & 3u;
        if constexpr (saturation == 1u) value = vfpu_clamp(value, 0.0f);
        else if constexpr (saturation == 3u) value = vfpu_clamp(value, -1.0f);
        if constexpr (((DestinationPrefix >> (8u + I)) & 1u) == 0u) {
            constexpr std::size_t index = vfpu_vector_lane_index(VectorRegister, Length, I);
            vfpu[index] = value;
        }
    }

    // write_vfpu_vector_with_destination_prefix with a known D prefix; it
    // consumes the prefixes as the generic helper does.
    template <std::uint32_t VectorRegister, std::uint32_t Length, std::uint32_t DestinationPrefix>
    PSPRECOMP_CONTEXT_FORCEINLINE void write_vfpu_vector_dprefix_const(const float *source) noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        if constexpr (DestinationPrefix == 0u) {
            write_vfpu_vector_ct<VectorRegister, Length>(source);
        } else {
            write_vfpu_lane_dprefix_const<VectorRegister, Length, DestinationPrefix, 0u>(source[0]);
            if constexpr (Length >= 2u) write_vfpu_lane_dprefix_const<VectorRegister, Length, DestinationPrefix, 1u>(source[1]);
            if constexpr (Length >= 3u) write_vfpu_lane_dprefix_const<VectorRegister, Length, DestinationPrefix, 2u>(source[2]);
            if constexpr (Length >= 4u) write_vfpu_lane_dprefix_const<VectorRegister, Length, DestinationPrefix, 3u>(source[3]);
        }
        eat_vfpu_prefixes();
    }

    template <std::uint32_t VectorRegister, std::uint32_t Length, std::uint32_t ControlIndex>
    PSPRECOMP_CONTEXT_FORCEINLINE void read_vfpu_vector_with_source_prefix_ct(float *destination) const noexcept {
        read_vfpu_vector_ct<VectorRegister, Length>(destination);
        apply_vfpu_source_prefix_ct<Length, ControlIndex>(destination);
    }

    template <std::uint32_t VectorRegister, std::uint32_t Length>
    PSPRECOMP_CONTEXT_FORCEINLINE void write_vfpu_vector_ct(const float *source) noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        if constexpr (Length == 1u) {
            constexpr std::size_t index = vfpu_scalar_index(VectorRegister & 0x7Fu);
            vfpu[index] = source[0];
        } else {
            constexpr std::uint32_t row = Length == 3u ? ((VectorRegister >> 6u) & 1u)
                                                        : ((VectorRegister >> 5u) & 2u);
            constexpr bool transpose = ((VectorRegister >> 5u) & 1u) != 0u;
            constexpr std::uint32_t matrix_base = ((VectorRegister << 2u) & 0x70u);
            constexpr std::uint32_t column = VectorRegister & 3u;
            if constexpr (transpose) {
                constexpr std::uint32_t base = matrix_base + column;
                vfpu[base + ((row + 0u) & 3u) * 4u] = source[0];
                if constexpr (Length >= 2u) vfpu[base + ((row + 1u) & 3u) * 4u] = source[1];
                if constexpr (Length >= 3u) vfpu[base + ((row + 2u) & 3u) * 4u] = source[2];
                if constexpr (Length >= 4u) vfpu[base + ((row + 3u) & 3u) * 4u] = source[3];
            } else {
                constexpr std::uint32_t base = matrix_base + column * 4u;
                vfpu[base + ((row + 0u) & 3u)] = source[0];
                if constexpr (Length >= 2u) vfpu[base + ((row + 1u) & 3u)] = source[1];
                if constexpr (Length >= 3u) vfpu[base + ((row + 2u) & 3u)] = source[2];
                if constexpr (Length >= 4u) vfpu[base + ((row + 3u) & 3u)] = source[3];
            }
        }
    }

    template <std::uint32_t VectorRegister, std::uint32_t Length>
    PSPRECOMP_CONTEXT_FORCEINLINE void write_vfpu_vector_with_destination_prefix_ct(const float *source) noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        const std::uint32_t destination_prefix = vfpu_ctrl[2];
        float value[4]{source[0], Length >= 2u ? source[1] : 0.0f,
                       Length >= 3u ? source[2] : 0.0f, Length >= 4u ? source[3] : 0.0f};
        // No saturation and no write mask: the common case is a plain lane store.
        // Keeping it free of the per-lane saturation code matters because GCC
        // leaves the lambda below out of line inside the giant generated units.
        if (destination_prefix == 0u) [[likely]] {
            write_vfpu_vector_ct<VectorRegister, Length>(value);
            eat_vfpu_prefixes();
            return;
        }
        auto saturate_lane = [&](std::uint32_t i) {
            const std::uint32_t saturation = (destination_prefix >> (i * 2u)) & 3u;
            if (saturation == 1u) value[i] = vfpu_clamp(value[i], 0.0f);
            else if (saturation == 3u) value[i] = vfpu_clamp(value[i], -1.0f);
        };
        saturate_lane(0u);
        if constexpr (Length >= 2u) saturate_lane(1u);
        if constexpr (Length >= 3u) saturate_lane(2u);
        if constexpr (Length >= 4u) saturate_lane(3u);

        if ((destination_prefix & (1u << 8u)) == 0u) {
            constexpr std::size_t i0 = vfpu_vector_lane_index(VectorRegister, Length, 0u);
            vfpu[i0] = value[0];
        }
        if constexpr (Length >= 2u) if ((destination_prefix & (1u << 9u)) == 0u) {
            constexpr std::size_t i1 = vfpu_vector_lane_index(VectorRegister, Length, 1u);
            vfpu[i1] = value[1];
        }
        if constexpr (Length >= 3u) if ((destination_prefix & (1u << 10u)) == 0u) {
            constexpr std::size_t i2 = vfpu_vector_lane_index(VectorRegister, Length, 2u);
            vfpu[i2] = value[2];
        }
        if constexpr (Length >= 4u) if ((destination_prefix & (1u << 11u)) == 0u) {
            constexpr std::size_t i3 = vfpu_vector_lane_index(VectorRegister, Length, 3u);
            vfpu[i3] = value[3];
        }
        eat_vfpu_prefixes();
    }

    void read_vfpu_vector(float *destination, std::uint32_t vector_register, std::uint32_t length) const noexcept {
        if (length == 1u) {
            destination[0] = vfpu[vfpu_scalar_index(vector_register & 0x7Fu)];
            return;
        }
        const std::uint32_t row = length == 3u ? ((vector_register >> 6u) & 1u)
                                               : ((vector_register >> 5u) & 2u);
        const bool transpose = ((vector_register >> 5u) & 1u) != 0u;
        const std::uint32_t matrix_base = ((vector_register << 2u) & 0x70u);
        const std::uint32_t column = vector_register & 3u;
        if (transpose) {
            const std::uint32_t base = matrix_base + column;
            for (std::uint32_t i = 0; i < length; ++i) {
                destination[i] = vfpu[base + ((row + i) & 3u) * 4u];
            }
        } else {
            const std::uint32_t base = matrix_base + column * 4u;
            for (std::uint32_t i = 0; i < length; ++i) {
                destination[i] = vfpu[base + ((row + i) & 3u)];
            }
        }
    }

    void apply_vfpu_source_prefix(float *value, std::uint32_t length, std::uint32_t control_index) const noexcept {
        if (control_index >= 2u || length == 0u) return;
        const std::uint32_t prefix = vfpu_ctrl[control_index];
        if (prefix == 0xE4u) return;

        float original[4]{};
        for (std::uint32_t i = 0; i < length && i < 4u; ++i) original[i] = value[i];
        static constexpr float constants[8] = {
            0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f,
        };
        for (std::uint32_t i = 0; i < length && i < 4u; ++i) {
            const std::uint32_t lane = (prefix >> (i * 2u)) & 3u;
            const bool absolute = ((prefix >> (8u + i)) & 1u) != 0u;
            const bool use_constant = ((prefix >> (12u + i)) & 1u) != 0u;
            const bool negate = ((prefix >> (16u + i)) & 1u) != 0u;
            if (use_constant) {
                value[i] = constants[lane + (absolute ? 4u : 0u)];
            } else {
                value[i] = lane < length ? original[lane] : 0.0f;
                if (absolute) value[i] = std::fabs(value[i]);
            }
            if (negate) {
                value[i] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(value[i]) ^ 0x80000000u);
            }
        }
    }

    void read_vfpu_vector_with_source_prefix(float *destination, std::uint32_t vector_register,
                                             std::uint32_t length, std::uint32_t control_index) const noexcept {
        read_vfpu_vector(destination, vector_register, length);
        apply_vfpu_source_prefix(destination, length, control_index);
    }

    void write_vfpu_vector(const float *source, std::uint32_t vector_register, std::uint32_t length) noexcept {
        if (length == 1u) {
            vfpu[vfpu_scalar_index(vector_register & 0x7Fu)] = source[0];
            return;
        }
        const std::uint32_t row = length == 3u ? ((vector_register >> 6u) & 1u)
                                               : ((vector_register >> 5u) & 2u);
        const bool transpose = ((vector_register >> 5u) & 1u) != 0u;
        const std::uint32_t matrix_base = ((vector_register << 2u) & 0x70u);
        const std::uint32_t column = vector_register & 3u;
        if (transpose) {
            const std::uint32_t base = matrix_base + column;
            for (std::uint32_t i = 0; i < length; ++i) {
                vfpu[base + ((row + i) & 3u) * 4u] = source[i];
            }
        } else {
            const std::uint32_t base = matrix_base + column * 4u;
            for (std::uint32_t i = 0; i < length; ++i) {
                vfpu[base + ((row + i) & 3u)] = source[i];
            }
        }
    }

    void write_vfpu_vector_with_destination_prefix(const float *source, std::uint32_t vector_register,
                                                   std::uint32_t length) noexcept {
        float value[4]{};
        const std::uint32_t destination_prefix = vfpu_ctrl[2];
        if (destination_prefix == 0u && length >= 1u && length <= 4u) [[likely]] {
            for (std::uint32_t i = 0; i < length; ++i) value[i] = source[i];
            write_vfpu_vector(value, vector_register, length);
            eat_vfpu_prefixes();
            return;
        }
        for (std::uint32_t i = 0; i < length && i < 4u; ++i) {
            value[i] = source[i];
            const std::uint32_t saturation = (destination_prefix >> (i * 2u)) & 3u;
            if (saturation == 1u) {
                value[i] = vfpu_clamp(value[i], 0.0f);
            } else if (saturation == 3u) {
                value[i] = vfpu_clamp(value[i], -1.0f);
            }
        }

        const std::uint32_t row = length == 3u ? ((vector_register >> 6u) & 1u)
                                               : ((vector_register >> 5u) & 2u);
        const bool transpose = ((vector_register >> 5u) & 1u) != 0u;
        const std::uint32_t matrix_base = ((vector_register << 2u) & 0x70u);
        const std::uint32_t column = vector_register & 3u;
        for (std::uint32_t i = 0; i < length && i < 4u; ++i) {
            // Destination-prefix mask bit 1 preserves the old lane.
            if (((destination_prefix >> (8u + i)) & 1u) != 0u) {
                continue;
            }
            const std::size_t index = length == 1u
                ? vfpu_scalar_index(vector_register & 0x7Fu)
                : (transpose
                    ? static_cast<std::size_t>(matrix_base + column + ((row + i) & 3u) * 4u)
                    : static_cast<std::size_t>(matrix_base + column * 4u + ((row + i) & 3u)));
            vfpu[index] = value[i];
        }
        eat_vfpu_prefixes();
    }

    [[nodiscard]] static constexpr std::size_t vfpu_vector_lane_index(
        std::uint32_t vector_register, std::uint32_t length, std::uint32_t lane) noexcept {
        if (length == 1u) return vfpu_scalar_index(vector_register & 0x7Fu);
        const std::uint32_t row = length == 3u ? ((vector_register >> 6u) & 1u)
                                               : ((vector_register >> 5u) & 2u);
        const bool transpose = ((vector_register >> 5u) & 1u) != 0u;
        const std::uint32_t matrix_base = ((vector_register << 2u) & 0x70u);
        const std::uint32_t column = vector_register & 3u;
        return transpose
            ? static_cast<std::size_t>(matrix_base + column + ((row + lane) & 3u) * 4u)
            : static_cast<std::size_t>(matrix_base + column * 4u + ((row + lane) & 3u));
    }

    [[nodiscard]] static constexpr std::uint32_t vfpu_expand_half_bits(std::uint16_t half) noexcept {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16u;
        const std::uint32_t exponent = (half >> 10u) & 0x1Fu;
        std::uint32_t mantissa = half & 0x03FFu;

        if (exponent == 0u) {
            if (mantissa == 0u) return sign;

            // Normalize a binary16 subnormal into binary32.  Exponent 113 is
            // the binary32 exponent corresponding to the smallest normal
            // binary16 value; each normalization shift decreases it by one.
            std::uint32_t float_exponent = 113u;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                --float_exponent;
            }
            mantissa &= 0x03FFu;
            return sign | (float_exponent << 23u) | (mantissa << 13u);
        }

        if (exponent == 31u) {
            // The PSP VFPU preserves the binary16 NaN payload in the low ten
            // binary32 mantissa bits for this conversion.
            return sign | 0x7F800000u | mantissa;
        }

        return sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }

    [[nodiscard]] static std::uint16_t vfpu_shrink_to_half_bits(float value) noexcept {
        // Integer-only binary32 -> binary16 conversion.  The VFPU rounds a
        // discarded exact half-way bit upward, so the rounding step is kept
        // explicit instead of depending on host floating-point state.
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16u) & 0x8000u);
        const std::uint32_t exponent = (bits >> 23u) & 0xFFu;
        const std::uint32_t fraction = bits & 0x007FFFFFu;

        if (exponent == 0xFFu) {
            if (fraction == 0u) return static_cast<std::uint16_t>(sign | 0x7C00u);
            // Preserve the low ten payload bits and force a quiet half NaN.
            return static_cast<std::uint16_t>(sign | 0x7E00u | (bits & 0x03FFu));
        }

        // Every binary32 subnormal is too small to survive as binary16.
        if (exponent == 0u) return sign;

        const std::int32_t unbiased = static_cast<std::int32_t>(exponent) - 127;
        if (unbiased > 15) return static_cast<std::uint16_t>(sign | 0x7C00u);

        const auto round_half_up = [](std::uint32_t value_bits, std::uint32_t shift) noexcept {
            if (shift == 0u) return value_bits;
            const std::uint32_t quotient = value_bits >> shift;
            const std::uint32_t remainder_mask = (std::uint32_t{1} << shift) - 1u;
            const std::uint32_t remainder = value_bits & remainder_mask;
            const std::uint32_t halfway = std::uint32_t{1} << (shift - 1u);
            return quotient + (remainder >= halfway ? 1u : 0u);
        };

        if (unbiased >= -14) {
            std::uint32_t half_exponent = static_cast<std::uint32_t>(unbiased + 15);
            std::uint32_t half_fraction = round_half_up(fraction, 13u);
            if (half_fraction == 0x0400u) {
                half_fraction = 0u;
                ++half_exponent;
                if (half_exponent >= 31u)
                    return static_cast<std::uint16_t>(sign | 0x7C00u);
            }
            return static_cast<std::uint16_t>(
                sign | (half_exponent << 10u) | half_fraction);
        }

        if (unbiased < -25) return sign;
        const std::uint32_t significand = 0x00800000u | fraction;
        const std::uint32_t shift = static_cast<std::uint32_t>(13 + (-14 - unbiased));
        const std::uint32_t half_fraction = round_half_up(significand, shift);
        if (half_fraction >= 0x0400u)
            return static_cast<std::uint16_t>(sign | 0x0400u);
        return static_cast<std::uint16_t>(sign | half_fraction);
    }

    void execute_vfpu_vf2h(std::uint32_t destination_register,
                            std::uint32_t source_register,
                            std::uint32_t source_length) noexcept {
        if (source_length == 0u || source_length > 4u) return;

        // VF2H applies S through a four-lane view, allowing prefix constants
        // to supply lanes that are not present in a short encoded vector.
        float source[4]{};
        read_vfpu_vector(source, source_register, source_length);
        apply_vfpu_source_prefix(source, 4u, 0u);

        std::uint32_t packed[2]{};
        packed[0] = static_cast<std::uint32_t>(vfpu_shrink_to_half_bits(source[0])) |
                    (static_cast<std::uint32_t>(vfpu_shrink_to_half_bits(source[1])) << 16u);
        const std::uint32_t destination_length = source_length <= 2u ? 1u : 2u;
        if (destination_length == 2u) {
            packed[1] = static_cast<std::uint32_t>(vfpu_shrink_to_half_bits(source[2])) |
                        (static_cast<std::uint32_t>(vfpu_shrink_to_half_bits(source[3])) << 16u);
        }

        float result[2]{
            std::bit_cast<float>(packed[0]),
            std::bit_cast<float>(packed[1]),
        };
        write_vfpu_vector_with_destination_prefix(result, destination_register, destination_length);
    }

    void execute_vfpu_vh2f(std::uint32_t destination_register,
                            std::uint32_t source_register,
                            std::uint32_t source_length) noexcept {
        if (source_length == 0u || source_length > 4u) return;

        float source[4]{};
        read_vfpu_vector(source, source_register, source_length);
        apply_vfpu_source_prefix(source, source_length, 0u);

        const std::uint32_t first_word = std::bit_cast<std::uint32_t>(source[0]);
        float result[4]{
            std::bit_cast<float>(vfpu_expand_half_bits(static_cast<std::uint16_t>(first_word))),
            std::bit_cast<float>(vfpu_expand_half_bits(static_cast<std::uint16_t>(first_word >> 16u))),
            0.0f,
            0.0f,
        };

        const std::uint32_t destination_length = source_length == 1u ? 2u : 4u;
        if (destination_length == 4u) {
            const std::uint32_t second_word = std::bit_cast<std::uint32_t>(source[1]);
            result[2] = std::bit_cast<float>(
                vfpu_expand_half_bits(static_cast<std::uint16_t>(second_word)));
            result[3] = std::bit_cast<float>(
                vfpu_expand_half_bits(static_cast<std::uint16_t>(second_word >> 16u)));
        }

        write_vfpu_vector_with_destination_prefix(result, destination_register, destination_length);
    }

    void execute_vfpu_vx2i(std::uint32_t destination_register,
                            std::uint32_t source_register,
                            std::uint32_t source_length,
                            std::uint32_t operation) noexcept {
        if (source_length == 0u || source_length > 4u || operation > 3u) return;

        float source[4]{};
        read_vfpu_vector(source, source_register, source_length);
        apply_vfpu_source_prefix(source, source_length, 0u);

        std::uint32_t result_bits[4]{};
        std::uint32_t destination_length = 4u;
        if (operation == 0u) { // VUC2I
            std::uint32_t value = std::bit_cast<std::uint32_t>(source[0]);
            for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
                result_bits[lane] = ((value & 0xFFu) * 0x01010101u) >> 1u;
                value >>= 8u;
            }
        } else if (operation == 1u) { // VC2I
            const std::uint32_t value = std::bit_cast<std::uint32_t>(source[0]);
            result_bits[0] = (value & 0x000000FFu) << 24u;
            result_bits[1] = (value & 0x0000FF00u) << 16u;
            result_bits[2] = (value & 0x00FF0000u) << 8u;
            result_bits[3] = value & 0xFF000000u;
        } else { // VUS2I / VS2I
            const std::uint32_t input_count = std::min(source_length, 2u);
            destination_length = source_length == 1u ? 2u : 4u;
            for (std::uint32_t lane = 0u; lane < input_count; ++lane) {
                const std::uint32_t value = std::bit_cast<std::uint32_t>(source[lane]);
                if (operation == 2u) {
                    result_bits[lane * 2u] = (value & 0x0000FFFFu) << 15u;
                    result_bits[lane * 2u + 1u] = (value & 0xFFFF0000u) >> 1u;
                } else {
                    result_bits[lane * 2u] = (value & 0x0000FFFFu) << 16u;
                    result_bits[lane * 2u + 1u] = value & 0xFFFF0000u;
                }
            }
        }

        float result[4]{};
        for (std::uint32_t lane = 0u; lane < destination_length; ++lane)
            result[lane] = std::bit_cast<float>(result_bits[lane]);
        write_vfpu_vector_with_destination_prefix(result, destination_register, destination_length);
    }

    // VI2UC/VI2C/VI2US/VI2S: the inverse of VX2I. Pack the high bits of 32-bit
    // integer lanes into bytes (quad -> single) or halfwords (pair -> single,
    // quad -> pair). The unsigned forms clamp negative lanes to zero.
    void execute_vfpu_vi2x(std::uint32_t destination_register,
                           std::uint32_t source_register,
                           std::uint32_t source_length,
                           std::uint32_t operation) noexcept {
        if (source_length == 0u || source_length > 4u || operation > 3u) return;

        float source[4]{};
        read_vfpu_vector(source, source_register, source_length);
        apply_vfpu_source_prefix(source, source_length, 0u);
        std::int32_t lanes[4]{};
        for (std::uint32_t lane = 0u; lane < source_length; ++lane)
            lanes[lane] = std::bit_cast<std::int32_t>(source[lane]);

        std::uint32_t result_bits[2]{};
        std::uint32_t destination_length = 1u;
        if (operation <= 1u) { // VI2UC / VI2C
            std::uint32_t packed = 0u;
            for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
                const std::int32_t value = lanes[lane];
                const std::uint32_t byte = operation == 0u
                    ? (value < 0 ? 0u : (static_cast<std::uint32_t>(value) >> 23u) & 0xFFu)
                    : (static_cast<std::uint32_t>(value) >> 24u) & 0xFFu;
                packed |= byte << (lane * 8u);
            }
            result_bits[0] = packed;
        } else { // VI2US / VI2S
            destination_length = source_length >= 4u ? 2u : 1u;
            for (std::uint32_t out_lane = 0u; out_lane < destination_length; ++out_lane) {
                std::uint32_t packed = 0u;
                for (std::uint32_t half = 0u; half < 2u; ++half) {
                    const std::int32_t value = lanes[out_lane * 2u + half];
                    const std::uint32_t bits = operation == 2u
                        ? (value < 0 ? 0u : (static_cast<std::uint32_t>(value) >> 15u) & 0xFFFFu)
                        : (static_cast<std::uint32_t>(value) >> 16u) & 0xFFFFu;
                    packed |= bits << (half * 16u);
                }
                result_bits[out_lane] = packed;
            }
        }

        float result[4]{};
        for (std::uint32_t lane = 0u; lane < destination_length; ++lane)
            result[lane] = std::bit_cast<float>(result_bits[lane]);
        write_vfpu_vector_with_destination_prefix(result, destination_register, destination_length);
    }

    // DefaultPrefixes: codegen proved that S/T are 0xE4 and D is 0 (all already
    // consumed), so the prefix checks and the consume stores are skipped.
    template <std::uint32_t DestinationScalarRegister, std::uint32_t SourceRegister,
              std::uint32_t TargetRegister, std::uint32_t Length, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vdot_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        float source[4]{};
        float target[4]{};
        read_vfpu_vector_ct<SourceRegister, Length>(source);
        read_vfpu_vector_ct<TargetRegister, Length>(target);
        if constexpr (!DefaultPrefixes) {
            // VDOT prefix semantics use a four-lane view even for a shorter encoded vector.
            apply_vfpu_source_prefix_ct<4u, 0u>(source);
            apply_vfpu_source_prefix_ct<4u, 1u>(target);
        }
        const float result[1]{
            source[0] * target[0] + source[1] * target[1] +
            source[2] * target[2] + source[3] * target[3]
        };
        if constexpr (DefaultPrefixes) write_vfpu_vector_ct<DestinationScalarRegister, 1u>(result);
        else write_vfpu_vector_with_destination_prefix_ct<DestinationScalarRegister, 1u>(result);
    }

    void execute_vfpu_vdot(std::uint32_t destination_scalar_register,
                            std::uint32_t source_register, std::uint32_t target_register,
                            std::uint32_t length) noexcept {
        if (length == 0u || length > 4u) return;

        // VDOT initializes the lanes beyond the encoded vector size to zero,
        // then applies both source prefixes through a four-lane view.  This is
        // important because prefix constants may legally introduce values in
        // those otherwise-unused lanes.
        float source[4]{};
        float target[4]{};
        read_vfpu_vector(source, source_register, length);
        read_vfpu_vector(target, target_register, length);
        apply_vfpu_source_prefix(source, 4u, 0u);
        apply_vfpu_source_prefix(target, 4u, 1u);

        float result[1]{0.0f};
        for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
            result[0] += source[lane] * target[lane];
        }
        write_vfpu_vector_with_destination_prefix(result, destination_scalar_register, 1u);
    }

    void execute_vfpu_vhdp(std::uint32_t destination_scalar_register,
                           std::uint32_t source_register, std::uint32_t target_register,
                           std::uint32_t length) noexcept {
        if (length == 0u || length > 4u) return;

        // VHDP is a four-lane dot product in which the final encoded source
        // lane is forced to constant ONE.  The VFPU rewrites only that lane's
        // swizzle/constant controls: its original absolute and negate bits are
        // deliberately retained.  Short vectors still use a four-lane prefix
        // view, so constants may populate lanes outside the nominal length.
        float source[4]{};
        float target[4]{};
        read_vfpu_vector(source, source_register, length);
        read_vfpu_vector(target, target_register, length);

        const std::uint32_t forced_lane = length - 1u;
        const std::uint32_t swizzle_shift = forced_lane * 2u;
        const std::uint32_t rewritten_source_prefix =
            (vfpu_ctrl[0] & ~(3u << swizzle_shift)) |
            (1u << swizzle_shift) |
            (1u << (12u + forced_lane));

        const std::uint32_t original_source_prefix = vfpu_ctrl[0];
        vfpu_ctrl[0] = rewritten_source_prefix;
        apply_vfpu_source_prefix(source, 4u, 0u);
        vfpu_ctrl[0] = original_source_prefix;
        apply_vfpu_source_prefix(target, 4u, 1u);

        float sum = 0.0f;
        for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
            sum += source[lane] * target[lane];
        }
        if (std::isnan(sum)) sum = std::fabs(sum);

        const float result[1]{sum};
        write_vfpu_vector_with_destination_prefix(result, destination_scalar_register, 1u);
    }

    void execute_vfpu_horizontal(std::uint32_t destination_scalar_register,
                                 std::uint32_t source_register,
                                 std::uint32_t source_length,
                                 bool average) noexcept {
        if (source_length == 0u || source_length > 4u) return;

        // The horizontal instructions use a four-lane view even for shorter
        // encoded vectors.  Prefix constants can therefore populate lanes
        // beyond the nominal source size.
        float source[4]{};
        read_vfpu_vector(source, source_register, source_length);
        apply_vfpu_source_prefix(source, 4u, 0u);

        const std::uint32_t original_target_prefix = vfpu_ctrl[1];
        float weights[4]{};
        if (!average) {
            // VFAD forces every T lane to constant ONE, but deliberately
            // retains the original T absolute and negate controls.  On the
            // VFPU, an absolute bit changes forced ONE into constant 1/3.
            vfpu_ctrl[1] = (original_target_prefix & ~0x000000FFu) | 0x0000F055u;
        } else {
            // VAVG forces 0, 1/2, 1/3, or 1/4 according to the encoded vector
            // size.  It discards T swizzle/absolute controls but retains T
            // negate flags, matching the hardware prefix rewrite.
            static constexpr std::uint32_t average_prefix[4]{
                0x0000F000u, // scalar: 0
                0x0000F0FFu, // pair:   1/2
                0x0000FF55u, // triple: 1/3
                0x0000FFAAu, // quad:   1/4
            };
            vfpu_ctrl[1] = (original_target_prefix & ~0x00000FFFu) |
                           average_prefix[source_length - 1u];
        }
        apply_vfpu_source_prefix(weights, 4u, 1u);
        vfpu_ctrl[1] = original_target_prefix;

        float result[1]{0.0f};
        for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
            result[0] += source[lane] * weights[lane];
        }
        write_vfpu_vector_with_destination_prefix(result, destination_scalar_register, 1u);
    }

    [[nodiscard]] static constexpr bool vfpu_is_nan_or_inf_bits(std::uint32_t bits) noexcept {
        return (bits & 0x7F800000u) == 0x7F800000u;
    }

    [[nodiscard]] static std::uint32_t vfpu_minmax_bits(std::uint32_t source_bits,
                                                        std::uint32_t target_bits,
                                                        bool maximum) noexcept {
        const float source = std::bit_cast<float>(source_bits);
        const float target = std::bit_cast<float>(target_bits);
        if (vfpu_is_nan_or_inf_bits(source_bits) || vfpu_is_nan_or_inf_bits(target_bits)) {
            const auto source_signed = static_cast<std::int32_t>(source_bits);
            const auto target_signed = static_cast<std::int32_t>(target_bits);
            const bool both_negative = source_signed < 0 && target_signed < 0;
            const std::int32_t selected = maximum
                ? (both_negative ? std::min(target_signed, source_signed)
                                 : std::max(target_signed, source_signed))
                : (both_negative ? std::max(target_signed, source_signed)
                                 : std::min(target_signed, source_signed));
            return static_cast<std::uint32_t>(selected);
        }

        // The PSP chooses T when values compare equal, preserving T's sign for
        // +0/-0.  The operand order below deliberately matches std::min(T, S)
        // and std::max(T, S) in the reference interpreter.
        const float selected = maximum ? std::max(target, source) : std::min(target, source);
        return std::bit_cast<std::uint32_t>(selected);
    }

    void execute_vfpu_cross_quat(std::uint32_t destination_register,
                                 std::uint32_t source_register, std::uint32_t target_register,
                                 std::uint32_t length) noexcept {
        if (length == 0u || length > 4u) return;

        float source[4]{};
        float target[4]{};
        float result[4]{};
        read_vfpu_vector(source, source_register, length);
        read_vfpu_vector(target, target_register, length);

        constexpr std::uint32_t kSwizzleAndNegateMask = 0x000F00FFu;
        if (length == 3u) { // VCRSP.T
            // X/Y are produced directly from the unprefixed inputs.  The PSP
            // applies rewritten S/T prefixes only to the final dot-product lane.
            result[0] = source[1] * target[2] - source[2] * target[1];
            result[1] = source[2] * target[0] - source[0] * target[2];

            // Forced T view: [T.y, -T.x, T.w, T.z], while retaining the
            // original constant/absolute controls.
            vfpu_ctrl[1] = (vfpu_ctrl[1] & ~kSwizzleAndNegateMask) | 0x000200B1u;
            apply_vfpu_source_prefix(target, 4u, 1u);
            apply_vfpu_source_prefix(source, 4u, 0u);
            result[2] = source[0] * target[0] + source[1] * target[1] +
                        source[2] * target[2] + source[3] * target[3];
        } else if (length == 4u) { // VQMUL.Q
            result[0] = source[0] * target[3] + source[1] * target[2] -
                        source[2] * target[1] + source[3] * target[0];
            result[1] = -source[0] * target[2] + source[1] * target[3] +
                         source[2] * target[0] + source[3] * target[1];
            result[2] = source[0] * target[1] - source[1] * target[0] +
                        source[2] * target[3] + source[3] * target[2];

            // Forced T view: [-T.x, -T.y, -T.z, T.w], retaining constants/abs.
            vfpu_ctrl[1] = (vfpu_ctrl[1] & ~kSwizzleAndNegateMask) | 0x000700E4u;
            apply_vfpu_source_prefix(target, 4u, 1u);
            apply_vfpu_source_prefix(source, 4u, 0u);
            result[3] = source[0] * target[0] + source[1] * target[1] +
                        source[2] * target[2] + source[3] * target[3];
        } else if (length == 2u) {
            result[0] = 0.0f;
            // Pair form can source lane 2 through S-prefix swizzling.
            vfpu_ctrl[1] = (vfpu_ctrl[1] & ~kSwizzleAndNegateMask);
            apply_vfpu_source_prefix(target, 4u, 1u);
            apply_vfpu_source_prefix(source, 4u, 0u);
            result[1] = source[2] * target[2];
        } else {
            result[0] = 0.0f;
        }

        // Hardware applies the original D-prefix lane-0 controls to the last
        // result lane only.  All earlier lanes are written unmasked/unsaturated.
        if (length == 1u) {
            vfpu_ctrl[2] = 0u;
        } else {
            const std::uint32_t destination_prefix = vfpu_ctrl[2];
            const std::uint32_t last_lane = length - 1u;
            const std::uint32_t last_mask = ((destination_prefix >> 8u) & 1u) << (8u + last_lane);
            const std::uint32_t last_saturation = (destination_prefix & 3u) << (last_lane * 2u);
            vfpu_ctrl[2] = last_mask | last_saturation;
        }
        write_vfpu_vector_with_destination_prefix(result, destination_register, length);
    }

    void execute_vfpu_vminmax(std::uint32_t destination_register,
                              std::uint32_t source_register, std::uint32_t target_register,
                              std::uint32_t length, bool maximum) noexcept {
        if (length == 0u || length > 4u) return;

        float source[4]{};
        float target[4]{};
        float result[4]{};
        read_vfpu_vector(source, source_register, length);
        read_vfpu_vector(target, target_register, length);
        apply_vfpu_source_prefix(source, length, 0u);
        apply_vfpu_source_prefix(target, length, 1u);

        const std::uint32_t source_prefix = vfpu_ctrl[0];
        const std::uint32_t target_prefix = vfpu_ctrl[1];
        for (std::uint32_t lane = 0u; lane < length; ++lane) {
            const std::uint32_t source_swizzle = (source_prefix >> (lane * 2u)) & 3u;
            const std::uint32_t target_swizzle = (target_prefix >> (lane * 2u)) & 3u;
            const bool source_constant = ((source_prefix >> (12u + lane)) & 1u) != 0u;
            const bool target_constant = ((target_prefix >> (12u + lane)) & 1u) != 0u;
            if ((!source_constant && source_swizzle >= length) ||
                (!target_constant && target_swizzle >= length)) {
                // VFPU min/max wires an invalid swizzle to an exact +0 result.
                result[lane] = 0.0f;
                continue;
            }
            const std::uint32_t selected = vfpu_minmax_bits(
                std::bit_cast<std::uint32_t>(source[lane]),
                std::bit_cast<std::uint32_t>(target[lane]), maximum);
            result[lane] = std::bit_cast<float>(selected);
        }
        write_vfpu_vector_with_destination_prefix(result, destination_register, length);
    }

    // Compile-time forms of VMIN/VMAX, VX2I and VI2X. DefaultPrefixes: codegen
    // proved S/T are 0xE4 and D is 0, so lanes map straight through and the
    // consume stores are redundant; otherwise they defer to the generic helper.
    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t TargetRegister,
              std::uint32_t Length, bool Maximum, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vminmax_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        if constexpr (!DefaultPrefixes) {
            execute_vfpu_vminmax(DestinationRegister, SourceRegister, TargetRegister, Length, Maximum);
        } else {
            float source[4]{};
            float target[4]{};
            read_vfpu_vector_ct<SourceRegister, Length>(source);
            read_vfpu_vector_ct<TargetRegister, Length>(target);
            for (std::uint32_t lane = 0u; lane < Length; ++lane)
                source[lane] = std::bit_cast<float>(vfpu_minmax_bits(
                    std::bit_cast<std::uint32_t>(source[lane]), std::bit_cast<std::uint32_t>(target[lane]), Maximum));
            write_vfpu_vector_ct<DestinationRegister, Length>(source);
        }
    }

    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t SourceLength,
              std::uint32_t Operation, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vx2i_ct() noexcept {
        static_assert(SourceLength >= 1u && SourceLength <= 4u && Operation <= 3u);
        if constexpr (!DefaultPrefixes) {
            execute_vfpu_vx2i(DestinationRegister, SourceRegister, SourceLength, Operation);
        } else {
            float source[4]{};
            read_vfpu_vector_ct<SourceRegister, SourceLength>(source);
            std::uint32_t bits[4]{};
            if constexpr (Operation == 0u) { // VUC2I
                std::uint32_t value = std::bit_cast<std::uint32_t>(source[0]);
                for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
                    bits[lane] = ((value & 0xFFu) * 0x01010101u) >> 1u;
                    value >>= 8u;
                }
            } else if constexpr (Operation == 1u) { // VC2I
                const std::uint32_t value = std::bit_cast<std::uint32_t>(source[0]);
                bits[0] = (value & 0x000000FFu) << 24u;
                bits[1] = (value & 0x0000FF00u) << 16u;
                bits[2] = (value & 0x00FF0000u) << 8u;
                bits[3] = value & 0xFF000000u;
            } else { // VUS2I / VS2I
                constexpr std::uint32_t inputs = SourceLength < 2u ? SourceLength : 2u;
                for (std::uint32_t lane = 0u; lane < inputs; ++lane) {
                    const std::uint32_t value = std::bit_cast<std::uint32_t>(source[lane]);
                    bits[lane * 2u] = Operation == 2u ? (value & 0x0000FFFFu) << 15u : (value & 0x0000FFFFu) << 16u;
                    bits[lane * 2u + 1u] = Operation == 2u ? (value & 0xFFFF0000u) >> 1u : value & 0xFFFF0000u;
                }
            }
            constexpr std::uint32_t destination_length = Operation <= 1u ? 4u : (SourceLength == 1u ? 2u : 4u);
            float result[4]{};
            for (std::uint32_t lane = 0u; lane < destination_length; ++lane) result[lane] = std::bit_cast<float>(bits[lane]);
            write_vfpu_vector_ct<DestinationRegister, destination_length>(result);
        }
    }

    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t SourceLength,
              std::uint32_t Operation, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vi2x_ct() noexcept {
        static_assert(SourceLength >= 1u && SourceLength <= 4u && Operation <= 3u);
        if constexpr (!DefaultPrefixes) {
            execute_vfpu_vi2x(DestinationRegister, SourceRegister, SourceLength, Operation);
        } else {
            float source[4]{};
            read_vfpu_vector_ct<SourceRegister, SourceLength>(source);
            std::int32_t lanes[4]{};
            for (std::uint32_t lane = 0u; lane < SourceLength; ++lane) lanes[lane] = std::bit_cast<std::int32_t>(source[lane]);
            std::uint32_t bits[2]{};
            if constexpr (Operation <= 1u) { // VI2UC / VI2C
                for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
                    const std::int32_t value = lanes[lane];
                    const std::uint32_t byte = Operation == 0u
                        ? (value < 0 ? 0u : (static_cast<std::uint32_t>(value) >> 23u) & 0xFFu)
                        : (static_cast<std::uint32_t>(value) >> 24u) & 0xFFu;
                    bits[0] |= byte << (lane * 8u);
                }
            } else { // VI2US / VI2S
                for (std::uint32_t out_lane = 0u; out_lane < (SourceLength >= 4u ? 2u : 1u); ++out_lane) {
                    for (std::uint32_t half = 0u; half < 2u; ++half) {
                        const std::int32_t value = lanes[out_lane * 2u + half];
                        const std::uint32_t half_bits = Operation == 2u
                            ? (value < 0 ? 0u : (static_cast<std::uint32_t>(value) >> 15u) & 0xFFFFu)
                            : (static_cast<std::uint32_t>(value) >> 16u) & 0xFFFFu;
                        bits[out_lane] |= half_bits << (half * 16u);
                    }
                }
            }
            constexpr std::uint32_t destination_length = Operation <= 1u ? 1u : (SourceLength >= 4u ? 2u : 1u);
            float result[2]{std::bit_cast<float>(bits[0]), std::bit_cast<float>(bits[1])};
            write_vfpu_vector_ct<DestinationRegister, destination_length>(result);
        }
    }

    void execute_vfpu_compare3(std::uint32_t destination_register,
                               std::uint32_t source_register, std::uint32_t target_register,
                               std::uint32_t length, std::uint32_t operation) noexcept {
        if (length == 0u || length > 4u || operation < 5u || operation > 7u) return;

        float source[4]{};
        float target[4]{};
        float result[4]{};
        read_vfpu_vector(source, source_register, length);
        read_vfpu_vector(target, target_register, length);
        apply_vfpu_source_prefix(source, length, 0u);
        apply_vfpu_source_prefix(target, length, 1u);

        const std::uint32_t source_prefix = vfpu_ctrl[0];
        const std::uint32_t target_prefix = vfpu_ctrl[1];
        for (std::uint32_t lane = 0u; lane < length; ++lane) {
            const std::uint32_t source_swizzle = (source_prefix >> (lane * 2u)) & 3u;
            const std::uint32_t target_swizzle = (target_prefix >> (lane * 2u)) & 3u;
            const bool source_constant = ((source_prefix >> (12u + lane)) & 1u) != 0u;
            const bool target_constant = ((target_prefix >> (12u + lane)) & 1u) != 0u;
            if ((!source_constant && source_swizzle >= length) ||
                (!target_constant && target_swizzle >= length)) {
                result[lane] = 0.0f;
                continue;
            }

            if (operation == 5u) { // VSCMP
                const float difference = source[lane] - target[lane];
                if (std::isnan(difference)) {
                    const std::uint32_t source_bits = std::bit_cast<std::uint32_t>(source[lane]);
                    const std::uint32_t target_bits = std::bit_cast<std::uint32_t>(target[lane]);
                    const std::int64_t source_magnitude = static_cast<std::int64_t>(source_bits & 0x7FFFFFFFu);
                    const std::int64_t target_magnitude = static_cast<std::int64_t>(target_bits & 0x7FFFFFFFu);
                    const std::int64_t ordered_source = (source_bits & 0x80000000u) != 0u
                        ? -source_magnitude : source_magnitude;
                    const std::int64_t ordered_target = (target_bits & 0x80000000u) != 0u
                        ? -target_magnitude : target_magnitude;
                    result[lane] = ordered_source > ordered_target ? 1.0f
                                 : ordered_source < ordered_target ? -1.0f : 0.0f;
                } else {
                    result[lane] = difference > 0.0f ? 1.0f
                                 : difference < 0.0f ? -1.0f : 0.0f;
                }
            } else if (operation == 6u) { // VSGE
                result[lane] = (!std::isnan(source[lane]) && !std::isnan(target[lane]) &&
                                source[lane] >= target[lane]) ? 1.0f : 0.0f;
            } else { // VSLT
                result[lane] = (!std::isnan(source[lane]) && !std::isnan(target[lane]) &&
                                source[lane] < target[lane]) ? 1.0f : 0.0f;
            }
        }
        write_vfpu_vector_with_destination_prefix(result, destination_register, length);
    }

    template <std::uint32_t Length, std::uint32_t Operation, std::uint32_t SourcePrefix, std::uint32_t TargetPrefix,
              std::uint32_t I>
    PSPRECOMP_CONTEXT_FORCEINLINE static float vfpu_compare3_lane(const float *source, const float *target) noexcept {
        constexpr std::uint32_t source_swizzle = (SourcePrefix >> (I * 2u)) & 3u;
        constexpr std::uint32_t target_swizzle = (TargetPrefix >> (I * 2u)) & 3u;
        constexpr bool source_constant = ((SourcePrefix >> (12u + I)) & 1u) != 0u;
        constexpr bool target_constant = ((TargetPrefix >> (12u + I)) & 1u) != 0u;
        if constexpr ((!source_constant && source_swizzle >= Length) || (!target_constant && target_swizzle >= Length)) {
            return 0.0f;
        } else if constexpr (Operation == 5u) {  // VSCMP
            const float difference = source[I] - target[I];
            if (std::isnan(difference)) [[unlikely]] {
                const std::uint32_t source_bits = std::bit_cast<std::uint32_t>(source[I]);
                const std::uint32_t target_bits = std::bit_cast<std::uint32_t>(target[I]);
                const std::int64_t source_magnitude = static_cast<std::int64_t>(source_bits & 0x7FFFFFFFu);
                const std::int64_t target_magnitude = static_cast<std::int64_t>(target_bits & 0x7FFFFFFFu);
                const std::int64_t ordered_source = (source_bits & 0x80000000u) != 0u ? -source_magnitude : source_magnitude;
                const std::int64_t ordered_target = (target_bits & 0x80000000u) != 0u ? -target_magnitude : target_magnitude;
                return ordered_source > ordered_target ? 1.0f : ordered_source < ordered_target ? -1.0f : 0.0f;
            }
            return difference > 0.0f ? 1.0f : difference < 0.0f ? -1.0f : 0.0f;
        } else if constexpr (Operation == 6u) {  // VSGE
            return (!std::isnan(source[I]) && !std::isnan(target[I]) && source[I] >= target[I]) ? 1.0f : 0.0f;
        } else {  // VSLT
            return (!std::isnan(source[I]) && !std::isnan(target[I]) && source[I] < target[I]) ? 1.0f : 0.0f;
        }
    }

    // execute_vfpu_compare3 with the operands and the S/T/D prefixes known.
    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t TargetRegister,
              std::uint32_t Length, std::uint32_t Operation, std::uint32_t SourcePrefix, std::uint32_t TargetPrefix,
              std::uint32_t DestinationPrefix>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_compare3_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u && Operation >= 5u && Operation <= 7u);
        float source[4]{};
        float target[4]{};
        float result[4]{};
        read_vfpu_vector_prefix_const<SourceRegister, Length, SourcePrefix>(source);
        read_vfpu_vector_prefix_const<TargetRegister, Length, TargetPrefix>(target);
        result[0] = vfpu_compare3_lane<Length, Operation, SourcePrefix, TargetPrefix, 0u>(source, target);
        if constexpr (Length >= 2u) result[1] = vfpu_compare3_lane<Length, Operation, SourcePrefix, TargetPrefix, 1u>(source, target);
        if constexpr (Length >= 3u) result[2] = vfpu_compare3_lane<Length, Operation, SourcePrefix, TargetPrefix, 2u>(source, target);
        if constexpr (Length >= 4u) result[3] = vfpu_compare3_lane<Length, Operation, SourcePrefix, TargetPrefix, 3u>(source, target);
        write_vfpu_vector_dprefix_const<DestinationRegister, Length, DestinationPrefix>(result);
    }

    // execute_vfpu_horizontal (VFAD/VAVG) with the operands and prefixes known.
    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t Length, bool Average,
              std::uint32_t SourcePrefix, std::uint32_t TargetPrefix, std::uint32_t DestinationPrefix>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_horizontal_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        float source[4]{};
        read_vfpu_vector_ct<SourceRegister, Length>(source);
        apply_vfpu_prefix_const<SourcePrefix, 4u>(source);
        constexpr std::uint32_t average_prefix[4]{0x0000F000u, 0x0000F0FFu, 0x0000FF55u, 0x0000FFAAu};
        constexpr std::uint32_t weight_prefix = !Average
            ? ((TargetPrefix & ~0x000000FFu) | 0x0000F055u)
            : ((TargetPrefix & ~0x00000FFFu) | average_prefix[Length - 1u]);
        float weights[4]{};
        apply_vfpu_prefix_const<weight_prefix, 4u>(weights);
        float result[1]{0.0f};
        result[0] += source[0] * weights[0];
        result[0] += source[1] * weights[1];
        result[0] += source[2] * weights[2];
        result[0] += source[3] * weights[3];
        write_vfpu_vector_dprefix_const<DestinationRegister, 1u, DestinationPrefix>(result);
    }

    // execute_vfpu_cross_quat (VCRSP.T / VQMUL.Q) with the operands and
    // prefixes known.
    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister, std::uint32_t TargetRegister,
              std::uint32_t Length, std::uint32_t SourcePrefix, std::uint32_t TargetPrefix, std::uint32_t DestinationPrefix>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_cross_quat_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        float source[4]{};
        float target[4]{};
        float result[4]{};
        read_vfpu_vector_ct<SourceRegister, Length>(source);
        read_vfpu_vector_ct<TargetRegister, Length>(target);
        constexpr std::uint32_t kSwizzleAndNegateMask = 0x000F00FFu;
        if constexpr (Length == 3u) {
            result[0] = source[1] * target[2] - source[2] * target[1];
            result[1] = source[2] * target[0] - source[0] * target[2];
            apply_vfpu_prefix_const<(TargetPrefix & ~kSwizzleAndNegateMask) | 0x000200B1u, 4u>(target);
            apply_vfpu_prefix_const<SourcePrefix, 4u>(source);
            result[2] = source[0] * target[0] + source[1] * target[1] + source[2] * target[2] + source[3] * target[3];
        } else if constexpr (Length == 4u) {
            result[0] = source[0] * target[3] + source[1] * target[2] - source[2] * target[1] + source[3] * target[0];
            result[1] = -source[0] * target[2] + source[1] * target[3] + source[2] * target[0] + source[3] * target[1];
            result[2] = source[0] * target[1] - source[1] * target[0] + source[2] * target[3] + source[3] * target[2];
            apply_vfpu_prefix_const<(TargetPrefix & ~kSwizzleAndNegateMask) | 0x000700E4u, 4u>(target);
            apply_vfpu_prefix_const<SourcePrefix, 4u>(source);
            result[3] = source[0] * target[0] + source[1] * target[1] + source[2] * target[2] + source[3] * target[3];
        } else if constexpr (Length == 2u) {
            result[0] = 0.0f;
            apply_vfpu_prefix_const<(TargetPrefix & ~kSwizzleAndNegateMask), 4u>(target);
            apply_vfpu_prefix_const<SourcePrefix, 4u>(source);
            result[1] = source[2] * target[2];
        } else {
            result[0] = 0.0f;
        }
        constexpr std::uint32_t last_lane = Length - 1u;
        constexpr std::uint32_t destination_prefix = Length == 1u ? 0u
            : ((((DestinationPrefix >> 8u) & 1u) << (8u + last_lane)) | ((DestinationPrefix & 3u) << (last_lane * 2u)));
        write_vfpu_vector_dprefix_const<DestinationRegister, Length, destination_prefix>(result);
    }

    // DefaultPrefixes: the code generator knows S/T/D are at their defaults,
    // so no prefix is read and none needs resetting afterwards.
    template <std::uint32_t SourceRegister, std::uint32_t TargetRegister,
              std::uint32_t Length, std::uint32_t Condition, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vcmp_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        static_assert(Condition < 16u);
        float source[4]{};
        float target[4]{};
        if constexpr (DefaultPrefixes) {
            read_vfpu_vector_ct<SourceRegister, Length>(source);
            read_vfpu_vector_ct<TargetRegister, Length>(target);
        } else {
            read_vfpu_vector_with_source_prefix_ct<SourceRegister, Length, 0u>(source);
            read_vfpu_vector_with_source_prefix_ct<TargetRegister, Length, 1u>(target);
        }

        auto compare_lane = [](float sv, float tv) -> bool {
            if constexpr (Condition == 0u) return false;
            else if constexpr (Condition == 1u) return sv == tv;
            else if constexpr (Condition == 2u) return sv < tv;
            else if constexpr (Condition == 3u) return sv <= tv;
            else if constexpr (Condition == 4u) return true;
            else if constexpr (Condition == 5u) return sv != tv;
            else if constexpr (Condition == 6u) return sv >= tv;
            else if constexpr (Condition == 7u) return sv > tv;
            else if constexpr (Condition == 8u) return sv == 0.0f;
            else if constexpr (Condition == 9u) return std::isnan(sv);
            else if constexpr (Condition == 10u) return std::isinf(sv);
            else if constexpr (Condition == 11u) return std::isnan(sv) || std::isinf(sv);
            else if constexpr (Condition == 12u) return sv != 0.0f;
            else if constexpr (Condition == 13u) return !std::isnan(sv);
            else if constexpr (Condition == 14u) return !std::isinf(sv);
            else return !(std::isnan(sv) || std::isinf(sv));
        };

        const bool r0 = compare_lane(source[0], target[0]);
        const bool r1 = Length >= 2u ? compare_lane(source[1], target[1]) : false;
        const bool r2 = Length >= 3u ? compare_lane(source[2], target[2]) : false;
        const bool r3 = Length >= 4u ? compare_lane(source[3], target[3]) : false;
        std::uint32_t lane_bits = static_cast<std::uint32_t>(r0);
        if constexpr (Length >= 2u) lane_bits |= static_cast<std::uint32_t>(r1) << 1u;
        if constexpr (Length >= 3u) lane_bits |= static_cast<std::uint32_t>(r2) << 2u;
        if constexpr (Length >= 4u) lane_bits |= static_cast<std::uint32_t>(r3) << 3u;
        bool any = r0;
        bool all = r0;
        if constexpr (Length >= 2u) { any = any || r1; all = all && r1; }
        if constexpr (Length >= 3u) { any = any || r2; all = all && r2; }
        if constexpr (Length >= 4u) { any = any || r3; all = all && r3; }

        constexpr std::uint32_t affected = ((1u << Length) - 1u) | (1u << 4u) | (1u << 5u);
        const std::uint32_t update = lane_bits | (static_cast<std::uint32_t>(any) << 4u) |
            (static_cast<std::uint32_t>(all) << 5u);
        vfpu_ctrl[3] = (vfpu_ctrl[3] & ~affected) | (update & affected);
        if constexpr (!DefaultPrefixes) eat_vfpu_prefixes();
    }

    void execute_vfpu_vcmp(std::uint32_t source_register, std::uint32_t target_register,
                            std::uint32_t length, std::uint32_t condition) noexcept {
        if (length == 0u || length > 4u) return;

        float source[4]{};
        float target[4]{};
        read_vfpu_vector_with_source_prefix(source, source_register, length, 0u);
        read_vfpu_vector_with_source_prefix(target, target_register, length, 1u);

        std::uint32_t lane_bits = 0u;
        bool any = false;
        bool all = true;
        for (std::uint32_t lane = 0u; lane < length; ++lane) {
            const float s = source[lane];
            const float t = target[lane];
            bool result = false;
            switch (condition & 15u) {
            case 0u: result = false; break;                         // FL
            case 1u: result = s == t; break;                        // EQ
            case 2u: result = s < t; break;                         // LT
            case 3u: result = s <= t; break;                        // LE
            case 4u: result = true; break;                          // TR
            case 5u: result = s != t; break;                        // NE
            case 6u: result = s >= t; break;                        // GE
            case 7u: result = s > t; break;                         // GT
            case 8u: result = s == 0.0f; break;                     // EZ
            case 9u: result = std::isnan(s); break;                 // EN
            case 10u: result = std::isinf(s); break;                // EI
            case 11u: result = std::isnan(s) || std::isinf(s); break; // ES
            case 12u: result = s != 0.0f; break;                    // NZ
            case 13u: result = !std::isnan(s); break;               // NN
            case 14u: result = !std::isinf(s); break;               // NI
            default: result = !(std::isnan(s) || std::isinf(s)); break; // NS
            }
            if (result) lane_bits |= 1u << lane;
            any = any || result;
            all = all && result;
        }

        // CC lanes x/y/z/w occupy bits 0..3; bit 4 is ANY and bit 5
        // is ALL.  A narrower comparison preserves untouched lane bits.
        const std::uint32_t affected = ((1u << length) - 1u) | (1u << 4u) | (1u << 5u);
        const std::uint32_t update = lane_bits | (static_cast<std::uint32_t>(any) << 4u) |
            (static_cast<std::uint32_t>(all) << 5u);
        vfpu_ctrl[3] = (vfpu_ctrl[3] & ~affected) | (update & affected);
        eat_vfpu_prefixes();
    }

    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister,
              std::uint32_t Length, std::uint32_t ConditionIndex, bool MoveIfFalse, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vcmov_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        static_assert(ConditionIndex < 8u);
        float source[4]{};
        float destination[4]{};
        if constexpr (DefaultPrefixes) {
            read_vfpu_vector_ct<SourceRegister, Length>(source);
            read_vfpu_vector_ct<DestinationRegister, Length>(destination);
        } else {
            read_vfpu_vector_with_source_prefix_ct<SourceRegister, Length, 0u>(source);
            read_vfpu_vector_ct<DestinationRegister, Length>(destination);
            apply_vfpu_source_prefix_ct<Length, 1u>(destination);
        }

        const std::uint32_t condition_code = vfpu_ctrl[3];
        if constexpr (ConditionIndex < 6u) {
            const bool cc = ((condition_code >> ConditionIndex) & 1u) != 0u;
            if (cc == !MoveIfFalse) {
                destination[0] = source[0];
                if constexpr (Length >= 2u) destination[1] = source[1];
                if constexpr (Length >= 3u) destination[2] = source[2];
                if constexpr (Length >= 4u) destination[3] = source[3];
            }
        } else if constexpr (ConditionIndex == 6u) {
            const bool want = !MoveIfFalse;
            if ((((condition_code >> 0u) & 1u) != 0u) == want) destination[0] = source[0];
            if constexpr (Length >= 2u)
                if ((((condition_code >> 1u) & 1u) != 0u) == want) destination[1] = source[1];
            if constexpr (Length >= 3u)
                if ((((condition_code >> 2u) & 1u) != 0u) == want) destination[2] = source[2];
            if constexpr (Length >= 4u)
                if ((((condition_code >> 3u) & 1u) != 0u) == want) destination[3] = source[3];
        }
        if constexpr (DefaultPrefixes) write_vfpu_vector_ct<DestinationRegister, Length>(destination);
        else write_vfpu_vector_with_destination_prefix_ct<DestinationRegister, Length>(destination);
    }

    void execute_vfpu_vcmov(std::uint32_t destination_register, std::uint32_t source_register,
                             std::uint32_t length, std::uint32_t condition_index,
                             bool move_if_false) noexcept {
        if (length == 0u || length > 4u) return;

        float source[4]{};
        float destination[4]{};
        read_vfpu_vector_with_source_prefix(source, source_register, length, 0u);

        // VCMOV unusually treats the old destination as its T input, so the
        // guest T prefix is applied even when no source lane is selected.
        read_vfpu_vector(destination, destination_register, length);
        apply_vfpu_source_prefix(destination, length, 1u);

        const std::uint32_t condition_code = vfpu_ctrl[3];
        if (condition_index < 6u) {
            const bool cc = ((condition_code >> condition_index) & 1u) != 0u;
            if (cc == !move_if_false) {
                for (std::uint32_t lane = 0u; lane < length; ++lane) destination[lane] = source[lane];
            }
        } else if (condition_index == 6u) {
            for (std::uint32_t lane = 0u; lane < length; ++lane) {
                const bool cc = ((condition_code >> lane) & 1u) != 0u;
                if (cc == !move_if_false) destination[lane] = source[lane];
            }
        }
        // condition_index 7 is invalid on hardware; preserving the T-prefixed
        // destination is deterministic and leaves error reporting to callers.

        write_vfpu_vector_with_destination_prefix(destination, destination_register, length);
    }

    template <std::uint32_t DestinationRegister, std::uint32_t SourceRegister,
              std::uint32_t TargetScalarRegister, std::uint32_t Length, bool DefaultPrefixes = false>
    PSPRECOMP_CONTEXT_FORCEINLINE void execute_vfpu_vscl_ct() noexcept {
        static_assert(Length >= 1u && Length <= 4u);
        float source[4]{};
        float target[4]{};
        target[0] = std::bit_cast<float>(vfpu_scalar_bits_ct<(TargetScalarRegister & 0x7Fu)>());
        if constexpr (DefaultPrefixes) {
            // 0xE4 with the swizzle cleared broadcasts the scalar to every lane.
            read_vfpu_vector_ct<SourceRegister, Length>(source);
            if constexpr (Length >= 2u) target[1] = target[0];
            if constexpr (Length >= 3u) target[2] = target[0];
            if constexpr (Length >= 4u) target[3] = target[0];
        } else {
            read_vfpu_vector_with_source_prefix_ct<SourceRegister, Length, 0u>(source);
            const std::uint32_t original_target_prefix = vfpu_ctrl[1];
            vfpu_ctrl[1] = original_target_prefix & ~0xFFu;
            apply_vfpu_source_prefix_ct<Length, 1u>(target);
            vfpu_ctrl[1] = original_target_prefix;
        }

        float result[4]{};
        result[0] = source[0] * target[0];
        if constexpr (Length >= 2u) result[1] = source[1] * target[1];
        if constexpr (Length >= 3u) result[2] = source[2] * target[2];
        if constexpr (Length >= 4u) result[3] = source[3] * target[3];
        if constexpr (DefaultPrefixes) write_vfpu_vector_ct<DestinationRegister, Length>(result);
        else write_vfpu_vector_with_destination_prefix_ct<DestinationRegister, Length>(result);
    }

    void execute_vfpu_vscl(std::uint32_t destination_register, std::uint32_t source_register,
                            std::uint32_t target_scalar_register, std::uint32_t length) noexcept {
        if (length == 0u || length > 4u) return;

        float source[4]{};
        read_vfpu_vector_with_source_prefix(source, source_register, length, 0u);

        // VSCL reads VT as a scalar, but the T prefix still operates on a
        // vector view.  Materialize the scalar in lane 0 and broadcast from
        // there, while preserving the guest prefix's abs, constant and negate
        // flags (those flags are indexed by output lane).
        //
        // Do not place the scalar in its encoded physical lane.  The source
        // prefix helper only populates `length` lanes; for VSCL.T a scalar in
        // physical lane w would therefore be outside the populated 3-lane
        // range and the broadcast would become exactly zero.  That produced
        // null normalized vectors, breaking both collision normals and world
        // vertex/lighting transforms.
        float target[4]{};
        target[0] = std::bit_cast<float>(vfpu_scalar_bits(target_scalar_register & 0x7Fu));
        const std::uint32_t original_target_prefix = vfpu_ctrl[1];
        vfpu_ctrl[1] = original_target_prefix & ~0xFFu;  // every output lane selects lane 0
        apply_vfpu_source_prefix(target, length, 1u);
        vfpu_ctrl[1] = original_target_prefix;

        float result[4]{};
        for (std::uint32_t lane = 0u; lane < length; ++lane) {
            result[lane] = source[lane] * target[lane];
        }
        write_vfpu_vector_with_destination_prefix(result, destination_register, length);
    }

    void execute_vfpu_vrot(std::uint32_t destination_register, std::uint32_t source_register,
                           std::uint32_t length, std::uint32_t immediate) noexcept {
        float source[4]{};
        read_vfpu_vector_with_source_prefix(source, source_register, 1u, 0u);
        const float original_source =
            std::bit_cast<float>(vfpu_scalar_bits(source_register & 0x7Fu));
        constexpr float half_pi = 1.57079632679489661923f;
        float sine = vfpu_sin(source[0] * half_pi);
        const float original_cosine = vfpu_cos(original_source * half_pi);
        if ((immediate & 0x10u) != 0u)
            sine = std::bit_cast<float>(std::bit_cast<std::uint32_t>(sine) ^ 0x80000000u);

        const std::uint32_t sine_lane = (immediate >> 2u) & 3u;
        const std::uint32_t cosine_lane = immediate & 3u;
        float value[4]{};
        if (sine_lane == cosine_lane) {
            for (std::uint32_t lane = 0u; lane < length && lane < 4u; ++lane) value[lane] = sine;
        } else if (sine_lane < length) {
            value[sine_lane] = sine;
        }

        float cosine = original_cosine;
        if (((destination_register >> 2u) & 7u) == ((source_register >> 2u) & 7u)) {
            const std::size_t source_index = vfpu_scalar_index(source_register & 0x7Fu);
            for (std::uint32_t lane = 0u; lane < length && lane < 4u; ++lane) {
                if (vfpu_vector_lane_index(destination_register, length, lane) == source_index) {
                    cosine = vfpu_cos(value[lane] * half_pi);
                    break;
                }
            }
        }
        if (cosine_lane < length) value[cosine_lane] = cosine;

        // VROT consumes all prefixes, but the cosine lane ignores destination
        // saturation and write masking on the hardware.
        if (cosine_lane < 4u) {
            vfpu_ctrl[2] &= ~((3u << (cosine_lane * 2u)) | (1u << (8u + cosine_lane)));
        }
        write_vfpu_vector_with_destination_prefix(value, destination_register, length);
    }

    void execute_vfpu_vocp(std::uint32_t destination_register, std::uint32_t source_register,
                           std::uint32_t length) noexcept {
        if (length == 0u || length > 4u) return;

        const std::uint32_t source_prefix = vfpu_ctrl[0];
        const std::uint32_t target_prefix = vfpu_ctrl[1];

        // VOCP forces the S-prefix negate flags on, preserving its swizzle,
        // abs, and constant controls.  Therefore the common no-prefix case
        // reads the source as -S.
        float source[4]{};
        read_vfpu_vector(source, source_register, length);
        vfpu_ctrl[0] = source_prefix | 0x000F0000u;
        apply_vfpu_source_prefix(source, length, 0u);
        vfpu_ctrl[0] = source_prefix;

        // VOCP forces every T lane to constant ONE while preserving the
        // original abs and negate flags.  In VFPU prefix encoding, ONE in all
        // four lanes is swizzle 1 plus all constant bits: 0x0000F055.
        float target[4]{};
        vfpu_ctrl[1] = (target_prefix & ~0x000000FFu) | 0x0000F055u;
        apply_vfpu_source_prefix(target, length, 1u);
        vfpu_ctrl[1] = target_prefix;

        float result[4]{};
        for (std::uint32_t lane = 0u; lane < length; ++lane) {
            // Hardware produces a positive NaN for a NaN source instead of
            // adding the forced T constant.
            result[lane] = std::isnan(source[lane]) ? std::fabs(source[lane])
                                                     : target[lane] + source[lane];

            // Invalid swizzles are retained as zero based on the original
            // prefixes, even though VOCP rewrites T to constants internally.
            const std::uint32_t source_swizzle = (source_prefix >> (lane * 2u)) & 3u;
            const std::uint32_t target_swizzle = (target_prefix >> (lane * 2u)) & 3u;
            const bool source_constant = ((source_prefix >> (12u + lane)) & 1u) != 0u;
            const bool target_constant = ((target_prefix >> (12u + lane)) & 1u) != 0u;
            if ((source_swizzle >= length && !source_constant) ||
                (target_swizzle >= length && !target_constant)) {
                result[lane] = 0.0f;
            }
        }

        write_vfpu_vector_with_destination_prefix(result, destination_register, length);
    }

    void read_vfpu_matrix(float *destination, std::uint32_t matrix_register, std::uint32_t side) const noexcept {
        const std::uint32_t matrix = (matrix_register >> 2u) & 7u;
        const std::uint32_t column = matrix_register & 3u;
        bool transpose = ((matrix_register >> 5u) & 1u) != 0u;
        std::uint32_t row = 0u;
        if (side == 1u) { transpose = false; row = (matrix_register >> 5u) & 3u; }
        else if (side == 2u || side == 4u) row = (matrix_register >> 5u) & 2u;
        else if (side == 3u) row = (matrix_register >> 6u) & 1u;
        const std::size_t base = static_cast<std::size_t>(matrix * 16u);
        for (std::uint32_t j = 0; j < side; ++j) {
            for (std::uint32_t i = 0; i < side; ++i) {
                const std::size_t index = transpose
                    ? base + static_cast<std::size_t>(((row + i) & 3u) * 4u + ((column + j) & 3u))
                    : base + static_cast<std::size_t>(((column + j) & 3u) * 4u + ((row + i) & 3u));
                destination[j * 4u + i] = vfpu[index];
            }
        }
    }

    void write_vfpu_matrix(const float *source, std::uint32_t matrix_register, std::uint32_t side) noexcept {
        const std::uint32_t matrix = (matrix_register >> 2u) & 7u;
        const std::uint32_t column = matrix_register & 3u;
        bool transpose = ((matrix_register >> 5u) & 1u) != 0u;
        std::uint32_t row = 0u;
        if (side == 1u) { transpose = false; row = (matrix_register >> 5u) & 3u; }
        else if (side == 2u || side == 4u) row = (matrix_register >> 5u) & 2u;
        else if (side == 3u) row = (matrix_register >> 6u) & 1u;
        const std::size_t base = static_cast<std::size_t>(matrix * 16u);
        for (std::uint32_t j = 0; j < side; ++j) {
            for (std::uint32_t i = 0; i < side; ++i) {
                const std::size_t index = transpose
                    ? base + static_cast<std::size_t>(((row + i) & 3u) * 4u + ((column + j) & 3u))
                    : base + static_cast<std::size_t>(((column + j) & 3u) * 4u + ((row + i) & 3u));
                vfpu[index] = source[j * 4u + i];
            }
        }
    }

    // read/write_vfpu_matrix with the register and side known at compile time,
    // so the lane mapping folds to constant indices.
    template <std::uint32_t MatrixRegister, std::uint32_t Side>
    static constexpr std::size_t vfpu_matrix_index(std::uint32_t j, std::uint32_t i) noexcept {
        constexpr std::uint32_t matrix = (MatrixRegister >> 2u) & 7u;
        constexpr std::uint32_t column = MatrixRegister & 3u;
        constexpr bool transpose = Side != 1u && ((MatrixRegister >> 5u) & 1u) != 0u;
        constexpr std::uint32_t row = Side == 1u ? (MatrixRegister >> 5u) & 3u
            : Side == 3u ? (MatrixRegister >> 6u) & 1u : (MatrixRegister >> 5u) & 2u;
        return matrix * 16u + (transpose ? ((row + i) & 3u) * 4u + ((column + j) & 3u)
                                         : ((column + j) & 3u) * 4u + ((row + i) & 3u));
    }

    template <std::uint32_t MatrixRegister, std::uint32_t Side>
    PSPRECOMP_CONTEXT_FORCEINLINE void read_vfpu_matrix_ct(float *destination) const noexcept {
        static_assert(Side >= 1u && Side <= 4u);
        for (std::uint32_t j = 0; j < Side; ++j)
            for (std::uint32_t i = 0; i < Side; ++i)
                destination[j * 4u + i] = vfpu[vfpu_matrix_index<MatrixRegister, Side>(j, i)];
    }

    template <std::uint32_t MatrixRegister, std::uint32_t Side>
    PSPRECOMP_CONTEXT_FORCEINLINE void write_vfpu_matrix_ct(const float *source) noexcept {
        static_assert(Side >= 1u && Side <= 4u);
        for (std::uint32_t j = 0; j < Side; ++j)
            for (std::uint32_t i = 0; i < Side; ++i)
                vfpu[vfpu_matrix_index<MatrixRegister, Side>(j, i)] = source[j * 4u + i];
    }

    void execute_vfpu_vmscl(std::uint32_t destination_matrix_register,
                            std::uint32_t source_matrix_register,
                            std::uint32_t target_scalar_register,
                            std::uint32_t side) noexcept {
        if (side == 0u || side > 4u) return;

        float source[16]{};
        float target[4]{};
        float result[16]{};
        float previous_destination[16]{};
        read_vfpu_matrix(source, source_matrix_register, side);
        read_vfpu_matrix(previous_destination, destination_matrix_register, side);
        read_vfpu_vector(target, target_scalar_register, 1u);

        const float scalar = target[0];
        for (std::uint32_t row = 0u; row + 1u < side; ++row) {
            for (std::uint32_t column = 0u; column < side; ++column) {
                result[row * 4u + column] = source[row * 4u + column] * scalar;
            }
        }

        // Hardware applies S/T prefixes only to the final matrix row.  T is
        // internally rewritten so every output lane selects the scalar's
        // physical VFPU lane, while retaining constant/absolute/negate bits.
        const std::uint32_t last_row = side - 1u;
        apply_vfpu_source_prefix(source + last_row * 4u, 4u, 0u);

        const std::uint32_t target_lane = (target_scalar_register >> 5u) & 3u;
        target[target_lane] = scalar;
        const std::uint32_t original_target_prefix = vfpu_ctrl[1];
        const std::uint32_t replicated_swizzle = target_lane * 0x55u;
        vfpu_ctrl[1] = (original_target_prefix & ~0xFFu) | replicated_swizzle;
        apply_vfpu_source_prefix(target, 4u, 1u);
        vfpu_ctrl[1] = original_target_prefix;

        for (std::uint32_t column = 0u; column < side; ++column) {
            result[last_row * 4u + column] = source[last_row * 4u + column] * target[column];
        }

        // D prefix saturation and mask apply only to the final row.
        const std::uint32_t destination_prefix = vfpu_ctrl[2];
        for (std::uint32_t column = 0u; column < side; ++column) {
            if (((destination_prefix >> (8u + column)) & 1u) != 0u) {
                result[last_row * 4u + column] = previous_destination[last_row * 4u + column];
                continue;
            }
            const std::uint32_t saturation = (destination_prefix >> (column * 2u)) & 3u;
            float &value = result[last_row * 4u + column];
            if (saturation == 1u) value = vfpu_clamp(value, 0.0f);
            else if (saturation == 3u) value = vfpu_clamp(value, -1.0f);
        }

        write_vfpu_matrix(result, destination_matrix_register, side);
        eat_vfpu_prefixes();
    }

    void execute_vfpu_vmmov(std::uint32_t destination_matrix_register,
                            std::uint32_t source_matrix_register,
                            std::uint32_t side) noexcept {
        if (side == 0u || side > 4u) return;

        float source[16]{};
        float previous_destination[16]{};
        read_vfpu_matrix(source, source_matrix_register, side);
        read_vfpu_matrix(previous_destination, destination_matrix_register, side);

        const std::uint32_t last_row = side - 1u;
        apply_vfpu_source_prefix(source + last_row * 4u, 4u, 0u);

        const std::uint32_t destination_prefix = vfpu_ctrl[2];
        for (std::uint32_t column = 0u; column < side; ++column) {
            float &value = source[last_row * 4u + column];
            if (((destination_prefix >> (8u + column)) & 1u) != 0u) {
                value = previous_destination[last_row * 4u + column];
                continue;
            }
            const std::uint32_t saturation = (destination_prefix >> (column * 2u)) & 3u;
            if (saturation == 1u) value = vfpu_clamp(value, 0.0f);
            else if (saturation == 3u) value = vfpu_clamp(value, -1.0f);
        }

        write_vfpu_matrix(source, destination_matrix_register, side);
        eat_vfpu_prefixes();
    }

    void execute_vfpu_matrix_init(std::uint32_t destination_matrix_register,
                                  std::uint32_t side,
                                  std::uint32_t operation) noexcept {
        if (side == 0u || side > 4u) return;
        if (operation != 3u && operation != 6u && operation != 7u) return;

        float matrix[16]{};
        float previous_destination[16]{};
        read_vfpu_matrix(previous_destination, destination_matrix_register, side);
        for (std::uint32_t row = 0u; row < side; ++row) {
            for (std::uint32_t column = 0u; column < side; ++column) {
                matrix[row * 4u + column] = operation == 7u ? 1.0f
                    : (operation == 3u && row == column ? 1.0f : 0.0f);
            }
        }

        // Matrix-init operations force the final row through source-prefix
        // constants while retaining the original absolute/negate controls.
        const std::uint32_t last_row = side - 1u;
        std::uint32_t rewritten_source_prefix = vfpu_ctrl[0] & ~0xFFu;
        for (std::uint32_t lane = 0u; lane < 4u; ++lane) {
            const bool one = operation == 7u || (operation == 3u && lane == last_row);
            rewritten_source_prefix |= (one ? 1u : 0u) << (lane * 2u);
            rewritten_source_prefix |= 1u << (12u + lane);
        }
        const std::uint32_t original_source_prefix = vfpu_ctrl[0];
        vfpu_ctrl[0] = rewritten_source_prefix;
        apply_vfpu_source_prefix(matrix + last_row * 4u, 4u, 0u);
        vfpu_ctrl[0] = original_source_prefix;

        // Matrix-init saturation is undefined on hardware; honor only the
        // architecturally useful final-row write mask.
        const std::uint32_t destination_prefix = vfpu_ctrl[2];
        for (std::uint32_t column = 0u; column < side; ++column) {
            if (((destination_prefix >> (8u + column)) & 1u) != 0u) {
                matrix[last_row * 4u + column] = previous_destination[last_row * 4u + column];
            }
        }

        write_vfpu_matrix(matrix, destination_matrix_register, side);
        eat_vfpu_prefixes();
    }

    void write_vfpu_identity_matrix(std::uint32_t matrix_register, std::uint32_t side) noexcept {
        const std::uint32_t matrix = (matrix_register >> 2u) & 7u;
        const std::uint32_t column = matrix_register & 3u;
        const bool transpose = ((matrix_register >> 5u) & 1u) != 0u;
        const std::uint32_t row = side == 3u ? ((matrix_register >> 6u) & 1u)
                                             : (side == 1u ? ((matrix_register >> 5u) & 3u)
                                                          : ((matrix_register >> 5u) & 2u));
        const std::size_t base = static_cast<std::size_t>(matrix * 16u);
        for (std::uint32_t j = 0; j < side; ++j) {
            for (std::uint32_t i = 0; i < side; ++i) {
                const std::size_t index = transpose
                    ? base + static_cast<std::size_t>(((row + i) & 3u) * 4u + ((column + j) & 3u))
                    : base + static_cast<std::size_t>(((column + j) & 3u) * 4u + ((row + i) & 3u));
                vfpu[index] = i == j ? 1.0f : 0.0f;
            }
        }
        eat_vfpu_prefixes();
    }

    PSPRECOMP_CONTEXT_FORCEINLINE void eat_vfpu_prefixes() noexcept {
        vfpu_ctrl[0] = 0xE4u;
        vfpu_ctrl[1] = 0xE4u;
        vfpu_ctrl[2] = 0u;
    }

    [[nodiscard]] bool execute_signed_add(std::uint32_t destination, std::uint32_t source_a,
                                          std::uint32_t source_b) noexcept {
        const std::int64_t result = static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[source_a & 31u])) +
                                    static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[source_b & 31u]));
        if (result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        set_gpr(destination, static_cast<std::uint32_t>(static_cast<std::int32_t>(result)));
        return true;
    }

    [[nodiscard]] bool execute_signed_sub(std::uint32_t destination, std::uint32_t source_a,
                                          std::uint32_t source_b) noexcept {
        const std::int64_t result = static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[source_a & 31u])) -
                                    static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[source_b & 31u]));
        if (result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        set_gpr(destination, static_cast<std::uint32_t>(static_cast<std::int32_t>(result)));
        return true;
    }

    void set_gpr(std::uint32_t index, std::uint32_t value) noexcept {
        // Allegrex $zero is initialized to zero and no supported path writes a
        // non-zero value into gpr[0].  Generated AOT emits set_gpr() for every
        // architectural register write (over half a million static call sites
        // in generated AOT corpora), so redundantly storing gpr[0] after *every* write
        // creates a hot dependency/store stream for no semantic benefit.
        // Writes whose destination is register zero are simply discarded, as
        // the hardware does. Runtime dispatch boundaries still assert gpr[0]
        // explicitly as a defensive invariant.
        if (index != 0u && index < gpr.size()) {
            gpr[index] = value;
        }
    }
};

} // namespace psprecomp
