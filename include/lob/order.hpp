// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "lob/object_pool.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// A resting order.
//
// Laid out to fit in exactly 32 bytes so two orders share a 64-byte cache line
// and a queue walk during a sweep touches half as many lines. The static_assert
// below is load bearing: if a field is added that pushes this over 32 bytes the
// build fails rather than silently halving match throughput.
//
// `next`/`prev` are pool indices, threaded by IntrusiveList.
//
// `loc` is doing double duty, which is what keeps the struct at 32 bytes. For
// an order inside the book's dense price band it is the tick index -- the
// array subscript. For an order outside the band (see BookSide) it is the raw
// wire price instead. The `kFar` flag says which. Real ITCH prices top out at
// $199,999.9900, comfortably inside a signed 32-bit value, so a price fits the
// same field a tick does.
// ---------------------------------------------------------------------------
struct Order {
    Handle       next;   // intrusive queue link (pool index)
    Handle       prev;   // intrusive queue link (pool index)
    Qty          qty;    // shares still resting; 0 means fully filled
    std::int32_t loc;    // tick index, or raw price when kFar is set
    OrderId      id;     // client / exchange order reference
    Seq          seq;    // arrival sequence, for audit and tie-break assertions
    Side         side;
    Tif          tif;
    std::uint8_t flags;
    std::uint8_t _pad;

    enum : std::uint8_t { kFar = 1u << 0 };

    [[nodiscard]] bool is_far() const noexcept { return (flags & kFar) != 0; }
};

static_assert(sizeof(Order) == 32, "Order must stay at 32 bytes: two per cache line");
static_assert(alignof(Order) == 8);

// Link accessors for IntrusiveList.
struct OrderLinks {
    static Handle& next(Order& o) noexcept { return o.next; }
    static Handle& prev(Order& o) noexcept { return o.prev; }
};

using OrderPool = ObjectPool<Order>;

}  // namespace lob
