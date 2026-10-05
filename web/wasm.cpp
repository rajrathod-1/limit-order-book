// SPDX-License-Identifier: MIT
//
// The browser demo's bridge: the same headers the native build uses, compiled
// to WebAssembly. One live engine for the order flow the page drives, and the
// mixed-flow benchmark run against both books so a visitor can measure them.
//
// Prices cross the boundary as integer cents; the engine's fixed-point Price
// has four implied decimals, so a cent is 100 units.

#include <emscripten/emscripten.h>

#include <algorithm>
#include <random>
#include <vector>

#include "lob/engine.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

// Collects fills until the page reads them: (cents, qty, aggressor side) triples.
struct Tape {
    std::vector<double> fills;
    void on_fill(const Fill& f) {
        fills.insert(fills.end(), {double(f.price / 100), double(f.qty), double(f.aggressor_side)});
    }
    void on_accepted(OrderId, Side, Price, Qty) noexcept {}
    void on_cancelled(OrderId, Side, Price, Qty) noexcept {}
    void on_replaced(OrderId, OrderId, Side, Price, Qty) noexcept {}
    void on_rejected(OrderId, Status) noexcept {}
};

MatchingEngine<Tape>& live() {
    static MatchingEngine<Tape> e([] {
        EngineConfig c;
        c.tick_capacity = 1u << 15;  // $0.00 to $327.67 on a penny grid
        c.max_orders = 1u << 16;
        return c;
    }());
    return e;
}

std::vector<double> out;  // results the page reads through out_ptr()
OrderId next_id = 1;

// The mixed flow from bench/bench_book.cpp, weighted like the measured ITCH
// day: 45% adds, 45% cancels, 8% replaces, 2% executions, across `levels`
// price levels either side of a $200 mid. Returns nanoseconds per operation.
template <class Engine>
double mixed_flow_ns(int levels, int ops) {
    constexpr std::size_t kDepth = 20000;
    constexpr Ticks kMid = 20000;
    constexpr std::size_t kWorkload = 1u << 16;

    std::mt19937_64 rng(42);
    std::geometric_distribution<int> away(3.0 / std::max(1, levels));
    std::vector<Side> sides(kWorkload);
    std::vector<Price> prices(kWorkload);
    std::vector<Qty> qtys(kWorkload);
    for (std::size_t i = 0; i < kWorkload; ++i) {
        const bool buy = (rng() & 1) != 0;
        const int d = std::min(away(rng), levels - 1);
        sides[i] = buy ? Side::Buy : Side::Sell;
        prices[i] = kPennyTicks.to_price(buy ? kMid - 1 - d : kMid + 1 + d);
        qtys[i] = Qty(1 + rng() % 20) * 100;
    }

    EngineConfig c;
    c.tick_capacity = 1u << 16;
    c.max_orders = kDepth + std::size_t(ops) + 1024;
    Engine e(c);
    OrderId next = 1;
    std::vector<OrderId> resting;
    for (std::size_t i = 0; i < kDepth; ++i, ++next)
        if (e.insert_passive(next, sides[i], prices[i], qtys[i]) == Status::Ok) resting.push_back(next);

    const double start = emscripten_get_now();
    for (int i = 0; i < ops; ++i) {
        const auto j = std::size_t(i) & (kWorkload - 1);
        const int roll = int(rng() % 100);
        if (roll < 45 || resting.size() < 1000) {
            if (e.insert_passive(next, sides[j], prices[j], qtys[j]) == Status::Ok) resting.push_back(next);
            ++next;
        } else if (roll < 90) {
            const std::size_t k = rng() % resting.size();
            e.cancel(resting[k]);
            resting[k] = resting.back();
            resting.pop_back();
        } else if (roll < 98) {
            const std::size_t k = rng() % resting.size();
            if (e.replace_passive(resting[k], next, prices[j], qtys[j]) == Status::Ok) resting[k] = next;
            ++next;
        } else {
            const std::size_t k = rng() % resting.size();
            if (e.execute(resting[k], 100) != Status::Ok) {
                resting[k] = resting.back();
                resting.pop_back();
            }
        }
    }
    return (emscripten_get_now() - start) * 1e6 / ops;
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void reset() {
    live().clear();
    live().sink().fills.clear();
    next_id = 1;
}

// The order's id if any of it rests, 0 if it traded in full, or -status if rejected.
EMSCRIPTEN_KEEPALIVE double limit(int side, int cents, int qty) {
    const OrderId id = next_id++;
    const SubmitResult r = live().submit_limit(id, Side(side), Price(cents) * 100, Qty(qty));
    if (!r.ok()) return -double(r.status);
    return r.rested() ? double(id) : 0;
}

// Shares filled.
EMSCRIPTEN_KEEPALIVE int market(int side, int qty) {
    return int(live().submit_market(next_id++, Side(side), Qty(qty)).filled_qty);
}

EMSCRIPTEN_KEEPALIVE int cancel(double id) {
    return int(live().cancel(OrderId(id)));
}

// Up to `max` levels of one side, touch outward, as (cents, qty) pairs.
EMSCRIPTEN_KEEPALIVE int depth(int side, int max) {
    out.clear();
    const auto push = [](Price p, const PriceLevel& l) {
        out.push_back(double(p / 100));
        out.push_back(double(l.total_qty));
    };
    if (side == 0) live().bids().top_levels(std::size_t(max), push);
    else           live().asks().top_levels(std::size_t(max), push);
    return int(out.size() / 2);
}

// Fills since the last call, as (cents, qty, aggressor side) triples.
EMSCRIPTEN_KEEPALIVE int fills() {
    out.swap(live().sink().fills);
    live().sink().fills.clear();
    return int(out.size() / 3);
}

EMSCRIPTEN_KEEPALIVE int live_orders() { return int(live().live_orders()); }

EMSCRIPTEN_KEEPALIVE double* out_ptr() { return out.data(); }

// Nanoseconds per operation of the mixed flow, on the array book or the std::map baseline.
EMSCRIPTEN_KEEPALIVE double bench(int use_map, int levels, int ops) {
    return use_map ? mixed_flow_ns<MapMatchingEngine<NullSink>>(levels, ops)
                   : mixed_flow_ns<MatchingEngine<NullSink>>(levels, ops);
}

}  // extern "C"
