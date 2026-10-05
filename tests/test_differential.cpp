// SPDX-License-Identifier: MIT
//
// The differential test: the flat-array book and the std::map baseline are
// driven through identical random order flow and must agree on everything --
// every fill, in order, at the same price and size, and the same book after.
//
// This is the test that actually justifies the optimised structure. Unit tests
// check the cases somebody thought of; this one checks the cases nobody thought
// of, by generating hundreds of thousands of them and holding a deliberately
// simple, obviously correct implementation up against the fast one.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/engine.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

// Records fills so the two engines' execution streams can be compared exactly.
struct FillLog {
    struct Row {
        OrderId aggressor, resting;
        Price   price;
        Qty     qty;
        bool    consumed;
        friend bool operator==(const Row&, const Row&) = default;
    };
    std::vector<Row> rows;

    void on_fill(const Fill& f) { rows.push_back({f.aggressor_id, f.resting_id, f.price, f.qty, f.resting_filled}); }
    void on_accepted(OrderId, Side, Price, Qty) {}
    void on_cancelled(OrderId, Side, Price, Qty) {}
    void on_replaced(OrderId, OrderId, Side, Price, Qty) {}
    void on_rejected(OrderId, Status) {}
};

EngineConfig make_config() {
    EngineConfig c;
    c.scale         = TickScale{100, 900'000};  // penny grid based at $90
    c.tick_capacity = 4000;                     // $90.00 .. $130.00
    c.max_orders    = 1 << 16;
    return c;
}

using Fast = MatchingEngine<FillLog>;
using Slow = MapMatchingEngine<FillLog>;

// The full visible book, as an ordered price -> quantity map, from each engine.
std::map<Price, std::uint64_t> depth_of(const Fast& e, Side s) {
    std::map<Price, std::uint64_t> d;
    if (s == Side::Buy)
        e.bids().top_levels(1u << 30, [&](Price p, const PriceLevel& l) { d[p] = l.total_qty; });
    else
        e.asks().top_levels(1u << 30, [&](Price p, const PriceLevel& l) { d[p] = l.total_qty; });
    return d;
}

std::map<Price, std::uint64_t> depth_of(const Slow& e, Side s, const EngineConfig&) {
    // The baseline is keyed by price and has no band, so ask it directly rather
    // than scanning a tick range that would miss far-book levels entirely.
    return e.depth(s);
}

void expect_same_book(Fast& fast, Slow& slow, const EngineConfig& cfg, const std::string& where) {
    ASSERT_EQ(fast.live_orders(), slow.live_orders()) << where << ": live order count diverged";
    ASSERT_EQ(fast.best_bid(), slow.best_bid())       << where << ": best bid diverged";
    ASSERT_EQ(fast.best_ask(), slow.best_ask())       << where << ": best ask diverged";
    ASSERT_EQ(fast.bids().best_qty(), slow.best_bid_qty()) << where << ": bid size diverged";
    ASSERT_EQ(fast.asks().best_qty(), slow.best_ask_qty()) << where << ": ask size diverged";
    ASSERT_EQ(depth_of(fast, Side::Buy),  depth_of(slow, Side::Buy,  cfg)) << where << ": bid depth diverged";
    ASSERT_EQ(depth_of(fast, Side::Sell), depth_of(slow, Side::Sell, cfg)) << where << ": ask depth diverged";
}

void expect_same_fills(Fast& fast, Slow& slow, const std::string& where) {
    ASSERT_EQ(fast.sink().rows.size(), slow.sink().rows.size()) << where << ": fill count diverged";
    for (std::size_t i = 0; i < fast.sink().rows.size(); ++i)
        ASSERT_EQ(fast.sink().rows[i], slow.sink().rows[i]) << where << ": fill " << i << " diverged";
    fast.sink().rows.clear();
    slow.sink().rows.clear();
}

}  // namespace

TEST(Differential, MatchesTheBaselineUnderRandomOrderFlow) {
    const EngineConfig cfg = make_config();
    Fast fast(cfg);
    Slow slow(cfg);

    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> tick(0, static_cast<int>(cfg.tick_capacity) - 1);
    std::uniform_int_distribution<Qty> qty(1, 5000);
    std::uniform_int_distribution<int> pick(0, 999);

    // 3% of prices deliberately land outside the dense band or off the tick
    // grid, so the far-book path is covered by the same comparison.
    auto a_price = [&]() -> Price {
        const int r = pick(rng);
        if (r < 10)  return 1 + static_cast<Price>(rng() % 500);                  // sub-penny, far below
        if (r < 20)  return cfg.scale.to_price(static_cast<Ticks>(cfg.tick_capacity)) +
                            static_cast<Price>(rng() % 1000000);                  // far above the band
        if (r < 30)  return cfg.scale.to_price(static_cast<Ticks>(tick(rng))) +
                            static_cast<Price>(1 + rng() % 99);                   // on-band but off-grid
        return cfg.scale.to_price(static_cast<Ticks>(tick(rng)));
    };

    std::vector<OrderId> live;
    OrderId next_id = 1;

    constexpr int kSteps = 120000;
    for (int step = 0; step < kSteps; ++step) {
        const int roll = pick(rng);
        const std::string where = "step " + std::to_string(step);

        if (roll < 500 || live.empty()) {
            const Side  side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price px   = a_price();
            const Qty   q    = qty(rng);
            const Tif   tif  = (roll < 440) ? Tif::Day : ((roll < 470) ? Tif::Ioc : Tif::Fok);
            const OrderId id = next_id++;

            const auto a = fast.submit_limit(id, side, px, q, tif);
            const auto b = slow.submit_limit(id, side, px, q, tif);
            ASSERT_EQ(a.status, b.status)          << where << ": status diverged";
            ASSERT_EQ(a.filled_qty, b.filled_qty)  << where << ": filled quantity diverged";
            ASSERT_EQ(a.resting_qty, b.resting_qty)<< where << ": resting quantity diverged";
            ASSERT_EQ(a.notional, b.notional)      << where << ": traded notional diverged";
            if (a.resting_qty > 0) live.push_back(id);

        } else if (roll < 620) {
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Qty  q    = qty(rng);
            const OrderId id = next_id++;
            const auto a = fast.submit_market(id, side, q);
            const auto b = slow.submit_market(id, side, q);
            ASSERT_EQ(a.status, b.status)         << where << ": market status diverged";
            ASSERT_EQ(a.filled_qty, b.filled_qty) << where << ": market fill diverged";
            ASSERT_EQ(a.notional, b.notional)     << where << ": market notional diverged";

        } else if (roll < 800) {
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k];
            live[k] = live.back(); live.pop_back();
            ASSERT_EQ(fast.cancel(id), slow.cancel(id)) << where << ": cancel status diverged";

        } else if (roll < 900) {
            const OrderId id = live[rng() % live.size()];
            const Qty cut = qty(rng);
            ASSERT_EQ(fast.reduce(id, cut), slow.reduce(id, cut)) << where << ": reduce status diverged";

        } else {
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k];
            const OrderId nid = next_id++;
            const Price px = a_price();
            const Qty   q  = qty(rng);
            const auto a = fast.replace(id, nid, px, q);
            const auto b = slow.replace(id, nid, px, q);
            ASSERT_EQ(a.status, b.status)         << where << ": replace status diverged";
            ASSERT_EQ(a.filled_qty, b.filled_qty) << where << ": replace fill diverged";
            if (a.status == Status::Ok) {
                live[k] = live.back(); live.pop_back();
                if (a.resting_qty > 0) live.push_back(nid);
            }
        }

        expect_same_fills(fast, slow, where);

        // Comparing the entire book every step would dominate the runtime, so
        // it is checked often enough to localise a divergence quickly.
        if (step % 250 == 0) {
            expect_same_book(fast, slow, cfg, where);
            if (::testing::Test::HasFailure()) return;
        }

        // An invariant that must hold at every single step, cheap enough to.
        ASSERT_FALSE(fast.crossed()) << where << ": array book left crossed";
    }

    expect_same_book(fast, slow, cfg, "final");
}

TEST(Differential, MatchesTheBaselineUnderReconstructionFlow) {
    // The ITCH replay path uses different entry points than order entry does,
    // so it gets its own differential run.
    const EngineConfig cfg = make_config();
    Fast fast(cfg);
    Slow slow(cfg);

    std::mt19937_64 rng(0xBEEF);
    std::uniform_int_distribution<int> tick(0, static_cast<int>(cfg.tick_capacity) - 1);
    std::uniform_int_distribution<Qty> qty(100, 9900);
    std::uniform_int_distribution<int> pick(0, 999);

    auto a_price = [&]() -> Price {
        const int r = pick(rng);
        if (r < 10) return 1 + static_cast<Price>(rng() % 500);
        if (r < 20) return cfg.scale.to_price(static_cast<Ticks>(cfg.tick_capacity)) +
                           static_cast<Price>(rng() % 1000000);
        if (r < 30) return cfg.scale.to_price(static_cast<Ticks>(tick(rng))) +
                           static_cast<Price>(1 + rng() % 99);
        return cfg.scale.to_price(static_cast<Ticks>(tick(rng)));
    };

    struct LiveOrder { OrderId id; Qty qty; };
    std::vector<LiveOrder> live;
    OrderId next_id = 1;

    for (int step = 0; step < 120000; ++step) {
        const std::string where = "step " + std::to_string(step);
        const int roll = pick(rng);

        if (roll < 450 || live.empty()) {
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const Price px = a_price();
            const Qty q = qty(rng);
            const OrderId id = next_id++;
            ASSERT_EQ(fast.insert_passive(id, side, px, q), slow.insert_passive(id, side, px, q)) << where;
            live.push_back({id, q});

        } else if (roll < 880) {
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k].id;
            live[k] = live.back(); live.pop_back();
            ASSERT_EQ(fast.cancel(id), slow.cancel(id)) << where;

        } else if (roll < 950) {
            const std::size_t k = rng() % live.size();
            LiveOrder& lo = live[k];
            const Qty exec = 1 + static_cast<Qty>(rng() % lo.qty);
            ASSERT_EQ(fast.execute(lo.id, exec), slow.execute(lo.id, exec)) << where;
            if (exec == lo.qty) { live[k] = live.back(); live.pop_back(); }
            else                { lo.qty -= exec; }

        } else {
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k].id;
            const OrderId nid = next_id++;
            const Price px = a_price();
            const Qty q = qty(rng);
            ASSERT_EQ(fast.replace_passive(id, nid, px, q), slow.replace_passive(id, nid, px, q)) << where;
            live[k] = {nid, q};
        }

        expect_same_fills(fast, slow, where);
        if (step % 250 == 0) {
            expect_same_book(fast, slow, cfg, where);
            if (::testing::Test::HasFailure()) return;
        }
    }

    expect_same_book(fast, slow, cfg, "final");
}
