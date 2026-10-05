// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lob/order_index.hpp"

using namespace lob;

TEST(OrderIndex, InsertFindErase) {
    OrderIndex ix(1024);
    EXPECT_TRUE(ix.insert(42, 7));
    EXPECT_EQ(ix.find(42), 7u);
    EXPECT_EQ(ix.find(43), kNullHandle);
    EXPECT_TRUE(ix.erase(42));
    EXPECT_EQ(ix.find(42), kNullHandle);
    EXPECT_FALSE(ix.erase(42));
}

TEST(OrderIndex, RejectsDuplicateId) {
    OrderIndex ix(1024);
    EXPECT_TRUE(ix.insert(1, 1));
    EXPECT_FALSE(ix.insert(1, 2)) << "a live order reference must not be reusable";
    EXPECT_EQ(ix.find(1), 1u);
}

TEST(OrderIndex, UpdateRewritesHandle) {
    OrderIndex ix(1024);
    ix.insert(9, 100);
    EXPECT_TRUE(ix.update(9, 200));
    EXPECT_EQ(ix.find(9), 200u);
    EXPECT_FALSE(ix.update(10, 1));
}

TEST(OrderIndex, RefusesToExceedLoadFactor) {
    OrderIndex ix(16);
    std::size_t inserted = 0;
    for (OrderId id = 1; id <= 1000; ++id)
        if (ix.insert(id, static_cast<Handle>(id))) ++inserted;
    EXPECT_LT(inserted, ix.capacity());
    EXPECT_GT(inserted, 0u);
}

// Backward-shift deletion is the subtle part: a broken implementation leaves
// entries unreachable after churn rather than failing loudly, so this drives a
// long add/remove cycle against a std::unordered_map oracle.
TEST(OrderIndex, SurvivesHeavyChurnAgainstOracle) {
    OrderIndex ix(1 << 14);
    std::unordered_map<OrderId, Handle> oracle;
    std::mt19937_64 rng(999);
    std::vector<OrderId> live;

    for (int step = 0; step < 200000; ++step) {
        const bool add = live.empty() || (rng() % 100) < 55;
        if (add && oracle.size() < 8000) {
            const OrderId id = rng();
            const auto h = static_cast<Handle>(rng() % 1000000);
            if (oracle.count(id)) continue;
            ASSERT_TRUE(ix.insert(id, h));
            oracle[id] = h;
            live.push_back(id);
        } else if (!live.empty()) {
            const std::size_t k = rng() % live.size();
            const OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();
            ASSERT_TRUE(ix.erase(id)) << "erase lost an id after churn";
            oracle.erase(id);
        }
        ASSERT_EQ(ix.size(), oracle.size());
    }

    // Everything still present must still be findable: this is what tombstone
    // rot would break.
    for (const auto& [id, h] : oracle) EXPECT_EQ(ix.find(id), h) << "id " << id << " unreachable";
}
