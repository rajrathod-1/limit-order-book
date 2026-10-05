// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// A three-level hierarchical bitmap over the tick array: "which price levels
// are currently occupied?"
//
// This is what makes the best-bid/best-ask claim hold up under load. Caching
// the best tick gives O(1) lookup while the touched level stays populated, but
// the moment the best level is fully consumed or cancelled away, something has
// to find the next one. A linear scan of the tick array degrades badly across a
// wide, sparse book -- exactly the shape a real symbol has after the open.
//
// Instead each level of the hierarchy summarises 64 entries of the level below:
//
//     L0: one bit per tick                    (capacity bits)
//     L1: one bit per 64 ticks                (capacity / 64 bits)
//     L2: one bit per 4096 ticks              (capacity / 4096 bits)
//
// A set/clear touches at most 3 words. find_next/find_prev walk at most 3 words
// down and 3 up using count-leading/trailing-zeros, so the search is a fixed
// handful of instructions regardless of how far apart the populated levels are.
// With a 4096-word L2 root that covers 2^24 ticks -- $167k of penny grid --
// which is more range than any single-name book needs.
// ---------------------------------------------------------------------------
class BitsetIndex {
public:
    static constexpr std::size_t kWordBits = 64;

    BitsetIndex() = default;
    explicit BitsetIndex(std::size_t capacity) { reserve(capacity); }

    void reserve(std::size_t capacity) {
        capacity_ = capacity;
        l0_.assign(words_for(capacity), 0);
        l1_.assign(words_for(l0_.size()), 0);
        l2_.assign(words_for(l1_.size()), 0);
    }

    void clear() noexcept {
        std::fill(l0_.begin(), l0_.end(), 0ull);
        std::fill(l1_.begin(), l1_.end(), 0ull);
        std::fill(l2_.begin(), l2_.end(), 0ull);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        assert(i < capacity_);
        return (l0_[i / kWordBits] >> (i % kWordBits)) & 1ull;
    }

    void set(std::size_t i) noexcept {
        assert(i < capacity_);
        const std::size_t w0 = i / kWordBits;
        l0_[w0] |= bit(i % kWordBits);
        const std::size_t w1 = w0 / kWordBits;
        l1_[w1] |= bit(w0 % kWordBits);
        l2_[w1 / kWordBits] |= bit(w1 % kWordBits);
    }

    void clear_bit(std::size_t i) noexcept {
        assert(i < capacity_);
        const std::size_t w0 = i / kWordBits;
        l0_[w0] &= ~bit(i % kWordBits);
        if (l0_[w0] != 0) return;  // summary still valid, stop early

        const std::size_t w1 = w0 / kWordBits;
        l1_[w1] &= ~bit(w0 % kWordBits);
        if (l1_[w1] != 0) return;

        l2_[w1 / kWordBits] &= ~bit(w1 % kWordBits);
    }

    // Lowest set bit at or above `from`, or npos. This is the best *ask* search.
    [[nodiscard]] std::size_t find_first_at_or_after(std::size_t from) const noexcept {
        if (from >= capacity_) return npos;

        std::size_t w0 = from / kWordBits;
        if (std::uint64_t w = l0_[w0] & (~0ull << (from % kWordBits)); w)
            return w0 * kWordBits + static_cast<std::size_t>(std::countr_zero(w));

        // Escalate: find the next non-empty L0 word via L1, then via L2.
        std::size_t w1 = w0 / kWordBits;
        if (std::uint64_t w = l1_[w1] & mask_above(w0 % kWordBits); w)
            return scan_l0(w1 * kWordBits + static_cast<std::size_t>(std::countr_zero(w)));

        for (std::size_t w2 = w1 / kWordBits; w2 < l2_.size(); ++w2) {
            std::uint64_t w = l2_[w2];
            if (w2 == w1 / kWordBits) w &= mask_above(w1 % kWordBits);
            if (!w) continue;
            const std::size_t nw1 = w2 * kWordBits + static_cast<std::size_t>(std::countr_zero(w));
            return scan_l0(nw1 * kWordBits +
                           static_cast<std::size_t>(std::countr_zero(l1_[nw1])));
        }
        return npos;
    }

    // Highest set bit at or below `from`, or npos. This is the best *bid* search.
    [[nodiscard]] std::size_t find_last_at_or_before(std::size_t from) const noexcept {
        if (capacity_ == 0) return npos;
        if (from >= capacity_) from = capacity_ - 1;

        std::size_t w0 = from / kWordBits;
        if (std::uint64_t w = l0_[w0] & mask_below(from % kWordBits); w)
            return w0 * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(w)));

        std::size_t w1 = w0 / kWordBits;
        if (std::uint64_t w = l1_[w1] & mask_strictly_below(w0 % kWordBits); w) {
            const std::size_t nw0 =
                w1 * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(w)));
            return nw0 * kWordBits +
                   (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l0_[nw0])));
        }

        for (std::size_t w2 = w1 / kWordBits + 1; w2-- > 0;) {
            std::uint64_t w = l2_[w2];
            if (w2 == w1 / kWordBits) w &= mask_strictly_below(w1 % kWordBits);
            if (!w) continue;
            const std::size_t nw1 =
                w2 * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(w)));
            const std::size_t nw0 =
                nw1 * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l1_[nw1])));
            return nw0 * kWordBits +
                   (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l0_[nw0])));
        }
        return npos;
    }

    [[nodiscard]] bool any() const noexcept {
        for (std::uint64_t w : l2_) if (w) return true;
        return false;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    static constexpr std::uint64_t bit(std::size_t b) noexcept { return 1ull << b; }
    static constexpr std::size_t words_for(std::size_t bits) noexcept {
        return (bits + kWordBits - 1) / kWordBits;
    }
    // Bits strictly above b, plus b itself excluded / included as named.
    static constexpr std::uint64_t mask_above(std::size_t b) noexcept {
        return b >= kWordBits - 1 ? 0ull : (~0ull << (b + 1));
    }
    static constexpr std::uint64_t mask_below(std::size_t b) noexcept {
        return b >= kWordBits - 1 ? ~0ull : ((1ull << (b + 1)) - 1);
    }
    static constexpr std::uint64_t mask_strictly_below(std::size_t b) noexcept {
        return b == 0 ? 0ull : ((1ull << b) - 1);
    }

    [[nodiscard]] std::size_t scan_l0(std::size_t w0) const noexcept {
        return w0 * kWordBits + static_cast<std::size_t>(std::countr_zero(l0_[w0]));
    }

    std::vector<std::uint64_t> l0_, l1_, l2_;
    std::size_t                capacity_ = 0;
};

}  // namespace lob
