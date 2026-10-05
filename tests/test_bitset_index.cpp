// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <random>
#include <set>

#include "lob/bitset_index.hpp"

using namespace lob;

TEST(BitsetIndex, SetTestClear) {
    BitsetIndex b(4096);
    EXPECT_FALSE(b.any());
    b.set(0); b.set(63); b.set(64); b.set(4095);
    EXPECT_TRUE(b.test(0));
    EXPECT_TRUE(b.test(63));
    EXPECT_TRUE(b.test(64));
    EXPECT_TRUE(b.test(4095));
    EXPECT_FALSE(b.test(1));
    b.clear_bit(63);
    EXPECT_FALSE(b.test(63));
    EXPECT_TRUE(b.test(64));
}

TEST(BitsetIndex, FindsAcrossWordAndSummaryBoundaries) {
    BitsetIndex b(1 << 20);
    // Deliberately spread across L0, L1 and L2 boundaries.
    const std::size_t marks[] = {0, 1, 63, 64, 65, 4095, 4096, 4097, 262143, 262144, 1048575};
    for (auto m : marks) b.set(m);

    EXPECT_EQ(b.find_first_at_or_after(0), 0u);
    EXPECT_EQ(b.find_first_at_or_after(1), 1u);
    EXPECT_EQ(b.find_first_at_or_after(2), 63u);
    EXPECT_EQ(b.find_first_at_or_after(66), 4095u);
    EXPECT_EQ(b.find_first_at_or_after(4098), 262143u);
    EXPECT_EQ(b.find_first_at_or_after(262145), 1048575u);
    EXPECT_EQ(b.find_first_at_or_after(1048576 - 1), 1048575u);

    EXPECT_EQ(b.find_last_at_or_before(1048575), 1048575u);
    EXPECT_EQ(b.find_last_at_or_before(1048574), 262144u);
    EXPECT_EQ(b.find_last_at_or_before(4096), 4096u);
    EXPECT_EQ(b.find_last_at_or_before(4094), 65u);
    EXPECT_EQ(b.find_last_at_or_before(0), 0u);
}

TEST(BitsetIndex, EmptyReturnsNpos) {
    BitsetIndex b(8192);
    EXPECT_EQ(b.find_first_at_or_after(0), BitsetIndex::npos);
    EXPECT_EQ(b.find_last_at_or_before(8191), BitsetIndex::npos);
    b.set(100);
    b.clear_bit(100);
    EXPECT_EQ(b.find_first_at_or_after(0), BitsetIndex::npos);
    EXPECT_EQ(b.find_last_at_or_before(8191), BitsetIndex::npos);
    EXPECT_FALSE(b.any());
}

// The hierarchical search is the part most likely to hide an off-by-one, so it
// is checked against a std::set oracle over random populations.
TEST(BitsetIndex, MatchesSetOracleUnderRandomChurn) {
    constexpr std::size_t kCap = 70000;
    BitsetIndex b(kCap);
    std::set<std::size_t> oracle;
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::size_t> pick(0, kCap - 1);

    for (int round = 0; round < 20000; ++round) {
        const std::size_t i = pick(rng);
        if (rng() & 1) { b.set(i); oracle.insert(i); }
        else           { b.clear_bit(i); oracle.erase(i); }

        if (round % 97 != 0) continue;  // full comparison is expensive

        const std::size_t probe = pick(rng);

        auto up = oracle.lower_bound(probe);
        const std::size_t want_up = (up == oracle.end()) ? BitsetIndex::npos : *up;
        EXPECT_EQ(b.find_first_at_or_after(probe), want_up) << "probe=" << probe;

        auto down = oracle.upper_bound(probe);
        const std::size_t want_down = (down == oracle.begin()) ? BitsetIndex::npos : *std::prev(down);
        EXPECT_EQ(b.find_last_at_or_before(probe), want_down) << "probe=" << probe;
    }
}
