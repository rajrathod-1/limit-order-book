// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <compare>
#include <limits>
#include <string_view>

namespace lob {

// ---------------------------------------------------------------------------
// Scalar domain types.
//
// Prices are integral. Nasdaq ITCH 5.0 carries prices as unsigned 32-bit
// fixed-point with four implied decimals (i.e. $12.3400 == 123400), so we keep
// the same representation end-to-end and never introduce a floating point value
// into the hot path. `Ticks` is the *quantised* form of a price: a small dense
// integer suitable for indexing a flat array.
// ---------------------------------------------------------------------------

using Price   = std::int64_t;   // fixed point, `kPriceScale` implied decimals
using Ticks   = std::int32_t;   // price / tick_size, dense array index
using Qty     = std::uint32_t;  // shares
using OrderId = std::uint64_t;  // exchange order reference number
// Per-session arrival counter stamped on each order. 32 bits is deliberate:
// it keeps Order at 32 bytes, and a full Nasdaq day is ~10^8 messages across
// every symbol, so a per-book counter has three orders of magnitude of room.
using Seq     = std::uint32_t;
// Monotonic id for emitted events. Not size constrained, so it stays 64-bit.
using EventSeq = std::uint64_t;
using Handle  = std::uint32_t;  // index into the order pool

inline constexpr int   kPriceDecimals = 4;
inline constexpr Price kPriceScale    = 10000;  // 10^kPriceDecimals

// A sentinel handle. Chosen as all-ones so a zeroed pool is not accidentally
// a valid link, and so `null` survives narrowing to 32 bits.
inline constexpr Handle kNullHandle = std::numeric_limits<Handle>::max();

inline constexpr Ticks kNoTick = std::numeric_limits<Ticks>::min();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

// Time in force.
enum class Tif : std::uint8_t {
    Day = 0,  // rests until cancelled
    Ioc = 1,  // immediate-or-cancel: cross what you can, cancel the remainder
    Fok = 2,  // fill-or-kill: all of it in one go, or nothing at all
};

[[nodiscard]] constexpr std::string_view to_string(Tif t) noexcept {
    switch (t) {
        case Tif::Day: return "DAY";
        case Tif::Ioc: return "IOC";
        case Tif::Fok: return "FOK";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Price <-> tick conversion.
//
// A book is configured with a tick size and a floor price. `TickScale` turns a
// wire price into the dense index used by ArrayBook. Kept as a tiny value type
// so it can live in a register and be inlined into the hot path.
// ---------------------------------------------------------------------------
class TickScale {
public:
    constexpr TickScale() noexcept = default;

    // `tick_size` and `floor_price` are in the same fixed-point units as Price.
    constexpr TickScale(Price tick_size, Price floor_price) noexcept
        : tick_size_(tick_size), floor_(floor_price) {}

    [[nodiscard]] constexpr Price tick_size()   const noexcept { return tick_size_; }
    [[nodiscard]] constexpr Price floor_price() const noexcept { return floor_; }

    // Exact conversion. Returns false when `p` is not on the tick grid or sits
    // below the floor; callers decide whether that is a reject or a spill.
    [[nodiscard]] constexpr bool to_ticks(Price p, Ticks& out) const noexcept {
        if (p < floor_) return false;
        const Price rel = p - floor_;
        if (rel % tick_size_ != 0) return false;
        const Price t = rel / tick_size_;
        if (t > std::numeric_limits<Ticks>::max()) return false;
        out = static_cast<Ticks>(t);
        return true;
    }

    // Round toward the passive side: a buy rounds down, a sell rounds up, so a
    // rounded order is never more aggressive than the price the client sent.
    [[nodiscard]] constexpr Ticks to_ticks_passive(Price p, Side s) const noexcept {
        const Price rel = p - floor_;
        Price t = rel / tick_size_;
        const Price rem = rel % tick_size_;
        if (rem != 0 && s == Side::Sell) ++t;
        if (rem != 0 && rel < 0)         --t;  // C++ truncates toward zero
        return static_cast<Ticks>(t);
    }

    [[nodiscard]] constexpr Price to_price(Ticks t) const noexcept {
        return floor_ + static_cast<Price>(t) * tick_size_;
    }

    friend constexpr bool operator==(const TickScale&, const TickScale&) noexcept = default;

private:
    Price tick_size_ = 100;  // $0.0100 -- the Nasdaq penny tick in ITCH units
    Price floor_     = 0;
};

// The standard US equity grid: one cent ticks from zero.
inline constexpr TickScale kPennyTicks{100, 0};

}  // namespace lob
