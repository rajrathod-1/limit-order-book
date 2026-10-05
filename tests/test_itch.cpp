// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "itch/reader.hpp"
#include "itch/replay.hpp"

using namespace itch;

namespace {

// Builds messages the way the wire does, so the decoder is tested against
// independently constructed bytes rather than against its own encoder.
struct Builder {
    std::vector<std::byte> b;

    void u8(unsigned v)  { b.push_back(static_cast<std::byte>(v)); }
    void ch(char c)      { b.push_back(static_cast<std::byte>(c)); }
    void u16(std::uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
    void u32(std::uint32_t v) { for (int i = 3; i >= 0; --i) u8((v >> (i * 8)) & 0xFF); }
    void u48(std::uint64_t v) { for (int i = 5; i >= 0; --i) u8((v >> (i * 8)) & 0xFF); }
    void u64(std::uint64_t v) { for (int i = 7; i >= 0; --i) u8((v >> (i * 8)) & 0xFF); }
    void text(const char* s, std::size_t w) {
        const std::size_t n = std::strlen(s);
        for (std::size_t i = 0; i < w; ++i) ch(i < n ? s[i] : ' ');
    }
    void header(char t, std::uint16_t locate, std::uint64_t ts) { ch(t); u16(locate); u16(0); u48(ts); }
    [[nodiscard]] const std::byte* data() const { return b.data(); }
};

// Wraps messages in BinaryFILE framing.
std::vector<std::byte> frame(const std::vector<std::vector<std::byte>>& msgs) {
    std::vector<std::byte> out;
    for (const auto& m : msgs) {
        out.push_back(static_cast<std::byte>((m.size() >> 8) & 0xFF));
        out.push_back(static_cast<std::byte>(m.size() & 0xFF));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

}  // namespace

TEST(Itch, BigEndianReaders) {
    const std::byte bytes[] = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78},
        std::byte{0x9A}, std::byte{0xBC}, std::byte{0xDE}, std::byte{0xF0}};
    EXPECT_EQ(be16(bytes), 0x1234u);
    EXPECT_EQ(be32(bytes), 0x12345678u);
    EXPECT_EQ(be48(bytes), 0x123456789ABCull);
    EXPECT_EQ(be64(bytes), 0x123456789ABCDEF0ull);
}

TEST(Itch, TrimsPaddedAlphaFields) {
    const char raw[] = "AAPL    ";
    const auto* p = reinterpret_cast<const std::byte*>(raw);
    EXPECT_EQ(alpha(p, 8), "AAPL");
    const char full[] = "ABCDEFGH";
    EXPECT_EQ(alpha(reinterpret_cast<const std::byte*>(full), 8), "ABCDEFGH");
}

TEST(Itch, DecodesAddOrder) {
    Builder m;
    m.header('A', 7, 34'200'000'000'000ull);
    m.u64(123456789);
    m.ch('B');
    m.u32(500);
    m.text("MSFT", 8);
    m.u32(1'578'500);           // $157.85
    ASSERT_EQ(m.b.size(), declared_length('A'));

    AddOrder a{m.data()};
    EXPECT_EQ(a.type(), 'A');
    EXPECT_EQ(a.stock_locate(), 7u);
    EXPECT_EQ(a.timestamp(), 34'200'000'000'000ull);
    EXPECT_EQ(a.order_ref(), 123456789u);
    EXPECT_EQ(a.side(), 'B');
    EXPECT_EQ(a.shares(), 500u);
    EXPECT_EQ(a.stock(), "MSFT");
    EXPECT_EQ(a.price(), 1'578'500u);
}

TEST(Itch, DecodesAddOrderWithMpid) {
    Builder m;
    m.header('F', 1, 1);
    m.u64(42); m.ch('S'); m.u32(100); m.text("AAPL", 8); m.u32(2'000'000);
    m.text("NSDQ", 4);
    ASSERT_EQ(m.b.size(), declared_length('F'));
    AddOrder a{m.data()};
    EXPECT_EQ(a.mpid(), "NSDQ");
    EXPECT_EQ(a.stock(), "AAPL");
}

TEST(Itch, DecodesExecutionsCancelsDeletesReplaces) {
    {
        Builder m; m.header('E', 1, 5); m.u64(99); m.u32(250); m.u64(7777);
        ASSERT_EQ(m.b.size(), declared_length('E'));
        OrderExecuted e{m.data()};
        EXPECT_EQ(e.order_ref(), 99u);
        EXPECT_EQ(e.executed_shares(), 250u);
        EXPECT_EQ(e.match_number(), 7777u);
        EXPECT_FALSE(e.has_price());
    }
    {
        Builder m; m.header('C', 1, 5); m.u64(99); m.u32(250); m.u64(7777); m.ch('Y'); m.u32(123400);
        ASSERT_EQ(m.b.size(), declared_length('C'));
        OrderExecuted e{m.data()};
        EXPECT_TRUE(e.has_price());
        EXPECT_EQ(e.printable(), 'Y');
        EXPECT_EQ(e.exec_price(), 123400u);
    }
    {
        Builder m; m.header('X', 1, 5); m.u64(55); m.u32(300);
        ASSERT_EQ(m.b.size(), declared_length('X'));
        OrderCancel c{m.data()};
        EXPECT_EQ(c.order_ref(), 55u);
        EXPECT_EQ(c.cancelled_shares(), 300u);
    }
    {
        Builder m; m.header('D', 1, 5); m.u64(66);
        ASSERT_EQ(m.b.size(), declared_length('D'));
        EXPECT_EQ(OrderDelete{m.data()}.order_ref(), 66u);
    }
    {
        Builder m; m.header('U', 1, 5); m.u64(10); m.u64(11); m.u32(400); m.u32(999900);
        ASSERT_EQ(m.b.size(), declared_length('U'));
        OrderReplace r{m.data()};
        EXPECT_EQ(r.original_ref(), 10u);
        EXPECT_EQ(r.new_ref(), 11u);
        EXPECT_EQ(r.shares(), 400u);
        EXPECT_EQ(r.price(), 999900u);
    }
}

TEST(Itch, DecodesStockDirectory) {
    Builder m;
    m.header('R', 3, 1);
    m.text("TSLA", 8); m.ch('Q'); m.ch('N'); m.u32(100); m.ch('N'); m.ch('C');
    m.text("", 2); m.ch('P'); m.ch('N'); m.ch('N'); m.ch('1'); m.ch('N'); m.u32(0); m.ch('N');
    ASSERT_EQ(m.b.size(), declared_length('R'));
    StockDirectory d{m.data()};
    EXPECT_EQ(d.stock(), "TSLA");
    EXPECT_EQ(d.market_category(), 'Q');
    EXPECT_EQ(d.round_lot_size(), 100u);
    EXPECT_EQ(d.luld_tier(), '1');
}

TEST(Itch, FramingWalksEveryMessage) {
    Builder a; a.header('A', 1, 1); a.u64(1); a.ch('B'); a.u32(100); a.text("AAA", 8); a.u32(10000);
    Builder d; d.header('D', 1, 2); d.u64(1);
    Builder s; s.header('S', 0, 3); s.ch('O');
    const auto buf = frame({a.b, d.b, s.b});

    ParseStats st;
    std::vector<char> types;
    const std::size_t used = for_each_message(buf, st, [&](Header h, std::uint16_t len) {
        types.push_back(h.type());
        EXPECT_EQ(len, declared_length(h.type()));
    });
    EXPECT_EQ(used, buf.size());
    EXPECT_EQ(st.messages, 3u);
    EXPECT_EQ(st.unknown, 0u);
    EXPECT_EQ(types, (std::vector<char>{'A', 'D', 'S'}));
}

TEST(Itch, PartialTrailingFrameIsLeftForTheNextBlock) {
    Builder a; a.header('A', 1, 1); a.u64(1); a.ch('B'); a.u32(100); a.text("AAA", 8); a.u32(10000);
    auto buf = frame({a.b, a.b});
    buf.resize(buf.size() - 5);            // chop the last message in half

    ParseStats st;
    const std::size_t used = for_each_message(buf, st, [](Header, std::uint16_t) {});
    EXPECT_EQ(st.messages, 1u);
    EXPECT_EQ(used, 2 + declared_length('A')) << "consumed bytes must stop at the last whole message";
}

TEST(Itch, UnknownMessageTypeIsSkippedNotDesynchronised) {
    Builder a; a.header('A', 1, 1); a.u64(1); a.ch('B'); a.u32(100); a.text("AAA", 8); a.u32(10000);
    std::vector<std::byte> weird;          // a type this build does not model
    weird.push_back(static_cast<std::byte>('z'));
    for (int i = 0; i < 20; ++i) weird.push_back(std::byte{0});
    Builder d; d.header('D', 1, 2); d.u64(1);

    const auto buf = frame({a.b, weird, d.b});
    ParseStats st;
    std::vector<char> types;
    const std::size_t used = for_each_message(buf, st, [&](Header h, std::uint16_t) { types.push_back(h.type()); });

    EXPECT_EQ(used, buf.size()) << "framing must survive a message type we do not know";
    EXPECT_EQ(st.unknown, 1u);
    EXPECT_EQ(types, (std::vector<char>{'A', 'z', 'D'}));
}

TEST(Itch, PriceScanFindsRangeAndGrid) {
    // A quarter-cent grid. Enough samples that the 0.1/99.9 percentiles span
    // the whole set, so this checks range and grid detection rather than the
    // outlier trimming (which PriceScanSeparatesTheCoreFromTheTail covers).
    std::vector<std::vector<std::byte>> msgs;
    for (int i = 0; i < 200; ++i) {
        Builder b; b.header('A', 1, static_cast<std::uint64_t>(i));
        b.u64(static_cast<std::uint64_t>(i)); b.ch(i % 2 ? 'B' : 'S'); b.u32(100);
        b.text("AAA", 8);
        b.u32(static_cast<std::uint32_t>(995'000 + i * 2500));
        msgs.push_back(b.b);
    }
    const PriceRange pr = scan_price_range(frame(msgs));

    ASSERT_TRUE(pr.valid());
    EXPECT_EQ(pr.min_price, 995'000u);
    EXPECT_EQ(pr.max_price, 995'000u + 199u * 2500u);
    EXPECT_EQ(pr.priced, 200u);
    EXPECT_EQ(pr.tick_size(), 2500u) << "grid is the GCD of the prices on it";

    // The band widens the core by its own span on each side, snapped to grid.
    EXPECT_LE(pr.band_floor(), pr.core_low);
    EXPECT_EQ(pr.band_floor() % pr.tick_size(), 0u);
    EXPECT_GE(pr.band_floor() + pr.band_ticks() * pr.tick_size(), pr.core_high);
}

TEST(Itch, PriceScanIgnoresTheMarketPegSentinel) {
    // 0x7FFFFFFF is Nasdaq's "no price" sentinel; letting it into the range
    // would stretch the band across the entire 32-bit price space.
    Builder a1; a1.header('A', 1, 1); a1.u64(1); a1.ch('B'); a1.u32(100); a1.text("AAA", 8); a1.u32(1'000'000);
    Builder a2; a2.header('A', 1, 2); a2.u64(2); a2.ch('S'); a2.u32(100); a2.text("AAA", 8); a2.u32(0x7FFFFFFFu);
    Builder a3; a3.header('A', 1, 3); a3.u64(3); a3.ch('B'); a3.u32(100); a3.text("AAA", 8); a3.u32(0);
    const auto buf = frame({a1.b, a2.b, a3.b});

    const PriceRange pr = scan_price_range(buf);
    ASSERT_TRUE(pr.valid());
    EXPECT_EQ(pr.priced, 1u);
    EXPECT_EQ(pr.max_price, 1'000'000u);
}

TEST(Itch, PriceScanSeparatesTheCoreFromTheTail) {
    // One thousand prices on a penny grid around $100, plus two extreme
    // outliers of the kind real feeds carry. The core must survive both.
    std::vector<std::vector<std::byte>> msgs;
    for (int i = 0; i < 1000; ++i) {
        Builder b; b.header('A', 1, static_cast<std::uint64_t>(i));
        b.u64(static_cast<std::uint64_t>(i)); b.ch('B'); b.u32(100); b.text("AAA", 8);
        b.u32(static_cast<std::uint32_t>(1'000'000 + i * 100));
        msgs.push_back(b.b);
    }
    Builder lo; lo.header('A', 1, 9998); lo.u64(9998); lo.ch('B'); lo.u32(100); lo.text("AAA", 8); lo.u32(1);
    Builder hi; hi.header('A', 1, 9999); hi.u64(9999); hi.ch('S'); hi.u32(100); hi.text("AAA", 8); hi.u32(1'999'990'000u);
    msgs.push_back(lo.b); msgs.push_back(hi.b);

    const PriceRange pr = scan_price_range(frame(msgs));
    ASSERT_TRUE(pr.valid());
    EXPECT_EQ(pr.min_price, 1u);
    EXPECT_EQ(pr.max_price, 1'999'990'000u);
    // The $0.0001 outlier must not drag the grid down to a hundredth of a cent.
    EXPECT_EQ(pr.tick_size(), 100u) << "grid must be measured over the core, not the tail";
    EXPECT_GE(pr.core_low, 1'000'000u);
    EXPECT_LE(pr.core_high, 1'099'900u);
    EXPECT_GT(pr.outliers, 0u);
    EXPECT_LT(pr.outlier_fraction(), 0.01);
    EXPECT_LT(pr.band_ticks(), 10000u) << "the band must not stretch to reach the tail";
}

TEST(Itch, DeclaredLengthsMatchTheSpec) {
    // Guards against a typo in the offset tables: every modelled type has the
    // length Nasdaq documents, and unknown types report zero.
    EXPECT_EQ(declared_length('S'), 12u);
    EXPECT_EQ(declared_length('R'), 39u);
    EXPECT_EQ(declared_length('A'), 36u);
    EXPECT_EQ(declared_length('F'), 40u);
    EXPECT_EQ(declared_length('E'), 31u);
    EXPECT_EQ(declared_length('C'), 36u);
    EXPECT_EQ(declared_length('X'), 23u);
    EXPECT_EQ(declared_length('D'), 19u);
    EXPECT_EQ(declared_length('U'), 35u);
    EXPECT_EQ(declared_length('P'), 44u);
    EXPECT_EQ(declared_length('Q'), 40u);
    EXPECT_EQ(declared_length('I'), 50u);
    EXPECT_EQ(declared_length('*'), 0u);
}
