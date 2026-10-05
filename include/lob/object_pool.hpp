// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// A preallocated, fixed-capacity slab of `T` with an intrusive free list.
//
// The whole point is that the matching hot path never calls the allocator.
// Capacity is reserved once at construction; acquire() and release() are a
// handful of instructions each and cannot fail except by exhausting the slab,
// which is reported rather than papered over with a fallback allocation.
//
// Handles are 32-bit indices instead of pointers so that nodes can hold links
// to each other in 4 bytes, and so that the slab stays position independent.
//
// The free list is threaded through the storage of the free objects themselves,
// so it costs no extra memory. A freed slot's first 4 bytes hold the index of
// the next free slot.
// ---------------------------------------------------------------------------
template <class T>
class ObjectPool {
    static_assert(sizeof(T) >= sizeof(Handle),
                  "pooled objects must be large enough to thread the free list");
    static_assert(alignof(T) >= alignof(Handle),
                  "pooled objects must be aligned enough to thread the free list");

public:
    ObjectPool() = default;

    explicit ObjectPool(std::size_t capacity) { reserve(capacity); }

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&)                 = default;
    ObjectPool& operator=(ObjectPool&&)      = default;

    // Allocates the slab. Must be called before any acquire(). Calling it twice
    // is a logic error rather than a resize: growing would invalidate every
    // outstanding handle's backing address mid-flight.
    void reserve(std::size_t capacity) {
        if (!storage_.empty()) throw std::logic_error("ObjectPool::reserve called twice");
        if (capacity == 0 || capacity >= kNullHandle)
            throw std::invalid_argument("ObjectPool capacity out of range");

        storage_.resize(capacity);
        capacity_ = capacity;

        // Thread the free list front to back so the first allocations walk
        // forward through memory and the prefetcher gets an easy ride.
        for (std::size_t i = 0; i + 1 < capacity; ++i)
            next_free_slot(static_cast<Handle>(i)) = static_cast<Handle>(i + 1);
        next_free_slot(static_cast<Handle>(capacity - 1)) = kNullHandle;

        free_head_ = 0;
        live_      = 0;
        high_water_ = 0;
    }

    // Returns kNullHandle when the slab is exhausted. Callers on the order-entry
    // path translate that into a reject; nothing in the engine treats it as
    // fatal, because a full book is an operational condition, not a bug.
    [[nodiscard]] Handle acquire() noexcept {
        const Handle h = free_head_;
        if (h == kNullHandle) return kNullHandle;
        free_head_ = next_free_slot(h);
        ++live_;
        if (live_ > high_water_) high_water_ = live_;
        return h;
    }

    void release(Handle h) noexcept {
        assert(h < capacity_ && "releasing a handle outside the slab");
        assert(live_ > 0 && "double release");
        next_free_slot(h) = free_head_;
        free_head_        = h;
        --live_;
    }

    [[nodiscard]] T&       operator[](Handle h) noexcept       { assert(h < capacity_); return object(h); }
    [[nodiscard]] const T& operator[](Handle h) const noexcept { assert(h < capacity_); return object(h); }

    [[nodiscard]] std::size_t capacity()   const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live()       const noexcept { return live_; }
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::size_t free_count() const noexcept { return capacity_ - live_; }

    // Fault the whole slab in and warm the TLB, so the first thousand orders of
    // the session do not pay page faults that later orders never see. Worth
    // doing before a latency measurement or a trading session.
    void prefault() noexcept {
        for (auto& slot : storage_) slot.bytes[0] = std::byte{0};
        // Re-thread: the write above clobbered the free-list links.
        for (std::size_t i = 0; i + 1 < capacity_; ++i)
            next_free_slot(static_cast<Handle>(i)) = static_cast<Handle>(i + 1);
        if (capacity_) next_free_slot(static_cast<Handle>(capacity_ - 1)) = kNullHandle;
        free_head_ = 0;
        live_      = 0;
    }

    void reset() noexcept {
        for (std::size_t i = 0; i + 1 < capacity_; ++i)
            next_free_slot(static_cast<Handle>(i)) = static_cast<Handle>(i + 1);
        if (capacity_) next_free_slot(static_cast<Handle>(capacity_ - 1)) = kNullHandle;
        free_head_  = capacity_ ? 0 : kNullHandle;
        live_       = 0;
        high_water_ = 0;
    }

private:
    // Raw storage: we never construct or destroy T, because T is required to be
    // a trivial aggregate that the book fully initialises on acquire.
    struct alignas(T) Slot { std::byte bytes[sizeof(T)]; };

    [[nodiscard]] T& object(Handle h) noexcept {
        return *std::launder(reinterpret_cast<T*>(storage_[h].bytes));
    }
    [[nodiscard]] const T& object(Handle h) const noexcept {
        return *std::launder(reinterpret_cast<const T*>(storage_[h].bytes));
    }
    [[nodiscard]] Handle& next_free_slot(Handle h) noexcept {
        return *reinterpret_cast<Handle*>(storage_[h].bytes);
    }

    std::vector<Slot> storage_;
    std::size_t       capacity_   = 0;
    std::size_t       live_       = 0;
    std::size_t       high_water_ = 0;
    Handle            free_head_  = kNullHandle;
};

}  // namespace lob
