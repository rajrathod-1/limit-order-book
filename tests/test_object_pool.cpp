// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "lob/object_pool.hpp"

using namespace lob;

namespace {
struct Node { Handle link; std::uint64_t payload; };
}

TEST(ObjectPool, HandsOutDistinctSlots) {
    ObjectPool<Node> pool(64);
    std::set<Handle> seen;
    for (int i = 0; i < 64; ++i) {
        const Handle h = pool.acquire();
        ASSERT_NE(h, kNullHandle) << "pool should have capacity at " << i;
        EXPECT_TRUE(seen.insert(h).second) << "handle " << h << " handed out twice";
    }
    EXPECT_EQ(pool.live(), 64u);
    EXPECT_EQ(pool.free_count(), 0u);
}

TEST(ObjectPool, ReportsExhaustionRatherThanAllocating) {
    ObjectPool<Node> pool(4);
    for (int i = 0; i < 4; ++i) EXPECT_NE(pool.acquire(), kNullHandle);
    // The whole point of the pool: it refuses, it does not grow.
    EXPECT_EQ(pool.acquire(), kNullHandle);
    EXPECT_EQ(pool.acquire(), kNullHandle);
}

TEST(ObjectPool, RecyclesReleasedSlots) {
    ObjectPool<Node> pool(4);
    std::vector<Handle> hs;
    for (int i = 0; i < 4; ++i) hs.push_back(pool.acquire());
    for (Handle h : hs) pool.release(h);
    EXPECT_EQ(pool.live(), 0u);
    for (int i = 0; i < 4; ++i) EXPECT_NE(pool.acquire(), kNullHandle);
    EXPECT_EQ(pool.acquire(), kNullHandle);
}

TEST(ObjectPool, StorageSurvivesChurn) {
    ObjectPool<Node> pool(8);
    const Handle a = pool.acquire();
    pool[a].payload = 0xDEADBEEFull;
    const Handle b = pool.acquire();
    pool[b].payload = 0x1234ull;
    EXPECT_EQ(pool[a].payload, 0xDEADBEEFull);
    pool.release(b);
    EXPECT_EQ(pool[a].payload, 0xDEADBEEFull) << "releasing one slot corrupted another";
}

TEST(ObjectPool, TracksHighWaterMark) {
    ObjectPool<Node> pool(16);
    std::vector<Handle> hs;
    for (int i = 0; i < 10; ++i) hs.push_back(pool.acquire());
    for (int i = 0; i < 7; ++i) { pool.release(hs.back()); hs.pop_back(); }
    EXPECT_EQ(pool.live(), 3u);
    EXPECT_EQ(pool.high_water(), 10u) << "high water must remember the peak, not the current level";
}

TEST(ObjectPool, PrefaultLeavesPoolUsable) {
    ObjectPool<Node> pool(32);
    pool.prefault();
    EXPECT_EQ(pool.live(), 0u);
    std::set<Handle> seen;
    for (int i = 0; i < 32; ++i) {
        const Handle h = pool.acquire();
        ASSERT_NE(h, kNullHandle);
        EXPECT_TRUE(seen.insert(h).second);
    }
}

TEST(ObjectPool, RejectsDoubleReserve) {
    ObjectPool<Node> pool(8);
    EXPECT_THROW(pool.reserve(16), std::logic_error);
}
