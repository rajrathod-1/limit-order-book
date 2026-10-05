// SPDX-License-Identifier: MIT
//
// lob_replay -- replay an ITCH 5.0 stream through both books and compare them.
//
// The run is split into phases that each measure exactly one thing, because
// mixing them produces numbers that cannot be interpreted:
//
//   1. Correctness. Both books are driven through the identical stream in
//      lockstep and compared, repeatedly, *during* the session. Comparing only
//      at the end would prove nothing: Nasdaq cancels every resting order at
//      the close, so a book checked at the last message is an empty book.
//
//   2. Per-operation latency, flat array book. Each operation is timestamped
//      individually with the cycle counter and exact percentiles are reported.
//      Percentiles, not averages: the tail is the number that matters.
//
//   3. Throughput. Both books replay the same stream with timing switched off
//      at compile time, so neither pays for the measurement. Timing one and not
//      the other would hand the untimed book a ~34 ns per operation head start.
//
//   4. Match latency. Genuine aggressive orders sweep 1, 2, 5 and 10 levels of
//      the book, rebuilt to its deepest point in the session.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bench/latency.hpp"
#include "itch/replay.hpp"
#include "lob/engine.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

// A flat level array is only worth having while it stays cache-resident enough
// to beat a tree. Past this the configuration is wrong, not the design.
constexpr std::size_t kMaxBandTicks = 1u << 21;

struct Options {
    std::string   data;
    bool          validate     = true;
    bool          measure      = true;
    bool          throughput   = true;
    std::uint64_t max_messages = 0;
    std::uint64_t check_every  = 20000;
    std::uint32_t match_trials = 20000;
};

void usage(const char* a0) {
    std::fprintf(stderr,
        "usage: %s --data <file.itch> [options]\n\n"
        "  --data FILE        ITCH 5.0 BinaryFILE (raw, not gzipped)\n"
        "  --max-messages N   replay only the first N messages\n"
        "  --check-every N    cross-check the two books every N messages (default 20000)\n"
        "  --match-trials N   aggressive orders to time (default 20000)\n"
        "  --no-validate      skip the cross-check phase\n"
        "  --no-measure       skip the per-operation latency phase\n"
        "  --no-throughput    skip the throughput phase\n", a0);
}

// Captures fills so the match benchmark can put back the liquidity it consumed
// and leave the book as it found it for the next trial.
struct FillCapture {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) { fills.push_back(f); }
    void on_accepted(OrderId, Side, Price, Qty) noexcept {}
    void on_cancelled(OrderId, Side, Price, Qty) noexcept {}
    void on_replaced(OrderId, OrderId, Side, Price, Qty) noexcept {}
    void on_rejected(OrderId, Status) noexcept {}
};

constexpr double dollars(Price p) { return static_cast<double>(p) / 1e4; }

double wall_seconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Drives one engine over the whole file. Returns wall seconds.
template <class Engine, bool Measure>
double replay(Engine& engine, std::span<const std::byte> data, std::uint64_t cap,
              itch::ReplayTimers* timers, itch::ReplayStats& out) {
    itch::ParseStats ps;
    itch::Rebuilder<Engine, Measure> rb(engine, timers);
    std::uint64_t n = 0;
    const double t0 = wall_seconds();
    itch::for_each_message(data, ps, [&](itch::Header h, std::uint16_t) {
        if (cap && n >= cap) return;
        rb.apply(h);
        ++n;
    });
    const double secs = wall_seconds() - t0;
    out = rb.stats();
    return secs;
}

std::map<Price, std::uint64_t> array_depth(const MatchingEngine<NullSink>& e, Side s) {
    std::map<Price, std::uint64_t> d;
    if (s == Side::Buy)
        e.bids().top_levels(1u << 30, [&](Price p, const PriceLevel& l) { d[p] = l.total_qty; });
    else
        e.asks().top_levels(1u << 30, [&](Price p, const PriceLevel& l) { d[p] = l.total_qty; });
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--data")         opt.data = next();
        else if (a == "--max-messages") opt.max_messages = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--check-every")  opt.check_every  = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--match-trials") opt.match_trials = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--no-validate")   opt.validate   = false;
        else if (a == "--no-measure")    opt.measure    = false;
        else if (a == "--no-throughput") opt.throughput = false;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (opt.data.empty()) { usage(argv[0]); return 2; }

    // ---- phase 0: profile the data and size the book -----------------------
    itch::MappedFile file(opt.data);
    std::printf("\n=== data =================================================================\n");
    std::printf("  file        : %s (%.1f MB)\n", opt.data.c_str(), static_cast<double>(file.size()) / 1e6);

    const itch::PriceRange pr = itch::scan_price_range(file.bytes());
    if (!pr.valid()) { std::fprintf(stderr, "no priced messages found in %s\n", opt.data.c_str()); return 1; }

    std::printf("  symbol      : %s\n", pr.symbol.empty() ? "(none in file)" : pr.symbol.c_str());
    std::printf("  price range : $%.4f .. $%.4f over %" PRIu64 " priced messages\n",
                pr.min_price / 1e4, pr.max_price / 1e4, pr.priced);
    std::printf("  dense core  : $%.4f .. $%.4f  (%.3f%% of prices sit outside it)\n",
                pr.core_low / 1e4, pr.core_high / 1e4, 100.0 * pr.outlier_fraction());
    std::printf("  price grid  : $%.4f, measured over the core\n", pr.tick_size() / 1e4);

    const auto        tick_size = static_cast<Price>(pr.tick_size());
    const std::size_t ticks     = static_cast<std::size_t>(pr.band_ticks());
    if (ticks > kMaxBandTicks) {
        std::fprintf(stderr,
            "price band needs %zu ticks, beyond the %zu-tick cap.\n"
            "Split the file per symbol with itch_filter first.\n", ticks, kMaxBandTicks);
        return 1;
    }

    EngineConfig cfg;
    cfg.scale         = TickScale{tick_size, static_cast<Price>(pr.band_floor())};
    cfg.tick_capacity = ticks;
    cfg.max_orders    = 1u << 21;

    std::printf("  book band   : $%.4f .. $%.4f, %zu ticks (%.1f MB of level array per side)\n",
                dollars(cfg.scale.floor_price()),
                dollars(cfg.scale.to_price(static_cast<Ticks>(ticks - 1))),
                ticks, static_cast<double>(ticks * sizeof(PriceLevel)) / 1e6);
    std::printf("  prices outside the band rest in the book's far map.\n");

    std::printf("\n=== clock ================================================================\n");
    std::printf("  source      : %s\n", bench::Clock::source());
    std::printf("  frequency   : %.6f GHz measured", static_cast<double>(bench::Clock::frequency()) / 1e9);
    if (const auto d = bench::Clock::declared_frequency())
        std::printf(", %.6f GHz declared (CNTFRQ_EL0)", static_cast<double>(d) / 1e9);
    std::printf("\n  resolution  : %.3f ns per tick\n", bench::Clock::ns_per_tick());
    std::printf("  read cost   : %" PRIu64 " ticks (%.1f ns) -- subtracted from every sample\n",
                bench::Clock::overhead_ticks(),
                bench::Clock::ticks_to_ns(static_cast<double>(bench::Clock::overhead_ticks())));

    const std::uint64_t cap = opt.max_messages;
    int exit_code = 0;

    // Where in the stream the book was at its deepest. The match benchmark
    // rebuilds to this point, because benchmarking a sweep against the empty
    // book Nasdaq leaves at the close would measure nothing.
    std::uint64_t peak_index = 0;
    std::uint64_t peak_depth = 0;

    // ---- phase 1: correctness ---------------------------------------------
    if (opt.validate) {
        std::printf("\n=== cross-check: flat array vs std::map, during the session ===============\n");

        MatchingEngine<NullSink>    fast(cfg);
        MapMatchingEngine<NullSink> slow(cfg);
        itch::Rebuilder<MatchingEngine<NullSink>, false>    rb_fast(fast);
        itch::Rebuilder<MapMatchingEngine<NullSink>, false> rb_slow(slow);

        itch::ParseStats ps;
        std::uint64_t n = 0, checks = 0, problems = 0, levels_compared = 0;
        std::uint64_t max_levels = 0;

        itch::for_each_message(file.bytes(), ps, [&](itch::Header h, std::uint16_t) {
            if (cap && n >= cap) return;
            if (problems > 20) return;
            rb_fast.apply(h);
            rb_slow.apply(h);
            ++n;

            const auto depth = static_cast<std::uint64_t>(fast.live_orders());
            if (depth > peak_depth) { peak_depth = depth; peak_index = n; }

            if (n % opt.check_every != 0) return;
            ++checks;

            auto bad = [&](const char* what, long long a, long long b) {
                if (a == b) return;
                if (problems < 10)
                    std::printf("  MISMATCH at message %" PRIu64 ": %-16s array=%-14lld map=%lld\n",
                                n, what, a, b);
                ++problems;
            };
            bad("live orders",  (long long)fast.live_orders(), (long long)slow.live_orders());
            bad("best bid",     (long long)fast.best_bid(),    (long long)slow.best_bid());
            bad("best ask",     (long long)fast.best_ask(),    (long long)slow.best_ask());
            bad("best bid qty", (long long)fast.bids().best_qty(), (long long)slow.best_bid_qty());
            bad("best ask qty", (long long)fast.asks().best_qty(), (long long)slow.best_ask_qty());

            const auto fb = array_depth(fast, Side::Buy),  sb = slow.depth(Side::Buy);
            const auto fa = array_depth(fast, Side::Sell), sa = slow.depth(Side::Sell);
            if (fb != sb) { if (problems < 10) std::printf("  MISMATCH at message %" PRIu64 ": bid depth\n", n); ++problems; }
            if (fa != sa) { if (problems < 10) std::printf("  MISMATCH at message %" PRIu64 ": ask depth\n", n); ++problems; }
            levels_compared += fb.size() + fa.size();
            max_levels = std::max<std::uint64_t>(max_levels, fb.size() + fa.size());
        });

        std::printf("  %" PRIu64 " messages, %" PRIu64 " full-book comparisons\n", n, checks);
        std::printf("  %" PRIu64 " price levels compared (deepest snapshot: %" PRIu64 " levels)\n",
                    levels_compared, max_levels);
        std::printf("  peak depth %" PRIu64 " resting orders at message %" PRIu64 "\n",
                    peak_depth, peak_index);
        std::printf("  far-book use over the session: %zu of %" PRIu64
                    " inserts took the slow path (%.4f%%)\n",
                    fast.bids().far_inserts() + fast.asks().far_inserts(),
                    rb_fast.stats().adds + rb_fast.stats().replaces,
                    100.0 * static_cast<double>(fast.bids().far_inserts() + fast.asks().far_inserts()) /
                        static_cast<double>(std::max<std::uint64_t>(1, rb_fast.stats().adds + rb_fast.stats().replaces)));

        // A comparison that compared nothing must not be reported as a pass.
        if (levels_compared == 0) {
            std::printf("  INCONCLUSIVE: the book was empty at every checkpoint.\n");
            exit_code = 1;
        } else if (problems == 0) {
            std::printf("  OK: the two books agreed at every checkpoint.\n");
        } else {
            std::printf("  FAILED with %" PRIu64 " mismatches\n", problems);
            exit_code = 1;
        }
    }

    // ---- phase 2: per-operation latency ------------------------------------
    if (opt.measure) {
        std::printf("\n=== per-operation latency, flat array book (ns) ==========================\n");
        MatchingEngine<NullSink> e(cfg);
        e.prefault();
        itch::ReplayTimers timers;
        timers.reserve(1u << 22);
        itch::ReplayStats stats;
        replay<MatchingEngine<NullSink>, true>(e, file.bytes(), cap, &timers, stats);

        std::printf("  adds=%" PRIu64 " deletes=%" PRIu64 " reduces=%" PRIu64
                    " execs=%" PRIu64 " replaces=%" PRIu64 " rejects=%" PRIu64 "\n",
                    stats.adds, stats.deletes, stats.reduces, stats.executes,
                    stats.replaces, stats.rejected);
        std::printf("  pool high-water %zu of %zu slots\n\n",
                    e.pool().high_water(), e.pool().capacity());

        bench::print_header();
        for (auto* r : {&timers.insert, &timers.cancel, &timers.reduce,
                        &timers.execute, &timers.replace})
            if (r->count()) bench::print_summary(r->summarize());
    }

    // ---- phase 3: throughput, neither side timed ---------------------------
    if (opt.throughput) {
        std::printf("\n=== throughput: same stream, no per-operation timing on either side ======\n");

        itch::ReplayStats astats, mstats;
        double asecs, msecs;
        {
            MatchingEngine<NullSink> e(cfg);
            e.prefault();
            asecs = replay<MatchingEngine<NullSink>, false>(e, file.bytes(), cap, nullptr, astats);
        }
        {
            MapMatchingEngine<NullSink> e(cfg);
            msecs = replay<MapMatchingEngine<NullSink>, false>(e, file.bytes(), cap, nullptr, mstats);
        }

        const auto ops = astats.adds + astats.deletes + astats.reduces +
                         astats.executes + astats.replaces;
        const double a = static_cast<double>(ops) / asecs / 1e6;
        const double m = static_cast<double>(ops) / msecs / 1e6;
        std::printf("  %" PRIu64 " book operations from %" PRIu64 " messages\n", ops, astats.adds +
                    astats.deletes + astats.reduces + astats.executes + astats.replaces + astats.trades);
        std::printf("  flat array book : %7.2f M ops/s  (%.3f s, %.1f ns/op)\n",
                    a, asecs, asecs * 1e9 / static_cast<double>(ops));
        std::printf("  std::map book   : %7.2f M ops/s  (%.3f s, %.1f ns/op)\n",
                    m, msecs, msecs * 1e9 / static_cast<double>(ops));
        std::printf("  speedup         : %7.2fx\n", a / m);
    }

    // ---- phase 4: match latency at the deepest book ------------------------
    if (opt.match_trials > 0 && peak_index > 0) {
        std::printf("\n=== match latency: aggressive order sweeping N levels (ns) ===============\n");
        std::printf("  book rebuilt to message %" PRIu64 " (%" PRIu64 " resting orders)\n",
                    peak_index, peak_depth);

        MatchingEngine<FillCapture> me(cfg);
        me.prefault();
        {
            itch::ParseStats ps;
            itch::Rebuilder<MatchingEngine<FillCapture>, false> rb(me);
            std::uint64_t n = 0;
            itch::for_each_message(file.bytes(), ps, [&](itch::Header h, std::uint16_t) {
                if (n >= peak_index) return;
                rb.apply(h);
                ++n;
            });
        }
        me.sink().fills.clear();

        std::printf("  best bid %.4f x %llu   /   best ask %.4f x %llu\n\n",
                    dollars(me.best_bid()), (unsigned long long)me.bids().best_qty(),
                    dollars(me.best_ask()), (unsigned long long)me.asks().best_qty());

        bench::print_header();
        OrderId next_id = 1ull << 62;

        for (int levels : {1, 2, 5, 10}) {
            bench::LatencyRecorder rec("sweep " + std::to_string(levels) + " level(s)",
                                       opt.match_trials);
            std::uint32_t done = 0;

            for (std::uint32_t trial = 0; trial < opt.match_trials * 2 && done < opt.match_trials; ++trial) {
                const bool buy  = (trial & 1) == 0;   // alternate so neither side erodes
                const Side side = buy ? Side::Buy : Side::Sell;

                // Walk `levels` deep on the opposite side to find the limit
                // price and the exact quantity that consumes them.
                Qty   want  = 0;
                Price limit = 0;
                int   found = 0;
                if (buy) {
                    for (Price p = me.asks().best_price();
                         p != BookSide<Side::Sell>::kNoPrice && found < levels;
                         p = me.asks().next_price(p)) {
                        want += static_cast<Qty>(me.asks().qty_at(p));
                        limit = p;
                        ++found;
                    }
                } else {
                    for (Price p = me.bids().best_price();
                         p != BookSide<Side::Buy>::kNoPrice && found < levels;
                         p = me.bids().next_price(p)) {
                        want += static_cast<Qty>(me.bids().qty_at(p));
                        limit = p;
                        ++found;
                    }
                }
                if (found < levels || want == 0) continue;

                me.sink().fills.clear();

                const auto t0 = bench::Clock::now();
                me.submit_limit(next_id++, side, limit, want, Tif::Ioc);
                const auto t1 = bench::Clock::now();
                rec.add_ticks(t1 - t0);
                ++done;

                // Put back exactly what was consumed, so every trial starts
                // from an equivalent book.
                const Side passive = opposite(side);
                for (const Fill& f : me.sink().fills)
                    me.insert_passive(next_id++, passive, f.price, f.qty);
            }

            if (done) bench::print_summary(rec.summarize());
            else std::printf("  sweep %2d level(s): book too thin to run\n", levels);
        }
    }

    std::printf("\n");
    return exit_code;
}
