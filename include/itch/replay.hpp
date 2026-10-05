// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <algorithm>
#include <span>
#include <vector>
#include <string>

#include "bench/latency.hpp"
#include "itch/itch50.hpp"
#include "itch/reader.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"

namespace itch {

// ---------------------------------------------------------------------------
// Drives an order book from a replayed ITCH 5.0 stream.
//
// The mapping from wire message to book operation:
//
//   A / F  Add Order            -> passive insert       (the insert benchmark)
//   D      Order Delete         -> cancel               (the cancel benchmark)
//   X      Order Cancel         -> partial reduce       (keeps queue position)
//   E / C  Order Executed       -> execute at the front (the match benchmark)
//   U      Order Replace        -> cancel + re-insert   (loses time priority)
//
// Adds are inserted passively rather than submitted to the matcher, because
// Nasdaq has already matched this flow: anything that could have crossed did so
// before it reached the wire. Re-matching it would produce a book that is not
// the book the exchange had, and the whole point of replaying real data is that
// the book shapes are real.
//
// The E/C path is what "match latency" means here, and it is worth being exact
// about: it is the per-fill work an aggressive order does against each resting
// order it consumes -- locate the order, decrement the level aggregate, unlink
// it if exhausted, and repair the top of book if that emptied the level. A
// multi-level sweep is this operation in a loop, and lob_replay measures that
// separately by submitting genuine aggressive orders into the rebuilt book.
// ---------------------------------------------------------------------------

struct ReplayStats {
    std::uint64_t adds        = 0;
    std::uint64_t deletes     = 0;
    std::uint64_t reduces     = 0;
    std::uint64_t executes    = 0;
    std::uint64_t replaces    = 0;
    std::uint64_t trades      = 0;  // 'P', against hidden liquidity: no book change
    std::uint64_t unknown_ref = 0;  // named an order we never saw (pre-open state)
    std::uint64_t rejected    = 0;
    std::uint64_t max_live    = 0;
    std::uint64_t first_ts    = 0;
    std::uint64_t last_ts     = 0;
};

struct ReplayTimers {
    bench::LatencyRecorder insert{"insert (ITCH add)"};
    bench::LatencyRecorder cancel{"cancel (ITCH delete)"};
    bench::LatencyRecorder reduce{"reduce (ITCH cancel)"};
    bench::LatencyRecorder execute{"match  (ITCH execute)"};
    bench::LatencyRecorder replace{"replace (ITCH replace)"};

    void reserve(std::size_t n) {
        insert.reserve(n); cancel.reserve(n); reduce.reserve(n / 8 + 1);
        execute.reserve(n / 8 + 1); replace.reserve(n / 4 + 1);
    }
};

// `Measure` is a template parameter rather than a runtime flag so that the
// unmeasured run compiles with no timing code at all, and the throughput number
// is not quietly inflated by a branch that is present in both configurations.
template <class Engine, bool Measure = false>
class Rebuilder {
public:
    Rebuilder(Engine& engine, ReplayTimers* timers = nullptr)
        : engine_(engine), timers_(timers) {}

    void apply(Header h) {
        if (stats_.first_ts == 0) stats_.first_ts = h.timestamp();
        stats_.last_ts = h.timestamp();

        switch (h.type()) {
            case 'A': case 'F': {
                AddOrder a{h.p};
                const auto side = (a.side() == 'B') ? lob::Side::Buy : lob::Side::Sell;
                const auto px   = static_cast<lob::Price>(a.price());
                const auto qty  = static_cast<lob::Qty>(a.shares());
                const auto id   = a.order_ref();
                lob::Status st;
                if constexpr (Measure) {
                    const auto t0 = bench::Clock::now();
                    st = engine_.insert_passive(id, side, px, qty);
                    timers_->insert.add_ticks(bench::Clock::now() - t0);
                } else {
                    st = engine_.insert_passive(id, side, px, qty);
                }
                if (st != lob::Status::Ok) ++stats_.rejected; else ++stats_.adds;
                break;
            }
            case 'D': {
                OrderDelete d{h.p};
                lob::Status st;
                if constexpr (Measure) {
                    const auto t0 = bench::Clock::now();
                    st = engine_.cancel(d.order_ref());
                    timers_->cancel.add_ticks(bench::Clock::now() - t0);
                } else {
                    st = engine_.cancel(d.order_ref());
                }
                tally(st, stats_.deletes);
                break;
            }
            case 'X': {
                OrderCancel c{h.p};
                lob::Status st;
                if constexpr (Measure) {
                    const auto t0 = bench::Clock::now();
                    st = engine_.reduce(c.order_ref(), c.cancelled_shares());
                    timers_->reduce.add_ticks(bench::Clock::now() - t0);
                } else {
                    st = engine_.reduce(c.order_ref(), c.cancelled_shares());
                }
                tally(st, stats_.reduces);
                break;
            }
            case 'E': case 'C': {
                OrderExecuted e{h.p};
                lob::Status st;
                if constexpr (Measure) {
                    const auto t0 = bench::Clock::now();
                    st = engine_.execute(e.order_ref(), e.executed_shares());
                    timers_->execute.add_ticks(bench::Clock::now() - t0);
                } else {
                    st = engine_.execute(e.order_ref(), e.executed_shares());
                }
                tally(st, stats_.executes);
                break;
            }
            case 'U': {
                OrderReplace r{h.p};
                lob::Status st;
                if constexpr (Measure) {
                    const auto t0 = bench::Clock::now();
                    st = engine_.replace_passive(r.original_ref(), r.new_ref(),
                                                 static_cast<lob::Price>(r.price()),
                                                 static_cast<lob::Qty>(r.shares()));
                    timers_->replace.add_ticks(bench::Clock::now() - t0);
                } else {
                    st = engine_.replace_passive(r.original_ref(), r.new_ref(),
                                                 static_cast<lob::Price>(r.price()),
                                                 static_cast<lob::Qty>(r.shares()));
                }
                tally(st, stats_.replaces);
                break;
            }
            case 'P':
                // A trade against non-displayed liquidity. It prints, but it
                // never touched the visible book, so the book must not move.
                ++stats_.trades;
                break;
            default:
                break;
        }

        const auto live = static_cast<std::uint64_t>(engine_.live_orders());
        if (live > stats_.max_live) stats_.max_live = live;
    }

    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }

private:
    void tally(lob::Status st, std::uint64_t& counter) {
        if (st == lob::Status::Ok) ++counter;
        else if (st == lob::Status::UnknownOrderId) ++stats_.unknown_ref;
        else ++stats_.rejected;
    }

    Engine&       engine_;
    ReplayTimers* timers_;
    ReplayStats   stats_;
};

// ---------------------------------------------------------------------------
// A pre-pass that measures the price range actually used by the file, so the
// flat tick array can be sized to fit it exactly. This is what keeps the array
// book honest: the price band is derived from the data rather than guessed
// generously, and after this scan no order can fall outside it.
// ---------------------------------------------------------------------------
struct PriceRange {
    std::uint32_t min_price = 0;
    std::uint32_t max_price = 0;
    std::uint64_t priced    = 0;

    // The dense core of the distribution. Real feeds carry a thin tail of
    // resting orders at $0.0001 and $199,999 -- genuine orders, but sizing a
    // flat array to reach them would need two billion slots. The band is taken
    // from these quantiles instead, and the tail rests in the book's far map.
    std::uint32_t core_low  = 0;   // 0.1st percentile
    std::uint32_t core_high = 0;   // 99.9th percentile

    // GCD of the prices inside the core, i.e. the grid those prices sit on.
    // Measured rather than assumed: US equities display in pennies above $1.00
    // but in $0.0001 below it. Computing it over the whole distribution would
    // be dominated by a single sub-penny outlier and collapse to $0.0001.
    std::uint32_t grid = 0;

    std::uint64_t outliers = 0;    // priced messages outside [core_low, core_high]
    std::string   symbol;

    [[nodiscard]] bool valid() const noexcept { return priced > 0 && core_low <= core_high; }
    [[nodiscard]] std::uint32_t tick_size() const noexcept { return grid ? grid : 1u; }

    [[nodiscard]] double outlier_fraction() const noexcept {
        return priced ? static_cast<double>(outliers) / static_cast<double>(priced) : 0.0;
    }

    // The band to configure the book with: the core, widened by its own span on
    // each side so ordinary intraday movement never reaches the edge, then
    // snapped to the grid.
    [[nodiscard]] std::uint32_t band_floor() const noexcept {
        const std::uint32_t span = core_high - core_low;
        const std::uint32_t lo   = (core_low > span) ? core_low - span : 0;
        return lo - (lo % tick_size());
    }

    [[nodiscard]] std::uint64_t band_ticks() const noexcept {
        const std::uint32_t span = core_high - core_low;
        const std::uint64_t hi   = static_cast<std::uint64_t>(core_high) + span;
        return (hi - band_floor()) / tick_size() + 1;
    }
};

constexpr std::uint32_t gcd32(std::uint32_t a, std::uint32_t b) noexcept {
    while (b) { const std::uint32_t t = a % b; a = b; b = t; }
    return a;
}

inline PriceRange scan_price_range(std::span<const std::byte> data) {
    PriceRange pr;
    ParseStats st;

    // Every price is collected so the quantiles are exact. A busy single-name
    // day is a few million prices, which is a few tens of megabytes held for
    // the length of one pre-pass -- cheap, and it buys a band that is derived
    // from the data instead of guessed.
    std::vector<std::uint32_t> prices;
    prices.reserve(1u << 20);

    for_each_message(data, st, [&](Header h, std::uint16_t) {
        std::uint32_t px = 0;
        switch (h.type()) {
            case 'A': case 'F': px = AddOrder{h.p}.price();           break;
            case 'U':           px = OrderReplace{h.p}.price();       break;
            case 'C':           px = OrderExecuted{h.p}.exec_price(); break;
            case 'R':
                if (pr.symbol.empty()) pr.symbol = std::string(StockDirectory{h.p}.stock());
                return;
            default: return;
        }
        // Price 0 means "no price" on some messages, and 0x7FFFFFFF is the
        // sentinel Nasdaq uses for a market peg. Neither belongs in the range.
        if (px == 0 || px == 0x7FFFFFFFu) return;
        prices.push_back(px);
    });

    if (prices.empty()) return pr;

    std::sort(prices.begin(), prices.end());
    pr.priced    = prices.size();
    pr.min_price = prices.front();
    pr.max_price = prices.back();

    const auto at = [&](double q) {
        return prices[static_cast<std::size_t>(q * static_cast<double>(prices.size() - 1))];
    };
    pr.core_low  = at(0.001);
    pr.core_high = at(0.999);

    std::uint32_t g = 0;
    for (std::uint32_t v : prices) {
        if (v < pr.core_low || v > pr.core_high) { ++pr.outliers; continue; }
        g = gcd32(g, v);
    }
    pr.grid = g;
    return pr;
}

}  // namespace itch
