// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <vector>

#include "lob/array_book.hpp"

using namespace lob;

namespace {

// The band under test: a penny grid based at $0, 1<<14 ticks wide, so prices
// from $0.00 to $163.83 are indexable and anything else lands in the far map.
constexpr std::size_t kCapacity = 1 << 14;
constexpr Price       kBandTop  = static_cast<Price>(kCapacity) * 100;  // $163.84

template <Side S>
class Fixture {
public:
    Fixture() : pool_(4096), side_(pool_, kPennyTicks, kCapacity) {}

    Handle add(OrderId id, Price price, Qty q) {
        const Handle h = pool_.acquire();
        Order& o = pool_[h];
        o.next = o.prev = kNullHandle;
        o.qty = q; o.id = id; o.seq = seq_++;
        o.side = S; o.tif = Tif::Day; o._pad = 0;
        EXPECT_TRUE(side_.prepare(o, price));
        side_.insert(h);
        return h;
    }

    BookSide<S>& book() { return side_; }

    std::vector<OrderId> queue_at(Price p) {
        std::vector<OrderId> ids;
        const PriceLevel* l = side_.level_at(p);
        if (l) l->queue.for_each(pool_, [&](Handle, const Order& o) { ids.push_back(o.id); });
        return ids;
    }

    std::vector<Price> walk() {
        std::vector<Price> out;
        side_.top_levels(1000, [&](Price p, const PriceLevel&) { out.push_back(p); });
        return out;
    }

private:
    OrderPool   pool_;
    BookSide<S> side_;
    Seq         seq_ = 0;
};

constexpr Price P(double d) { return static_cast<Price>(d * 10000.0 + 0.5); }

}  // namespace

TEST(BookSide, EmptyBookHasNoBest) {
    Fixture<Side::Buy> f;
    EXPECT_TRUE(f.book().empty());
    EXPECT_EQ(f.book().best_price(), BookSide<Side::Buy>::kNoPrice);
    EXPECT_EQ(f.book().best_qty(), 0u);
    EXPECT_EQ(f.book().best_front(), kNullHandle);
}

TEST(BookSide, BidBestIsHighestPrice) {
    Fixture<Side::Buy> f;
    f.add(1, P(100.00), 10);
    EXPECT_EQ(f.book().best_price(), P(100.00));
    f.add(2, P(100.05), 10);
    EXPECT_EQ(f.book().best_price(), P(100.05)) << "a higher bid must take the touch";
    f.add(3, P(99.95), 10);
    EXPECT_EQ(f.book().best_price(), P(100.05)) << "a worse bid must not take the touch";
}

TEST(BookSide, AskBestIsLowestPrice) {
    Fixture<Side::Sell> f;
    f.add(1, P(100.00), 10);
    EXPECT_EQ(f.book().best_price(), P(100.00));
    f.add(2, P(99.95), 10);
    EXPECT_EQ(f.book().best_price(), P(99.95));
    f.add(3, P(100.05), 10);
    EXPECT_EQ(f.book().best_price(), P(99.95));
}

TEST(BookSide, AggregatesQuantityPerLevel) {
    Fixture<Side::Buy> f;
    f.add(1, P(100.00), 10);
    f.add(2, P(100.00), 25);
    f.add(3, P(100.01), 7);
    EXPECT_EQ(f.book().qty_at(P(100.00)), 35u);
    EXPECT_EQ(f.book().qty_at(P(100.01)), 7u);
    EXPECT_EQ(f.book().total_qty(), 42u);
    EXPECT_EQ(f.book().total_orders(), 3u);
}

TEST(BookSide, PreservesArrivalOrderWithinALevel) {
    Fixture<Side::Buy> f;
    f.add(1, P(100.00), 10);
    f.add(2, P(100.00), 10);
    f.add(3, P(100.00), 10);
    EXPECT_EQ(f.queue_at(P(100.00)), (std::vector<OrderId>{1, 2, 3}));
}

TEST(BookSide, RemovingFromTheMiddleKeepsTheRestInOrder) {
    Fixture<Side::Buy> f;
    f.add(1, P(100.00), 10);
    const Handle mid = f.add(2, P(100.00), 10);
    f.add(3, P(100.00), 10);
    f.book().remove(mid);
    EXPECT_EQ(f.queue_at(P(100.00)), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(f.book().qty_at(P(100.00)), 20u);
}

TEST(BookSide, BestRepairsWhenTopLevelEmpties) {
    Fixture<Side::Buy> f;
    const Handle top = f.add(1, P(120.00), 10);
    f.add(2, P(110.00), 10);
    f.add(3, P(100.00), 10);
    EXPECT_EQ(f.book().best_price(), P(120.00));
    f.book().remove(top);
    EXPECT_EQ(f.book().best_price(), P(110.00)) << "top of book must fall to the next populated level";
}

// The repair path must work when the next level is far away, which exercises
// the hierarchical bitmap rather than a same-word scan.
TEST(BookSide, BestRepairsAcrossAWideGap) {
    Fixture<Side::Buy> f;
    const Handle top = f.add(1, P(160.00), 10);
    f.add(2, P(0.12), 10);
    EXPECT_EQ(f.book().best_price(), P(160.00));
    f.book().remove(top);
    EXPECT_EQ(f.book().best_price(), P(0.12));
}

TEST(BookSide, ReduceKeepsQueuePosition) {
    Fixture<Side::Buy> f;
    const Handle first = f.add(1, P(100.00), 100);
    f.add(2, P(100.00), 50);
    f.book().reduce(first, 40);
    EXPECT_EQ(f.book().qty_at(P(100.00)), 110u);
    EXPECT_EQ(f.queue_at(P(100.00)), (std::vector<OrderId>{1, 2}))
        << "reducing size must not move an order to the back";
}

TEST(BookSide, WalksLevelsOutwardFromTheTouch) {
    Fixture<Side::Sell> f;
    f.add(1, P(100.00), 10);
    f.add(2, P(100.02), 20);
    f.add(3, P(100.07), 30);
    EXPECT_EQ(f.walk(), (std::vector<Price>{P(100.00), P(100.02), P(100.07)}));
}

// ---------------------------------------------------------------------------
// The far book: prices outside the dense band, or off the tick grid entirely.
// These are real orders on real feeds and must behave identically.
// ---------------------------------------------------------------------------

TEST(BookSide, PricesOutsideTheBandStillRest) {
    Fixture<Side::Sell> f;
    f.add(1, P(100.00), 10);
    f.add(2, kBandTop + P(1000.00), 5);     // far above the band
    EXPECT_EQ(f.book().total_orders(), 2u);
    EXPECT_EQ(f.book().far_orders(), 1u);
    EXPECT_EQ(f.book().best_price(), P(100.00)) << "the far offer is worse, so it is not the touch";
    EXPECT_EQ(f.book().qty_at(kBandTop + P(1000.00)), 5u);
}

TEST(BookSide, OffGridPricesGoToTheFarBook) {
    Fixture<Side::Buy> f;
    f.add(1, 1, 10);   // $0.0001 -- not a penny multiple
    EXPECT_EQ(f.book().far_orders(), 1u);
    EXPECT_EQ(f.book().best_price(), 1);
    EXPECT_EQ(f.book().qty_at(1), 10u);
}

TEST(BookSide, AFarPriceCanBeTheTouch) {
    Fixture<Side::Buy> f;
    f.add(1, P(100.00), 10);
    f.add(2, kBandTop + P(500.00), 7);      // a bid above the whole band
    EXPECT_EQ(f.book().best_price(), kBandTop + P(500.00))
        << "the most aggressive price wins regardless of which structure holds it";
    EXPECT_EQ(f.book().best_qty(), 7u);
}

TEST(BookSide, DepthWalkMergesArrayAndFarBook) {
    Fixture<Side::Sell> f;
    f.add(1, P(100.00), 10);
    f.add(2, P(101.00), 10);
    f.add(3, kBandTop + P(10.00), 10);      // far, worst
    f.add(4, 1, 10);                        // $0.0001, off grid, best
    EXPECT_EQ(f.walk(), (std::vector<Price>{1, P(100.00), P(101.00), kBandTop + P(10.00)}))
        << "one correctly ordered book, whichever structure each level lives in";
}

TEST(BookSide, RemovingFarOrdersRepairsTheTouch) {
    Fixture<Side::Buy> f;
    const Handle far = f.add(1, kBandTop + P(500.00), 7);
    f.add(2, P(100.00), 10);
    EXPECT_EQ(f.book().best_price(), kBandTop + P(500.00));
    f.book().remove(far);
    EXPECT_EQ(f.book().best_price(), P(100.00));
    EXPECT_EQ(f.book().far_orders(), 0u);
    EXPECT_EQ(f.book().far_levels(), 0u);
}

TEST(BookSide, FarOrdersKeepTimePriorityToo) {
    Fixture<Side::Buy> f;
    f.add(1, 1, 10);
    f.add(2, 1, 10);
    f.add(3, 1, 10);
    EXPECT_EQ(f.queue_at(1), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(f.book().qty_at(1), 30u);
}

TEST(BookSide, EmptiesCompletely) {
    Fixture<Side::Buy> f;
    const Handle a = f.add(1, P(100.00), 10);
    const Handle b = f.add(2, P(100.01), 10);
    const Handle c = f.add(3, 1, 10);        // far
    f.book().remove(a);
    f.book().remove(b);
    f.book().remove(c);
    EXPECT_TRUE(f.book().empty());
    EXPECT_EQ(f.book().best_price(), BookSide<Side::Buy>::kNoPrice);
    EXPECT_EQ(f.book().total_qty(), 0u);
    EXPECT_EQ(f.book().far_orders(), 0u);
}
