// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// OrderId -> Handle, as a preallocated open-addressing table.
//
// Every cancel and every replace starts here, so this lookup sits directly on
// the latency path and must not allocate, chain through pointers, or rehash
// mid-session. std::unordered_map does all three.
//
// Linear probing with backward-shift deletion. Tombstones are deliberately
// avoided: a trading day is overwhelmingly add/cancel churn, and a tombstoned
// table degrades toward a linear scan over the session. Backward-shift keeps
// every probe sequence contiguous, so the table performs the same at the close
// as it did at the open.
//
// Capacity is rounded up to a power of two and the table refuses to exceed a
// 0.7 load factor, which is where linear probing starts to lose its cache
// advantage.
// ---------------------------------------------------------------------------
class OrderIndex {
public:
    OrderIndex() = default;
    explicit OrderIndex(std::size_t expected_orders) { reserve(expected_orders); }

    void reserve(std::size_t expected_orders) {
        std::size_t cap = std::bit_ceil(std::max<std::size_t>(
            16, static_cast<std::size_t>(static_cast<double>(expected_orders) / kMaxLoad) + 1));
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        size_ = 0;
    }

    [[nodiscard]] std::size_t size()     const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] bool        empty()    const noexcept { return size_ == 0; }

    void clear() noexcept {
        for (auto& s : slots_) s.handle = kNullHandle;
        size_ = 0;
    }

    // Returns false if the id is already present (exchanges must reject a
    // duplicate order reference) or the table is full.
    bool insert(OrderId id, Handle h) noexcept {
        assert(h != kNullHandle);
        if (size_ + 1 > (slots_.size() * 7) / 10) return false;

        std::size_t i = slot_for(id);
        while (slots_[i].handle != kNullHandle) {
            if (slots_[i].id == id) return false;
            i = (i + 1) & mask_;
        }
        slots_[i].id     = id;
        slots_[i].handle = h;
        ++size_;
        return true;
    }

    [[nodiscard]] Handle find(OrderId id) const noexcept {
        std::size_t i = slot_for(id);
        while (slots_[i].handle != kNullHandle) {
            if (slots_[i].id == id) return slots_[i].handle;
            i = (i + 1) & mask_;
        }
        return kNullHandle;
    }

    // Rewrites the handle for an existing id. Used by replace, which moves an
    // order to a new pool slot but may keep its reference number.
    bool update(OrderId id, Handle h) noexcept {
        std::size_t i = slot_for(id);
        while (slots_[i].handle != kNullHandle) {
            if (slots_[i].id == id) { slots_[i].handle = h; return true; }
            i = (i + 1) & mask_;
        }
        return false;
    }

    bool erase(OrderId id) noexcept {
        std::size_t i = slot_for(id);
        while (slots_[i].handle != kNullHandle) {
            if (slots_[i].id == id) { erase_at(i); return true; }
            i = (i + 1) & mask_;
        }
        return false;
    }

private:
    struct Slot {
        OrderId id     = 0;
        Handle  handle = kNullHandle;  // kNullHandle marks the slot empty
    };

    static constexpr double kMaxLoad = 0.7;

    // Fibonacci hashing. ITCH order reference numbers are close to sequential,
    // and multiply-shift spreads a sequential key across the table far better
    // than a bare modulo, which would map consecutive ids to consecutive slots
    // and turn every collision into a long run.
    [[nodiscard]] std::size_t slot_for(OrderId id) const noexcept {
        constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
        std::uint64_t h = id * kGolden;
        h ^= h >> 29;
        return static_cast<std::size_t>(h) & mask_;
    }

    // Backward-shift deletion: walk forward from the hole and pull back any
    // entry that probed past it, so no probe sequence is ever broken and no
    // tombstone is needed.
    void erase_at(std::size_t hole) noexcept {
        slots_[hole].handle = kNullHandle;
        --size_;

        std::size_t i = (hole + 1) & mask_;
        while (slots_[i].handle != kNullHandle) {
            const std::size_t ideal = slot_for(slots_[i].id);
            // Does `i` sit at or after the hole in its probe sequence? If the
            // ideal slot is not strictly inside (hole, i], moving it back is safe.
            const bool can_move = distance(ideal, hole) < distance(ideal, i);
            if (can_move) {
                slots_[hole] = slots_[i];
                slots_[i].handle = kNullHandle;
                hole = i;
            }
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] std::size_t distance(std::size_t from, std::size_t to) const noexcept {
        return (to - from) & mask_;
    }

    std::vector<Slot> slots_;
    std::size_t       mask_ = 0;
    std::size_t       size_ = 0;
};

}  // namespace lob
