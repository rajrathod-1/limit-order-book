// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace itch {

// ---------------------------------------------------------------------------
// Nasdaq TotalView-ITCH 5.0 wire decoding.
//
// Everything on the wire is big endian and unaligned. Rather than declare
// packed structs and take the address of their members -- which is undefined
// behaviour on a misaligned field and miscompiles under strict alignment -- we
// decode by explicit byte offset through memcpy. Every mainstream compiler
// folds this to a single load plus a byte-swap instruction, so the safe version
// is also the fast one.
//
// Prices are unsigned 32-bit with four implied decimals ($12.3400 -> 123400).
// Timestamps are 48-bit nanoseconds since midnight Eastern.
// ---------------------------------------------------------------------------

// std::byteswap is C++23; this project targets C++20, so provide it. All three
// compilers lower these builtins to a single rev/bswap instruction.
[[nodiscard]] inline std::uint16_t bswap(std::uint16_t v) noexcept { return __builtin_bswap16(v); }
[[nodiscard]] inline std::uint32_t bswap(std::uint32_t v) noexcept { return __builtin_bswap32(v); }
[[nodiscard]] inline std::uint64_t bswap(std::uint64_t v) noexcept { return __builtin_bswap64(v); }

[[nodiscard]] inline std::uint16_t be16(const std::byte* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return bswap(v);
}

[[nodiscard]] inline std::uint32_t be32(const std::byte* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return bswap(v);
}

[[nodiscard]] inline std::uint64_t be64(const std::byte* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return bswap(v);
}

// 48-bit big-endian timestamp.
[[nodiscard]] inline std::uint64_t be48(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(reinterpret_cast<std::byte*>(&v) + 2, p, 6);
    return bswap(v);
}

// Fixed-width, space-padded ASCII field (stock symbols, MPIDs).
[[nodiscard]] inline std::string_view alpha(const std::byte* p, std::size_t n) noexcept {
    const char* c = reinterpret_cast<const char*>(p);
    while (n > 0 && c[n - 1] == ' ') --n;
    return {c, n};
}

// ---------------------------------------------------------------------------
// Message types, as the single ASCII character that leads every message.
// ---------------------------------------------------------------------------
enum class MsgType : char {
    SystemEvent            = 'S',
    StockDirectory         = 'R',
    StockTradingAction     = 'H',
    RegSho                 = 'Y',
    ParticipantPosition    = 'L',
    MwcbDeclineLevel       = 'V',
    MwcbStatus             = 'W',
    IpoQuotingUpdate       = 'K',
    LuldAuctionCollar      = 'J',
    OperationalHalt        = 'h',
    AddOrder               = 'A',
    AddOrderMpid           = 'F',
    OrderExecuted          = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel            = 'X',
    OrderDelete            = 'D',
    OrderReplace           = 'U',
    TradeNonCross          = 'P',
    CrossTrade             = 'Q',
    BrokenTrade            = 'B',
    Noii                   = 'I',
    Rpii                   = 'N',
    DirectListingCapitalRaise = 'O',
};

// Declared length of each message *including* the leading type byte. Zero means
// "unknown to this build" -- the reader falls back to the framing length, so a
// future Nasdaq message type is skipped safely rather than desynchronising the
// stream.
[[nodiscard]] constexpr std::size_t declared_length(char t) noexcept {
    switch (t) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default:  return 0;
    }
}

// ---------------------------------------------------------------------------
// Zero-copy message views. Each wraps the raw bytes and decodes on access, so
// a reader that only cares about `stock_locate` never pays to decode a price.
// ---------------------------------------------------------------------------

// Common 11-byte header shared by every message.
struct Header {
    const std::byte* p;
    [[nodiscard]] char          type()         const noexcept { return static_cast<char>(p[0]); }
    [[nodiscard]] std::uint16_t stock_locate() const noexcept { return be16(p + 1); }
    [[nodiscard]] std::uint16_t tracking()     const noexcept { return be16(p + 3); }
    [[nodiscard]] std::uint64_t timestamp()    const noexcept { return be48(p + 5); }
};

struct SystemEvent : Header {
    [[nodiscard]] char event_code() const noexcept { return static_cast<char>(p[11]); }
};

struct StockDirectory : Header {
    [[nodiscard]] std::string_view stock()          const noexcept { return alpha(p + 11, 8); }
    [[nodiscard]] char          market_category()   const noexcept { return static_cast<char>(p[19]); }
    [[nodiscard]] char          financial_status()  const noexcept { return static_cast<char>(p[20]); }
    [[nodiscard]] std::uint32_t round_lot_size()    const noexcept { return be32(p + 21); }
    [[nodiscard]] char          round_lots_only()   const noexcept { return static_cast<char>(p[25]); }
    [[nodiscard]] char          issue_class()       const noexcept { return static_cast<char>(p[26]); }
    [[nodiscard]] char          authenticity()      const noexcept { return static_cast<char>(p[29]); }
    [[nodiscard]] char          luld_tier()         const noexcept { return static_cast<char>(p[32]); }
};

struct StockTradingAction : Header {
    [[nodiscard]] std::string_view stock()  const noexcept { return alpha(p + 11, 8); }
    [[nodiscard]] char trading_state()      const noexcept { return static_cast<char>(p[19]); }
    [[nodiscard]] std::string_view reason() const noexcept { return alpha(p + 21, 4); }
};

struct AddOrder : Header {
    [[nodiscard]] std::uint64_t order_ref() const noexcept { return be64(p + 11); }
    [[nodiscard]] char          side()      const noexcept { return static_cast<char>(p[19]); }  // 'B' or 'S'
    [[nodiscard]] std::uint32_t shares()    const noexcept { return be32(p + 20); }
    [[nodiscard]] std::string_view stock()  const noexcept { return alpha(p + 24, 8); }
    [[nodiscard]] std::uint32_t price()     const noexcept { return be32(p + 32); }
    // Present only on 'F'; empty on 'A'.
    [[nodiscard]] std::string_view mpid()   const noexcept {
        return type() == 'F' ? alpha(p + 36, 4) : std::string_view{};
    }
};

struct OrderExecuted : Header {
    [[nodiscard]] std::uint64_t order_ref()       const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint32_t executed_shares() const noexcept { return be32(p + 19); }
    [[nodiscard]] std::uint64_t match_number()    const noexcept { return be64(p + 23); }
    // 'C' only: the trade printed at a price other than the order's own.
    [[nodiscard]] char          printable()       const noexcept { return static_cast<char>(p[31]); }
    [[nodiscard]] std::uint32_t exec_price()      const noexcept { return be32(p + 32); }
    [[nodiscard]] bool          has_price()       const noexcept { return type() == 'C'; }
};

struct OrderCancel : Header {
    [[nodiscard]] std::uint64_t order_ref()        const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint32_t cancelled_shares() const noexcept { return be32(p + 19); }
};

struct OrderDelete : Header {
    [[nodiscard]] std::uint64_t order_ref() const noexcept { return be64(p + 11); }
};

struct OrderReplace : Header {
    [[nodiscard]] std::uint64_t original_ref() const noexcept { return be64(p + 11); }
    [[nodiscard]] std::uint64_t new_ref()      const noexcept { return be64(p + 19); }
    [[nodiscard]] std::uint32_t shares()       const noexcept { return be32(p + 27); }
    [[nodiscard]] std::uint32_t price()        const noexcept { return be32(p + 31); }
};

struct TradeNonCross : Header {
    [[nodiscard]] std::uint64_t order_ref()    const noexcept { return be64(p + 11); }
    [[nodiscard]] char          side()         const noexcept { return static_cast<char>(p[19]); }
    [[nodiscard]] std::uint32_t shares()       const noexcept { return be32(p + 20); }
    [[nodiscard]] std::string_view stock()     const noexcept { return alpha(p + 24, 8); }
    [[nodiscard]] std::uint32_t price()        const noexcept { return be32(p + 32); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 36); }
};

struct CrossTrade : Header {
    [[nodiscard]] std::uint64_t shares()       const noexcept { return be64(p + 11); }
    [[nodiscard]] std::string_view stock()     const noexcept { return alpha(p + 19, 8); }
    [[nodiscard]] std::uint32_t price()        const noexcept { return be32(p + 27); }
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 31); }
    [[nodiscard]] char          cross_type()   const noexcept { return static_cast<char>(p[39]); }
};

struct BrokenTrade : Header {
    [[nodiscard]] std::uint64_t match_number() const noexcept { return be64(p + 11); }
};

// The four-decimal fixed point on the wire is exactly the representation the
// book uses, so this is an identity conversion kept for documentation value.
[[nodiscard]] constexpr std::int64_t to_price(std::uint32_t wire) noexcept {
    return static_cast<std::int64_t>(wire);
}

}  // namespace itch
