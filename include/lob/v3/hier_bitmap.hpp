#pragma once
// n-level, 64-ary hierarchical bitmap over 2^Bits slots, n = ceil(Bits / 6).
// Level 0 is the leaf (one bit per slot); a bit in level k+1 is set iff the
// corresponding 64-bit word of level k is non-zero, so the single top word
// summarises everything. min()/max() walk top -> leaf with one ctz/clz per
// level (n dependent loads); set()/clear() walk leaf -> top and stop as soon as
// a word's emptiness does not change. Non-owning view over zeroed words.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace lob::v3 {

template <unsigned Bits> class HierBitmap {
    static_assert(Bits >= 1 && Bits <= 30);

    static constexpr unsigned kLevels = (Bits + 5) / 6;

    static constexpr auto words_per_level() {
        std::array<std::size_t, kLevels> w{};
        std::size_t                      n = ((std::size_t{1} << Bits) + 63) / 64;
        for (unsigned l = 0; l < kLevels; ++l) {
            w[l] = n;
            n    = (n + 63) / 64;
        }
        return w;
    }
    static constexpr auto kWords = words_per_level();
    static constexpr auto offsets() {
        std::array<std::size_t, kLevels> o{};
        for (unsigned l = 1; l < kLevels; ++l) o[l] = o[l - 1] + kWords[l - 1];
        return o;
    }
    static constexpr auto kOffset = offsets();
    static_assert(kWords[kLevels - 1] == 1, "top level must be one word");

  public:
    static constexpr std::size_t kSize       = std::size_t{1} << Bits;
    static constexpr std::size_t kTotalWords = kOffset[kLevels - 1] + 1;
    static constexpr unsigned    kDepth      = kLevels;

    HierBitmap() = default;
    explicit HierBitmap(std::uint64_t *words) noexcept : w_(words) {}

    [[nodiscard]] auto empty() const noexcept -> bool {
        return w_[kOffset[kLevels - 1]] == 0;
    }

    [[nodiscard]] auto test(std::uint32_t i) const noexcept -> bool {
        return (w_[i >> 6] >> (i & 63)) & 1U;
    }

    void set(std::uint32_t i) noexcept {
        for (unsigned l = 0; l < kLevels; ++l) {
            std::uint64_t &word      = w_[kOffset[l] + (i >> 6)];
            const bool     was_empty = word == 0;
            word |= std::uint64_t{1} << (i & 63);
            if (!was_empty) return;
            i >>= 6;
        }
    }

    void clear(std::uint32_t i) noexcept {
        for (unsigned l = 0; l < kLevels; ++l) {
            std::uint64_t &word = w_[kOffset[l] + (i >> 6)];
            word &= ~(std::uint64_t{1} << (i & 63));
            if (word != 0) return;
            i >>= 6;
        }
    }

    // lowest set slot; precondition: !empty()
    [[nodiscard]] auto min() const noexcept -> std::uint32_t {
        std::uint32_t i = 0;
        for (unsigned l = kLevels; l-- > 0;) {
            const std::uint64_t word = w_[kOffset[l] + i];
            i = (i << 6) | static_cast<std::uint32_t>(std::countr_zero(word));
        }
        return i;
    }

    // highest set slot; precondition: !empty()
    [[nodiscard]] auto max() const noexcept -> std::uint32_t {
        std::uint32_t i = 0;
        for (unsigned l = kLevels; l-- > 0;) {
            const std::uint64_t word = w_[kOffset[l] + i];
            i = (i << 6) | static_cast<std::uint32_t>(63 - std::countl_zero(word));
        }
        return i;
    }

    // every set slot, ascending (snapshots / invariant checks, not hot path)
    template <class F> void for_each(F &&f) const {
        for (std::size_t w = 0; w < kWords[0]; ++w)
            for (std::uint64_t bits = w_[w]; bits != 0; bits &= bits - 1)
                f(static_cast<std::uint32_t>(
                    w * 64 + static_cast<std::size_t>(std::countr_zero(bits))));
    }

    // summary bits agree with the level below (invariant check)
    [[nodiscard]] auto consistent() const noexcept -> bool {
        for (unsigned l = 0; l + 1 < kLevels; ++l)
            for (std::size_t w = 0; w < kWords[l]; ++w) {
                const bool child  = w_[kOffset[l] + w] != 0;
                const bool parent = (w_[kOffset[l + 1] + (w >> 6)] >> (w & 63)) & 1U;
                if (child != parent) return false;
            }
        return true;
    }

    [[nodiscard]] auto leaf_word(std::size_t w) const noexcept -> std::uint64_t {
        return w_[w];
    }
    static constexpr std::size_t kLeafWords = kWords[0];

  private:
    std::uint64_t *w_ = nullptr;
};

} // namespace lob::v3
