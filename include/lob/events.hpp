// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include "lob/types.hpp"

namespace lob {

// Outcome of an order-entry request. Every failure mode is named, because
// "it didn't work" is not an acceptable answer to an order entry gateway.
enum class Status : std::uint8_t {
    Ok = 0,
    DuplicateOrderId,   // reference number already live in the book
    UnknownOrderId,     // cancel/replace named an order we do not have
    PriceOutOfRange,    // price is outside the book's configured tick band
    PriceNotOnTick,     // price is not a multiple of the tick size
    InvalidQuantity,    // zero, or a reduce that would increase size
    PoolExhausted,      // order pool is full: back pressure, not a bug
    IndexFull,          // order index is full
    FillOrKillUnfilled, // FOK could not be fully filled, so nothing was done
    NoLiquidity,        // market order with an empty opposite side
};

[[nodiscard]] constexpr std::string_view to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok:                 return "OK";
        case Status::DuplicateOrderId:   return "DUPLICATE_ORDER_ID";
        case Status::UnknownOrderId:     return "UNKNOWN_ORDER_ID";
        case Status::PriceOutOfRange:    return "PRICE_OUT_OF_RANGE";
        case Status::PriceNotOnTick:     return "PRICE_NOT_ON_TICK";
        case Status::InvalidQuantity:    return "INVALID_QUANTITY";
        case Status::PoolExhausted:      return "POOL_EXHAUSTED";
        case Status::IndexFull:          return "INDEX_FULL";
        case Status::FillOrKillUnfilled: return "FOK_UNFILLED";
        case Status::NoLiquidity:        return "NO_LIQUIDITY";
    }
    return "?";
}

// A single execution. Price is always the *resting* order's price: the passive
// side sets the terms, which is what price-time priority means in practice and
// is why an aggressive order can receive price improvement.
struct Fill {
    OrderId aggressor_id;
    OrderId resting_id;
    Price   price;
    Qty     qty;
    Side    aggressor_side;
    bool    resting_filled;  // the resting order was fully consumed and removed
};

// Result of submitting an order.
struct SubmitResult {
    Status        status       = Status::Ok;
    OrderId       id           = 0;
    Qty           filled_qty   = 0;   // traded immediately on arrival
    Qty           resting_qty  = 0;   // remainder left on the book
    std::uint32_t fill_count   = 0;
    std::uint64_t notional     = 0;   // sum(price * qty) in Price units

    [[nodiscard]] bool ok()      const noexcept { return status == Status::Ok; }
    [[nodiscard]] bool rested()  const noexcept { return resting_qty > 0; }
    [[nodiscard]] bool traded()  const noexcept { return filled_qty > 0; }
};

// ---------------------------------------------------------------------------
// The engine reports through a sink supplied as a template parameter, so a
// production gateway, a test recorder and a benchmark that wants zero reporting
// overhead all compile to different code with no virtual dispatch on the hot
// path. NullSink exists so the benchmark measures the book, not the reporting.
// ---------------------------------------------------------------------------
struct NullSink {
    void on_fill(const Fill&) noexcept {}
    void on_accepted(OrderId, Side, Price, Qty) noexcept {}
    void on_cancelled(OrderId, Side, Price, Qty) noexcept {}
    void on_replaced(OrderId, OrderId, Side, Price, Qty) noexcept {}
    void on_rejected(OrderId, Status) noexcept {}
};

}  // namespace lob
