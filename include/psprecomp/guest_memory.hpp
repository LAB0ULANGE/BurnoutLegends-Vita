#pragma once
#if !defined(__vita__) && !defined(PSPRECOMP_AOT_CHECK_BASED)
#define PSPRECOMP_AOT_CHECK_BASED 1  // TEMP check run
#endif

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psprecomp {

// Each generated AOT unit is one enormous function, and MSVC stops inlining
// into a caller that large -- measured: moving the fast paths into this header
// grew the linked image by only 2 KB, so the calls survived.  Roughly a third of
// the translated instructions are guest loads and stores, so the call has to go.
#if defined(_MSC_VER)
#define PSPRECOMP_MEMORY_FAST_PATH __forceinline
#else
#define PSPRECOMP_MEMORY_FAST_PATH inline __attribute__((always_inline))
#endif

#if defined(PSPRECOMP_COUNT_MIRROR_ACCESSES)
inline std::uint64_t aot_access_count{}, aot_mirror_count{};
#endif

class GuestMemory {
public:
    static constexpr std::uint32_t kVramPhysicalBase = 0x04000000u;
    static constexpr std::uint32_t kVramSize = 2u * 1024u * 1024u;
    static constexpr std::uint32_t kVramMirrorCount = 4u;
    static constexpr std::uint32_t kVramAddressSpan = kVramSize * kVramMirrorCount;
    static constexpr std::uint32_t kPhysicalBase = 0x08000000u;

    explicit GuestMemory(std::uint32_t size_bytes = 32u * 1024u * 1024u);

    // The AOT fast paths below index cached region pointers, so an instance may
    // not be relocated after construction.  Runtime owns exactly one by value
    // and never copies it; making that explicit turns a future copy into a
    // compile error instead of a dangling read.
    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;
    GuestMemory(GuestMemory &&) = delete;
    GuestMemory &operator=(GuestMemory &&) = delete;

    [[nodiscard]] std::uint32_t size() const noexcept;
    [[nodiscard]] std::uint32_t vram_size() const noexcept;
    // Allegrex uses cached/uncached MIPS aliases; this maps addresses such as
    // 0x44000000 and 0x88000000 to the physical VRAM/RAM windows.
    //
    // Inline for the same reason as the AOT fast paths below: it is one AND, and
    // Runtime::invoke_chained_call performs it on every translated guest call
    // from 234 separate translation units, where an out-of-line accessor is a
    // real call into psprecomp_core.
    [[nodiscard]] static constexpr std::uint32_t canonical(std::uint32_t address) noexcept {
        return address & 0x1FFFFFFFu;
    }
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t length = 1u) const noexcept;

    // cached AOT memory view. Generated units contain hundreds to
    // thousands of guest loads/stores each. Calling the inline GuestMemory
    // accessors still asks the optimizer to rediscover ram_data_, three limits
    // and the immutable write-watch flag at every static site. Materialize those
    // values once when a unit is entered, then keep them as ordinary locals that
    // MSVC/LTCG can retain in registers across the unit's basic blocks.
    //
    // The underlying RAM/VRAM vectors never resize after construction and
    // write_watch_enabled_ is fixed by the constructor, so this view remains
    // valid across nested AOT/HLE calls. Slow/VRAM paths delegate to the owning
    // GuestMemory and preserve the original validation/watch behavior.
    class AotFastView {
    public:
        // Distinct types for the generated code's word accesses and for the
        // page table: with strict aliasing GCC then knows a guest word store
        // changes neither the CPU context (guest registers stay in host
        // registers across it) nor the page table (one translation serves
        // every access off a base register). Byte, half and quad accesses
        // stay char-based (they alias everything, guest words included).
        // (A struct around a uint32_t is not enough: GCC gives the member
        // access the alias set of uint32_t, the type of the guest registers.
        // The word is a 32-bit integer type distinct from uint32_t, the page
        // entry a pointer, neither of which the CPU context holds.)
        using GuestScalar = std::conditional_t<sizeof(unsigned long) == 4u && !std::is_same_v<std::uint32_t, unsigned long>,
                                               unsigned long, std::uint32_t>;
        struct GuestWord { GuestScalar value; };
        struct PageEntry { std::uint8_t *offset; };
        static_assert(sizeof(GuestWord) == 4u && sizeof(PageEntry) == sizeof(std::uintptr_t));
#if defined(PSPRECOMP_AOT_PAGE_TABLE)
        // Page-table build: every canonical 2 MiB page maps to host memory
        // (RAM, the 2 MiB VRAM for each VRAM mirror, or a scratch page for
        // addresses the PSP has no memory at), so an access is a table load
        // and an add with no test and no call. With no call on the hot path
        // the generated code can keep guest registers in host registers.
        // Write watches are not supported in this build. An entry holds its
        // page's host address minus the page's guest address, for every 2 MiB
        // of the 32-bit space (mirrors included), so the host pointer is
        // entry + address: a shift, a table load and the access itself.
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t *page_pointer(std::uint32_t address) const noexcept {
            return pages_[address >> 21u].offset + address;
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t aot_load8(std::uint32_t address) const {
            return *page_pointer(address);
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint16_t aot_load16(std::uint32_t address) const {
            return GuestMemory::read_le16(page_pointer(address));
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load32(std::uint32_t address) const {
            return reinterpret_cast<const GuestWord *>(page_pointer(address))->value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store8(std::uint32_t address, std::uint8_t value) const {
            *page_pointer(address) = value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store16(std::uint32_t address, std::uint16_t value) const {
            GuestMemory::write_le16(page_pointer(address), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store32(std::uint32_t address, std::uint32_t value) const {
            reinterpret_cast<GuestWord *>(page_pointer(address))->value = value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_load_quad(std::uint32_t address, float *value) const {
            const GuestWord *words = reinterpret_cast<const GuestWord *>(page_pointer(address));
            for (std::uint32_t i = 0; i < 4u; ++i) value[i] = std::bit_cast<float>(words[i].value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_quad(std::uint32_t address, const float *value) const {
            GuestWord *words = reinterpret_cast<GuestWord *>(page_pointer(address));
            for (std::uint32_t i = 0; i < 4u; ++i) words[i].value = std::bit_cast<std::uint32_t>(value[i]);
        }
#else
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t aot_load8(std::uint32_t address) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
            if (offset <= ram_limit8_) return ram_data_[offset];
            return owner_->aot_load8_slow(address);
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint16_t aot_load16(std::uint32_t address) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
            if (offset <= ram_limit16_) return GuestMemory::read_le16(ram_data_ + offset);
            return owner_->aot_load16_slow(address);
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load32(std::uint32_t address) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
            if (offset <= ram_limit32_) return GuestMemory::read_le32(ram_data_ + offset);
            return owner_->aot_load32_slow(address);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store8(std::uint32_t address, std::uint8_t value) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
            if (offset <= ram_limit8_) {
#else
            if (!write_watch_enabled_ && offset <= ram_limit8_) {
#endif
                ram_data_[offset] = value;
                return;
            }
            owner_->aot_store8_slow(address, value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store16(std::uint32_t address, std::uint16_t value) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
            if (offset <= ram_limit16_) {
#else
            if (!write_watch_enabled_ && offset <= ram_limit16_) {
#endif
                GuestMemory::write_le16(ram_data_ + offset, value);
                return;
            }
            owner_->aot_store16_slow(address, value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store32(std::uint32_t address, std::uint32_t value) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
            if (offset <= ram_limit32_) {
#else
            if (!write_watch_enabled_ && offset <= ram_limit32_) {
#endif
                GuestMemory::write_le32(ram_data_ + offset, value);
                return;
            }
            owner_->aot_store32_slow(address, value);
        }
        // LV.Q/SV.Q: one range check covers the four words when all of them are
        // in main RAM; anything else takes the per-word path above.
        PSPRECOMP_MEMORY_FAST_PATH void aot_load_quad(std::uint32_t address, float *value) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
            if (ram_limit32_ >= 12u && offset <= ram_limit32_ - 12u) {
                for (std::uint32_t i = 0; i < 4u; ++i)
                    value[i] = std::bit_cast<float>(GuestMemory::read_le32(ram_data_ + offset + i * 4u));
                return;
            }
            owner_->aot_load_quad_slow(address, value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_quad(std::uint32_t address, const float *value) const {
            const std::uint32_t offset = ram_offset_of_fast(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
            if (ram_limit32_ >= 12u && offset <= ram_limit32_ - 12u) {
#else
            if (!write_watch_enabled_ && ram_limit32_ >= 12u && offset <= ram_limit32_ - 12u) {
#endif
                for (std::uint32_t i = 0; i < 4u; ++i)
                    GuestMemory::write_le32(ram_data_ + offset + i * 4u, std::bit_cast<std::uint32_t>(value[i]));
                return;
            }
            owner_->aot_store_quad_slow(address, value);
        }
#endif

        // Base register + constant offset (the MIPS addressing mode). In the
        // page-table build the base alone is translated, so accesses at
        // several offsets of one base register share the translation (one
        // host load or store each). Host memory is contiguous across every
        // RAM page of one mirror, so this matches page_pointer(base + offset)
        // unless the offset crosses out of the region the base is in.
#if defined(PSPRECOMP_AOT_PAGE_TABLE)
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t *based_pointer(std::uint32_t base, std::int32_t offset) const noexcept {
#if defined(PSPRECOMP_AOT_CHECK_BASED)
            check_based(base, offset);
#endif
            return pages_[base >> 21u].offset + base + offset;
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t aot_load8_at(std::uint32_t base, std::int32_t offset) const {
            return *based_pointer(base, offset);
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint16_t aot_load16_at(std::uint32_t base, std::int32_t offset) const {
            return GuestMemory::read_le16(based_pointer(base, offset));
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load32_at(std::uint32_t base, std::int32_t offset) const {
            return reinterpret_cast<const GuestWord *>(based_pointer(base, offset))->value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store8_at(std::uint32_t base, std::int32_t offset, std::uint8_t value) const {
            *based_pointer(base, offset) = value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store16_at(std::uint32_t base, std::int32_t offset, std::uint16_t value) const {
            GuestMemory::write_le16(based_pointer(base, offset), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store32_at(std::uint32_t base, std::int32_t offset, std::uint32_t value) const {
            reinterpret_cast<GuestWord *>(based_pointer(base, offset))->value = value;
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_load_quad_at(std::uint32_t base, std::int32_t offset, float *value) const {
            const GuestWord *words = reinterpret_cast<const GuestWord *>(based_pointer(base, offset));
            for (std::uint32_t i = 0; i < 4u; ++i) value[i] = std::bit_cast<float>(words[i].value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_quad_at(std::uint32_t base, std::int32_t offset, const float *value) const {
            GuestWord *words = reinterpret_cast<GuestWord *>(based_pointer(base, offset));
            for (std::uint32_t i = 0; i < 4u; ++i) words[i].value = std::bit_cast<std::uint32_t>(value[i]);
        }
#if defined(PSPRECOMP_AOT_CHECK_BASED)
        void check_based(std::uint32_t base, std::int32_t offset) const noexcept;
#endif
#endif
        // LWL/LWR (unaligned word halves), inline: as out-of-line GuestMemory
        // calls they made GCC store every guest register to the context before
        // each one (6 per triangle in the collision loop 0x08A3CE30). Same
        // arithmetic as GuestMemory::aot_load_word_left/right.
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load_word_left(std::uint32_t address, std::uint32_t existing) const {
            const std::uint32_t shift = (address & 3u) * 8u;
            const std::uint32_t memory_word = aot_load32(address & ~3u);
            return (existing & (0x00FFFFFFu >> shift)) | (memory_word << (24u - shift));
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load_word_right(std::uint32_t address, std::uint32_t existing) const {
            const std::uint32_t shift = (address & 3u) * 8u;
            const std::uint32_t memory_word = aot_load32(address & ~3u);
            return (existing & (0xFFFFFF00u << (24u - shift))) | (memory_word >> shift);
        }
        // SWL/SWR, inline for the same reason (GuestMemory::aot_store_word_left/right).
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_word_left(std::uint32_t address, std::uint32_t value) const {
            const std::uint32_t shift = (address & 3u) * 8u;
            const std::uint32_t aligned = address & ~3u;
            const std::uint32_t memory_word = aot_load32(aligned);
            aot_store32(aligned, (value >> (24u - shift)) | (memory_word & (0xFFFFFF00u << shift)));
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_word_right(std::uint32_t address, std::uint32_t value) const {
            const std::uint32_t shift = (address & 3u) * 8u;
            const std::uint32_t aligned = address & ~3u;
            const std::uint32_t memory_word = aot_load32(aligned);
            aot_store32(aligned, (value << shift) | (memory_word & (0x00FFFFFFu >> (24u - shift))));
        }
#if defined(PSPRECOMP_AOT_PAGE_TABLE)
#else
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t aot_load8_at(std::uint32_t base, std::int32_t offset) const {
            return aot_load8(base + static_cast<std::uint32_t>(offset));
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint16_t aot_load16_at(std::uint32_t base, std::int32_t offset) const {
            return aot_load16(base + static_cast<std::uint32_t>(offset));
        }
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load32_at(std::uint32_t base, std::int32_t offset) const {
            return aot_load32(base + static_cast<std::uint32_t>(offset));
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store8_at(std::uint32_t base, std::int32_t offset, std::uint8_t value) const {
            aot_store8(base + static_cast<std::uint32_t>(offset), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store16_at(std::uint32_t base, std::int32_t offset, std::uint16_t value) const {
            aot_store16(base + static_cast<std::uint32_t>(offset), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store32_at(std::uint32_t base, std::int32_t offset, std::uint32_t value) const {
            aot_store32(base + static_cast<std::uint32_t>(offset), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_load_quad_at(std::uint32_t base, std::int32_t offset, float *value) const {
            aot_load_quad(base + static_cast<std::uint32_t>(offset), value);
        }
        PSPRECOMP_MEMORY_FAST_PATH void aot_store_quad_at(std::uint32_t base, std::int32_t offset, const float *value) const {
            aot_store_quad(base + static_cast<std::uint32_t>(offset), value);
        }
#endif

    private:
        friend class GuestMemory;
        AotFastView(GuestMemory *owner, std::uint8_t *ram_data,
                    std::uint32_t limit8, std::uint32_t limit16,
                    std::uint32_t limit32, bool write_watch, const std::uintptr_t *pages) noexcept
            : owner_(owner), ram_data_(ram_data), ram_limit8_(limit8),
              ram_limit16_(limit16), ram_limit32_(limit32),
              write_watch_enabled_(write_watch), pages_(reinterpret_cast<const PageEntry *>(pages)) {}
        [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH static
#if !defined(PSPRECOMP_COUNT_MIRROR_ACCESSES)
            constexpr
#endif
            std::uint32_t ram_offset_of_fast(std::uint32_t address) noexcept {
#if defined(PSPRECOMP_COUNT_MIRROR_ACCESSES)
            // Measurement build: how many generated accesses use a mirror alias.
            ++aot_access_count;
            if ((address >> 29u) != 0u) ++aot_mirror_count;
#endif
            return (address & 0x1FFFFFFFu) - GuestMemory::kPhysicalBase;
        }
        GuestMemory *owner_{};
        std::uint8_t *ram_data_{};
        std::uint32_t ram_limit8_{};
        std::uint32_t ram_limit16_{};
        std::uint32_t ram_limit32_{};
        bool write_watch_enabled_{};
        const PageEntry *pages_{};
    };

    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH AotFastView aot_fast_view() noexcept {
        return AotFastView(this, ram_data_, ram_limit8_, ram_limit16_, ram_limit32_,
                           write_watch_enabled_, aot_pages_.data());
    }

    // Fast paths used only by statically generated AOT code. They retain
    // strict fallback behavior for invalid/cross-boundary accesses and write watches.
    //
    // These are defined inline because the generated units are separate
    // translation units linking against psprecomp_core without LTCG: an
    // out-of-line accessor turned every guest load and store into a real call,
    // and roughly a third of the translated instructions are memory accesses.
    // The inline body covers only main RAM, which is where the overwhelming
    // majority of guest traffic goes.  EDRAM, out-of-range and write-watched
    // accesses fall through to the out-of-line helpers, which keep exactly the
    // behavior these functions had when they lived entirely in the .cpp.
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint8_t aot_load8(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of(address);
        if (offset <= ram_limit8_) return ram_data_[offset];
        return aot_load8_slow(address);
    }
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint16_t aot_load16(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of(address);
        if (offset <= ram_limit16_) return read_le16(ram_data_ + offset);
        return aot_load16_slow(address);
    }
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH std::uint32_t aot_load32(std::uint32_t address) const {
        const std::uint32_t offset = ram_offset_of(address);
        if (offset <= ram_limit32_) return read_le32(ram_data_ + offset);
        return aot_load32_slow(address);
    }
    [[nodiscard]] std::uint32_t aot_load_word_left(std::uint32_t address, std::uint32_t existing) const;
    [[nodiscard]] std::uint32_t aot_load_word_right(std::uint32_t address, std::uint32_t existing) const;
    PSPRECOMP_MEMORY_FAST_PATH void aot_store8(std::uint32_t address, std::uint8_t value) {
        const std::uint32_t offset = ram_offset_of(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit8_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit8_) {
#endif
            ram_data_[offset] = value;
            return;
        }
        aot_store8_slow(address, value);
    }
    PSPRECOMP_MEMORY_FAST_PATH void aot_store16(std::uint32_t address, std::uint16_t value) {
        const std::uint32_t offset = ram_offset_of(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit16_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit16_) {
#endif
            write_le16(ram_data_ + offset, value);
            return;
        }
        aot_store16_slow(address, value);
    }
    PSPRECOMP_MEMORY_FAST_PATH void aot_store32(std::uint32_t address, std::uint32_t value) {
        const std::uint32_t offset = ram_offset_of(address);
#if defined(PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH)
        if (offset <= ram_limit32_) {
#else
        if (!write_watch_enabled_ && offset <= ram_limit32_) {
#endif
            write_le32(ram_data_ + offset, value);
            return;
        }
        aot_store32_slow(address, value);
    }
    PSPRECOMP_MEMORY_FAST_PATH void aot_load_quad(std::uint32_t address, float *value) {
        aot_fast_view().aot_load_quad(address, value);
    }
    PSPRECOMP_MEMORY_FAST_PATH void aot_store_quad(std::uint32_t address, const float *value) {
        aot_fast_view().aot_store_quad(address, value);
    }
    void aot_store_word_left(std::uint32_t address, std::uint32_t value);
    void aot_store_word_right(std::uint32_t address, std::uint32_t value);

    // DEFLATE/LZ-style forward overlap copy. Unlike memmove, bytes written at
    // the destination become immediately available as source bytes, so a short
    // match (for example distance=1) can expand to a long repeated run.
    // Generated AOT can use this for verified overlap-aware match-copy loops such as
    // hot guest loops instead of executing one translated load/store per byte.
    void aot_copy_lz_match(std::uint32_t destination, std::uint32_t source,
                           std::uint32_t length);

    // Direct pointer to `length` contiguous bytes, or nullptr when the range is
    // not wholly inside one region.  The software rasterizer resolves the frame
    // and depth buffers once per primitive and then indexes raw memory, instead
    // of paying canonicalization plus a bounds check on every pixel.  Callers
    // must keep the range valid; nothing here is revalidated afterwards.
    [[nodiscard]] std::uint8_t *raw_pointer(std::uint32_t address, std::size_t length) noexcept;
    [[nodiscard]] const std::uint8_t *raw_pointer(std::uint32_t address, std::size_t length) const noexcept;

    [[nodiscard]] std::uint8_t load8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t load16(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t load32(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t load_word_left(std::uint32_t address, std::uint32_t existing) const;
    [[nodiscard]] std::uint32_t load_word_right(std::uint32_t address, std::uint32_t existing) const;
    void store8(std::uint32_t address, std::uint8_t value);
    void store16(std::uint32_t address, std::uint16_t value);
    void store32(std::uint32_t address, std::uint32_t value);
    void store_word_left(std::uint32_t address, std::uint32_t value);
    void store_word_right(std::uint32_t address, std::uint32_t value);
    void memory_barrier() const noexcept;
    void copy_in(std::uint32_t address, std::span<const std::uint8_t> data);
    void copy_out(std::uint32_t address, std::span<std::uint8_t> data) const;
    void zero(std::uint32_t address, std::size_t length);

    [[nodiscard]] std::string read_c_string(std::uint32_t address, std::size_t max_length = 256u) const;
    [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t> &vram_bytes() const noexcept;

private:
    enum class Region { Vram, Ram };
    struct ResolvedAddress {
        Region region;
        std::size_t offset;
    };

    [[nodiscard]] ResolvedAddress resolve(std::uint32_t address, std::size_t length) const;
    [[nodiscard]] bool is_vram_window(std::uint32_t canonical_address) const noexcept;
    [[nodiscard]] std::size_t vram_offset(std::uint32_t canonical_address) const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t> &region_bytes(Region region) const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> &region_bytes(Region region) noexcept;

    // Canonicalize and rebase in one step.  An address below kPhysicalBase --
    // EDRAM included -- wraps to a value far above any RAM size, so a single
    // unsigned compare rejects it along with every out-of-range access.
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH static std::uint32_t ram_offset_of(std::uint32_t address) noexcept {
        return (address & 0x1FFFFFFFu) - kPhysicalBase;
    }

    // Guest memory is little-endian, so on a little-endian host these are the
    // same bytes the previous per-byte assembly produced, in one access.
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH static std::uint16_t read_le16(const std::uint8_t *source) noexcept {
        std::uint16_t value{};
        std::memcpy(&value, source, sizeof(value));
        if constexpr (std::endian::native == std::endian::big)
            value = static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
        return value;
    }
    [[nodiscard]] PSPRECOMP_MEMORY_FAST_PATH static std::uint32_t read_le32(const std::uint8_t *source) noexcept {
#if defined(__GNUC__)
        // A typed word access keeps GCC from assuming that guest RAM aliases the
        // float VFPU/FPU state (memcpy is a char access that aliases everything).
        // Byte and halfword accesses stay char accesses, so mixed-size guest
        // traffic is still ordered correctly.
        using guest_word = std::uint32_t __attribute__((aligned(1)));
        std::uint32_t value = *reinterpret_cast<const guest_word *>(source);
#else
        std::uint32_t value{};
        std::memcpy(&value, source, sizeof(value));
#endif
        if constexpr (std::endian::native == std::endian::big)
            value = ((value >> 24u) & 0x000000FFu) | ((value >> 8u) & 0x0000FF00u) |
                    ((value << 8u) & 0x00FF0000u) | ((value << 24u) & 0xFF000000u);
        return value;
    }
    PSPRECOMP_MEMORY_FAST_PATH static void write_le16(std::uint8_t *destination, std::uint16_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            value = static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
        std::memcpy(destination, &value, sizeof(value));
    }
    PSPRECOMP_MEMORY_FAST_PATH static void write_le32(std::uint8_t *destination, std::uint32_t value) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            value = ((value >> 24u) & 0x000000FFu) | ((value >> 8u) & 0x0000FF00u) |
                    ((value << 8u) & 0x00FF0000u) | ((value << 24u) & 0xFF000000u);
#if defined(__GNUC__)
        using guest_word = std::uint32_t __attribute__((aligned(1)));
        *reinterpret_cast<guest_word *>(destination) = value;
#else
        std::memcpy(destination, &value, sizeof(value));
#endif
    }

    [[nodiscard]] std::uint8_t aot_load8_slow(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t aot_load16_slow(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t aot_load32_slow(std::uint32_t address) const;
    // Per-word fallback for a quad that is not entirely in main RAM (or is watched).
    void aot_load_quad_slow(std::uint32_t address, float *value);
    void aot_store_quad_slow(std::uint32_t address, const float *value);
    void aot_store8_slow(std::uint32_t address, std::uint8_t value);
    void aot_store16_slow(std::uint32_t address, std::uint16_t value);
    void aot_store32_slow(std::uint32_t address, std::uint32_t value);

    std::vector<std::uint8_t> vram_;
    std::vector<std::uint8_t> bytes_;
    // AotFastView page table (PSPRECOMP_AOT_PAGE_TABLE): for each 2 MiB page of
    // the 32-bit guest space, host address of the page minus its guest
    // address; pages with no PSP memory share scratch_page_.
    std::vector<std::uint8_t> scratch_page_;
    std::vector<std::uintptr_t> aot_pages_;
    // Cached view of bytes_ for the inline fast paths.  Neither region is ever
    // resized after construction, so these stay valid for the object's life.
    //
    // Deliberately not __restrict.  It was tried on the theory that aliasing
    // against the AllegrexContext was forcing the guest register file to spill
    // on every memory instruction: measured on the 1500-vblank route it moved
    // nothing (0.599 us/dispatch against 0.595 without it), and it is not even
    // sound here -- this pointer aliases bytes_ below, which other members of
    // this class access directly.
    std::uint8_t *ram_data_{};
    std::uint32_t ram_limit8_{};
    std::uint32_t ram_limit16_{};
    std::uint32_t ram_limit32_{};
    bool write_watch_enabled_{};
};

} // namespace psprecomp
