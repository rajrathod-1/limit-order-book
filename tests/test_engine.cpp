// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "lob/engine.hpp"

using namespace lob;

namespace {

struct Recorder {
    std::vector<Fill>  fills;
    std::vector<Status> rejects;
    void on_fill(const Fill& f) { fills.push_back(f); }
    void on_accepted(OrderId, Side, Price, Qty) {}
    void on_cancelled(OrderId, Side, Price, Qty) {}
    void on_replaced(OrderId, OrderId, Side, Price, Qty) {}
    void on_rejected(OrderId, Status s) { rejects.push_back(s); }
};

constexpr Price P(double dollars) { return static_cast<Price>(dollars * 10000.0 + 0.5); }

EngineConfig cfg() {
    EngineConfig c;
    c.scale         = kPennyTicks;
    c.tick_capacity = 1u << 16;
    c.max_orders    = 4096;
    return c;
}

using Engine = MatchingEngine<Recorder>;

std::vector<OrderId> filled_ids(const Engine& e) {
    std::vector<OrderId> v;
    for (const auto& f : const_cast<Engine&>(e).sink().fills) v.push_back(f.resting_id);
    return v;
}

}  // namespace

TEST(Engine, RestsNonMarketableLimit) {
    Engine e(cfg());
    const auto r = e.submit_limit(1, Side::Buy, P(10.00), 100);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.filled_qty, 0u);
    EXPECT_EQ(r.resting_qty, 100u);
    EXPECT_EQ(e.best_bid(), P(10.00));
    EXPECT_EQ(e.live_orders(), 1u);
}

TEST(Engine, CrossesAtTheRestingPrice) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 100);
    // Buyer is willing to pay 10.05 but the resting ask sets the price.
    const auto r = e.submit_limit(2, Side::Buy, P(10.05), 100);
    EXPECT_EQ(r.filled_qty, 100u);
    EXPECT_EQ(r.notional, static_cast<std::uint64_t>(P(10.00)) * 100)
        << "aggressive order must receive price improvement, not pay its limit";
    EXPECT_EQ(e.live_orders(), 0u);
}

TEST(Engine, PriceThenTimePriority) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.01), 10);   // worse price, arrives first
    e.submit_limit(2, Side::Sell, P(10.00), 10);   // better price, arrives second
    e.submit_limit(3, Side::Sell, P(10.00), 10);   // same price, arrives third
    e.submit_limit(9, Side::Buy,  P(10.05), 30);
    // Price first: both 10.00 orders before the 10.01. Time second: 2 before 3.
    EXPECT_EQ(filled_ids(e), (std::vector<OrderId>{2, 3, 1}));
}

TEST(Engine, PartialFillLeavesRemainderResting) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 30);
    const auto r = e.submit_limit(2, Side::Buy, P(10.00), 100);
    EXPECT_EQ(r.filled_qty, 30u);
    EXPECT_EQ(r.resting_qty, 70u);
    EXPECT_EQ(e.best_bid(), P(10.00));
    EXPECT_EQ(e.bids().best_qty(), 70u);
}

TEST(Engine, PartiallyConsumedRestingOrderKeepsItsPlace) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 100);
    e.submit_limit(2, Side::Sell, P(10.00), 100);
    e.submit_limit(3, Side::Buy,  P(10.00), 40);   // eats 40 of order 1
    e.sink().fills.clear();
    e.submit_limit(4, Side::Buy,  P(10.00), 60);   // must finish order 1 first
    ASSERT_EQ(e.sink().fills.size(), 1u);
    EXPECT_EQ(e.sink().fills[0].resting_id, 1u);
}

TEST(Engine, MarketOrderSweepsLevelsAndNeverRests) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 50);
    e.submit_limit(2, Side::Sell, P(10.01), 50);
    const auto r = e.submit_market(3, Side::Buy, 200);
    EXPECT_EQ(r.filled_qty, 100u);
    EXPECT_EQ(r.resting_qty, 0u) << "a market order must never rest";
    EXPECT_TRUE(e.asks().empty());
}

TEST(Engine, MarketOrderIntoEmptyBookIsRejected) {
    Engine e(cfg());
    const auto r = e.submit_market(1, Side::Buy, 100);
    EXPECT_EQ(r.status, Status::NoLiquidity);
}

TEST(Engine, IocCancelsItsRemainder) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 30);
    const auto r = e.submit_limit(2, Side::Buy, P(10.00), 100, Tif::Ioc);
    EXPECT_EQ(r.filled_qty, 30u);
    EXPECT_EQ(r.resting_qty, 0u);
    EXPECT_TRUE(e.bids().empty());
}

TEST(Engine, FokIsAllOrNothingAndLeavesTheBookUntouched) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 30);
    const auto r = e.submit_limit(2, Side::Buy, P(10.00), 100, Tif::Fok);
    EXPECT_EQ(r.status, Status::FillOrKillUnfilled);
    EXPECT_EQ(r.filled_qty, 0u);
    EXPECT_EQ(e.asks().best_qty(), 30u) << "a killed FOK must not have consumed anything";

    const auto ok = e.submit_limit(3, Side::Buy, P(10.00), 30, Tif::Fok);
    EXPECT_TRUE(ok.ok());
    EXPECT_EQ(ok.filled_qty, 30u);
}

TEST(Engine, CancelRemovesTheOrder) {
    Engine e(cfg());
    e.submit_limit(1, Side::Buy, P(10.00), 100);
    EXPECT_EQ(e.cancel(1), Status::Ok);
    EXPECT_TRUE(e.bids().empty());
    EXPECT_EQ(e.live_orders(), 0u);
    EXPECT_EQ(e.cancel(1), Status::UnknownOrderId) << "cancelling twice must fail";
}

TEST(Engine, ReduceKeepsPriorityButShrinksSize) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 100);
    e.submit_limit(2, Side::Sell, P(10.00), 100);
    EXPECT_EQ(e.reduce(1, 60), Status::Ok);
    EXPECT_EQ(e.asks().best_qty(), 140u);
    e.submit_limit(3, Side::Buy, P(10.00), 40);
    ASSERT_FALSE(e.sink().fills.empty());
    EXPECT_EQ(e.sink().fills[0].resting_id, 1u) << "a reduced order keeps its place in the queue";
}

TEST(Engine, ReduceRejectsMoreThanResting) {
    Engine e(cfg());
    e.submit_limit(1, Side::Buy, P(10.00), 100);
    EXPECT_EQ(e.reduce(1, 101), Status::InvalidQuantity);
    EXPECT_EQ(e.bids().best_qty(), 100u);
}

TEST(Engine, ReplaceLosesTimePriority) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 10);
    e.submit_limit(2, Side::Sell, P(10.00), 10);
    e.replace(1, 3, P(10.00), 10);            // same price, still goes to the back
    e.submit_limit(9, Side::Buy, P(10.00), 20);
    EXPECT_EQ(filled_ids(e), (std::vector<OrderId>{2, 3}));
}

TEST(Engine, ReplaceCanCrossImmediately) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.10), 100);
    e.submit_limit(2, Side::Buy,  P(10.00), 100);
    // Repricing the bid up to the offer must trade, not rest crossed.
    const auto r = e.replace(2, 3, P(10.10), 100);
    EXPECT_EQ(r.filled_qty, 100u);
    EXPECT_TRUE(e.asks().empty());
    EXPECT_TRUE(e.bids().empty());
}

TEST(Engine, RejectsDuplicateOrderId) {
    Engine e(cfg());
    EXPECT_TRUE(e.submit_limit(1, Side::Buy, P(10.00), 100).ok());
    EXPECT_EQ(e.submit_limit(1, Side::Buy, P(10.00), 100).status, Status::DuplicateOrderId);
}

TEST(Engine, RejectsZeroQuantity) {
    Engine e(cfg());
    EXPECT_EQ(e.submit_limit(1, Side::Buy, P(10.00), 0).status, Status::InvalidQuantity);
}

// A price off the tick grid is not an error. Nasdaq carries sub-penny prices,
// and such an order rests in the far book rather than being turned away.
TEST(Engine, OffGridPriceRestsInTheFarBook) {
    Engine e(cfg());
    const auto r = e.submit_limit(1, Side::Buy, 100001, 10);   // $10.0001
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.resting_qty, 10u);
    EXPECT_EQ(e.best_bid(), 100001);
    EXPECT_EQ(e.far_orders(), 1u);
}

// Likewise a price outside the dense band: real feeds carry resting bids at
// $0.0001 and offers at $199,999, and dropping them would misstate the book.
TEST(Engine, PriceOutsideTheBandRestsInTheFarBook) {
    Engine e(cfg());
    const Price above = static_cast<Price>(cfg().tick_capacity) * 100 + 100;
    EXPECT_TRUE(e.submit_limit(1, Side::Sell, above, 10).ok());
    EXPECT_EQ(e.best_ask(), above);
    EXPECT_EQ(e.far_orders(), 1u);

    // And it still trades normally when something reaches it.
    const auto hit = e.submit_limit(2, Side::Buy, above, 10);
    EXPECT_EQ(hit.filled_qty, 10u);
    EXPECT_TRUE(e.asks().empty());
    EXPECT_EQ(e.far_orders(), 0u);
}

// The one price that genuinely cannot be represented: an order's location field
// is 32 bits, which covers every price Nasdaq can put on the wire.
TEST(Engine, RejectsPricesBeyondTheWireRange) {
    Engine e(cfg());
    const Price absurd = static_cast<Price>(std::numeric_limits<std::int32_t>::max()) + 1;
    EXPECT_EQ(e.submit_limit(1, Side::Buy, absurd, 10).status, Status::PriceOutOfRange);
    EXPECT_EQ(e.submit_limit(2, Side::Buy, -1, 10).status, Status::PriceOutOfRange);
}

// Far-book orders must obey price-time priority alongside array orders.
TEST(Engine, FarAndNearOrdersShareOnePriorityOrder) {
    Engine e(cfg());
    const Price far_ask = static_cast<Price>(cfg().tick_capacity) * 100 + 500;
    e.submit_limit(1, Side::Sell, far_ask,    10);   // worst offer, far book
    e.submit_limit(2, Side::Sell, P(10.00),   10);   // best offer, array
    e.submit_limit(3, Side::Sell, 100001,     10);   // $10.0001, off grid, middle
    const auto r = e.submit_limit(9, Side::Buy, far_ask, 30);
    EXPECT_EQ(r.filled_qty, 30u);
    EXPECT_EQ(filled_ids(e), (std::vector<OrderId>{2, 3, 1}))
        << "price priority must hold across the array/far split";
}

TEST(Engine, BookNeverEndsUpCrossed) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 100);
    e.submit_limit(2, Side::Buy,  P(10.50), 40);
    EXPECT_FALSE(e.crossed());
    e.submit_limit(3, Side::Buy,  P(10.50), 500);
    EXPECT_FALSE(e.crossed()) << "resting remainder must sit below the offer, not through it";
}

TEST(Engine, PoolExhaustionIsReportedNotFatal) {
    EngineConfig c = cfg();
    c.max_orders = 8;
    Engine e(c);
    Status last = Status::Ok;
    for (OrderId i = 1; i <= 20; ++i) {
        const auto r = e.submit_limit(i, Side::Buy, P(10.00) - static_cast<Price>(i) * 100, 10);
        last = r.status;
    }
    EXPECT_EQ(last, Status::PoolExhausted);
    EXPECT_LE(e.live_orders(), 8u);
}

TEST(Engine, FullyFilledAggressorNeverTouchesThePool) {
    Engine e(cfg());
    e.submit_limit(1, Side::Sell, P(10.00), 100);
    const auto before = e.pool().high_water();
    e.submit_limit(2, Side::Buy, P(10.00), 100);
    EXPECT_LE(e.pool().high_water(), before)
        << "an order that fills completely should never need a pool slot";
    EXPECT_EQ(e.pool().live(), 0u);
}

TEST(Engine, ReconstructionPrimitivesDoNotMatch) {
    Engine e(cfg());
    // A crossed book is legal during reconstruction: the feed is authoritative.
    e.insert_passive(1, Side::Sell, P(10.00), 100);
    e.insert_passive(2, Side::Buy,  P(10.50), 100);
    EXPECT_TRUE(e.sink().fills.empty()) << "passive insert must not match";
    EXPECT_TRUE(e.crossed());
    EXPECT_EQ(e.execute(1, 40), Status::Ok);
    EXPECT_EQ(e.asks().best_qty(), 60u);
    EXPECT_EQ(e.execute(1, 60), Status::Ok);
    EXPECT_TRUE(e.asks().empty());
}

TEST(Engine, ClearResetsEverything) {
    Engine e(cfg());
    e.submit_limit(1, Side::Buy,  P(10.00), 100);
    e.submit_limit(2, Side::Sell, P(10.50), 100);
    // Include a far-book order: clear() walks the occupancy bitmap for the
    // array, and must not forget the map hanging off the side of it.
    e.submit_limit(3, Side::Buy, 1, 50);
    ASSERT_EQ(e.far_orders(), 1u);
    e.clear();
    EXPECT_EQ(e.far_orders(), 0u);
    EXPECT_EQ(e.live_orders(), 0u);
    EXPECT_TRUE(e.bids().empty());
    EXPECT_TRUE(e.asks().empty());
    EXPECT_EQ(e.pool().live(), 0u);
    EXPECT_TRUE(e.submit_limit(1, Side::Buy, P(10.00), 100).ok()) << "ids must be reusable after clear";
}
