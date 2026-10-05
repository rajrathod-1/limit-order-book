// SPDX-License-Identifier: MIT
//
// Microbenchmarks: flat tick-indexed array book vs the std::map baseline.
//
// Every benchmark is parameterised by the number of distinct price levels the
// book spans, because that is the variable the two designs respond to
// differently. Array indexing does not care how wide the book is; a red-black
// tree does, and the gap widens as the book spreads out.
//
// Steady state matters here. A benchmark that inserts into an empty book
// measures nothing useful, so each one first builds a book of realistic depth
// and then holds it near constant size, pausing the clock only to refill.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "lob/engine.hpp"
#include "lob/map_book.hpp"

using namespace lob;

namespace {

constexpr std::size_t kTickCapacity = 1u << 16;
constexpr Qty         kLotSize       = 100;

// Sized per benchmark. The pool's free list is rebuilt in O(capacity) on reset,
// so handing every benchmark a four-million-slot pool would make a reset cost
// more than the thousands of operations it enables.
EngineConfig config(std::size_t max_orders) {
    EngineConfig c;
    c.scale         = kPennyTicks;
    c.tick_capacity = kTickCapacity;
    c.max_orders    = max_orders;
    return c;
}

// Ticks are centred so bids and asks straddle a mid and neither side is empty.
constexpr Ticks kMid = 20000;  // $200.00 on a penny grid from zero

struct Workload {
    std::vector<Ticks> ticks;
    std::vector<Qty>   qtys;
    std::vector<Side>  sides;
};

// A reproducible order flow spread over `levels` price levels either side of
// the mid, with depth thinning as you move away from the touch -- which is the
// shape a real book has and the shape that decides how often the top level
// empties.
Workload make_workload(std::size_t n, int levels, std::uint64_t seed = 42) {
    Workload w;
    w.ticks.reserve(n); w.qtys.reserve(n); w.sides.reserve(n);
    std::mt19937_64 rng(seed);
    // Geometric-ish: most orders near the touch, a tail further out.
    std::geometric_distribution<int> away(3.0 / std::max(1, levels));
    std::uniform_int_distribution<int> lots(1, 20);
    for (std::size_t i = 0; i < n; ++i) {
        const bool buy = (rng() & 1) != 0;
        int d = away(rng);
        if (d >= levels) d = levels - 1;
        w.sides.push_back(buy ? Side::Buy : Side::Sell);
        w.ticks.push_back(buy ? kMid - 1 - static_cast<Ticks>(d) : kMid + 1 + static_cast<Ticks>(d));
        w.qtys.push_back(static_cast<Qty>(lots(rng)) * kLotSize);
    }
    return w;
}

template <class Engine>
void fill_book(Engine& e, const Workload& w, std::size_t count, OrderId& next) {
    const auto scale = kPennyTicks;
    for (std::size_t i = 0; i < count; ++i)
        e.insert_passive(next++, w.sides[i % w.sides.size()],
                         scale.to_price(w.ticks[i % w.ticks.size()]),
                         w.qtys[i % w.qtys.size()]);
}

// ---------------------------------------------------------------------------
// Insert into a book already holding `kDepth` orders.
// ---------------------------------------------------------------------------
constexpr std::size_t kDepth = 100000;

template <class Engine>
void BM_Insert(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const Workload w = make_workload(1u << 16, levels);
    const auto scale = kPennyTicks;

    // Room for the steady-state book plus a long run of inserts before the
    // pool has to be drained.
    constexpr std::size_t kRun = 400000;
    Engine e(config(kDepth + kRun + 1024));
    OrderId next = 1;
    fill_book(e, w, kDepth, next);

    std::size_t i = 0, since_reset = 0;
    for (auto _ : state) {
        e.insert_passive(next++, w.sides[i & 0xFFFF], scale.to_price(w.ticks[i & 0xFFFF]),
                         w.qtys[i & 0xFFFF]);
        ++i;
        if (++since_reset >= kRun) {
            state.PauseTiming();
            e.clear();
            next = 1;
            fill_book(e, w, kDepth, next);
            since_reset = 0;
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = levels;
}

// ---------------------------------------------------------------------------
// Cancel an arbitrary resting order by id -- the operation a real cancel is.
// ---------------------------------------------------------------------------
template <class Engine>
void BM_Cancel(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const Workload w = make_workload(1u << 16, levels);

    Engine e(config(kDepth + 1024));
    OrderId next = 1;
    fill_book(e, w, kDepth, next);

    // Cancel in a shuffled order so the access pattern is not the insertion
    // order, which would flatter both books' cache behaviour.
    std::vector<OrderId> ids(kDepth);
    std::iota(ids.begin(), ids.end(), OrderId{1});
    std::shuffle(ids.begin(), ids.end(), std::mt19937_64(7));

    std::size_t k = 0;
    for (auto _ : state) {
        if (k >= ids.size()) {
            // Every order was cancelled, so the book is already empty and the
            // pool has reclaimed every slot. Refilling needs no clear().
            state.PauseTiming();
            next = 1;
            fill_book(e, w, kDepth, next);
            std::iota(ids.begin(), ids.end(), OrderId{1});
            std::shuffle(ids.begin(), ids.end(), std::mt19937_64(7));
            k = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(e.cancel(ids[k++]));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = levels;
}

// ---------------------------------------------------------------------------
// Cancel at the touch, repeatedly. This is the case the whole design is aimed
// at: emptying the best level forces the book to find the next best price. The
// array walks a three-level bitmap; the tree erases a node, rebalances, and
// then finds a new begin().
// ---------------------------------------------------------------------------
template <class Engine>
void BM_CancelTopOfBook(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const auto scale = kPennyTicks;

    Engine e(config(static_cast<std::size_t>(levels) * 2 + 64));
    OrderId next = 1;

    // One order per level on each side, so every cancel empties a level and
    // forces the book to find a new touch. Once they are all cancelled the book
    // is empty and every pool slot is back, so refilling needs no clear().
    auto refill = [&] {
        next = 1;
        for (int d = 1; d <= levels; ++d) {
            e.insert_passive(next++, Side::Buy,  scale.to_price(kMid - static_cast<Ticks>(d)), kLotSize);
            e.insert_passive(next++, Side::Sell, scale.to_price(kMid + static_cast<Ticks>(d)), kLotSize);
        }
    };
    refill();

    OrderId id = 1;
    for (auto _ : state) {
        if (id >= next) {
            state.PauseTiming();
            refill();
            id = 1;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(e.cancel(id++));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = levels;
}

// ---------------------------------------------------------------------------
// Read the touch. Both designs cache it, so this measures the floor cost of a
// market-data query rather than a structural difference.
// ---------------------------------------------------------------------------
template <class Engine>
void BM_TopOfBook(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const Workload w = make_workload(1u << 16, levels);
    Engine e(config(kDepth + 1024));
    OrderId next = 1;
    fill_book(e, w, kDepth, next);

    for (auto _ : state) {
        benchmark::DoNotOptimize(e.best_bid());
        benchmark::DoNotOptimize(e.best_ask());
    }
    state.SetItemsProcessed(state.iterations() * 2);
    state.counters["levels"] = levels;
}

// ---------------------------------------------------------------------------
// Aggressive order that consumes exactly one resting order: the per-fill cost
// of matching.
// ---------------------------------------------------------------------------
template <class Engine>
void BM_MatchSingleFill(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const auto scale = kPennyTicks;

    // Deep queues at each level so a single fill never empties one.
    constexpr int kPerLevel = 64;
    const std::size_t budget = static_cast<std::size_t>(levels) * kPerLevel;

    Engine e(config(budget + 1024));
    OrderId next = 1;

    // Each aggressive order consumes exactly one resting order, which returns
    // its pool slot, so the book drains rather than growing: refilling is a
    // plain re-insert with no clear().
    auto refill = [&] {
        next = 1;
        for (int d = 1; d <= levels; ++d)
            for (int k = 0; k < kPerLevel; ++k)
                e.insert_passive(next++, Side::Sell,
                                 scale.to_price(kMid + static_cast<Ticks>(d)), kLotSize);
    };
    refill();

    const Price touch = scale.to_price(kMid + static_cast<Ticks>(levels));
    std::size_t used = 0;

    OrderId agg = 1ull << 40;
    for (auto _ : state) {
        if (++used >= budget) {
            state.PauseTiming();
            refill();
            used = 0;
            state.ResumeTiming();
        }
        benchmark::DoNotOptimize(e.submit_limit(agg++, Side::Buy, touch, kLotSize, Tif::Ioc));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = levels;
}

// ---------------------------------------------------------------------------
// A realistic steady-state mix, weighted like the ITCH day measured in the
// README: roughly 45% adds, 45% cancels, 8% replaces, 2% executions.
// ---------------------------------------------------------------------------
template <class Engine>
void BM_MixedFlow(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const Workload w = make_workload(1u << 16, levels);
    const auto scale = kPennyTicks;

    constexpr std::size_t kRun = 400000;
    Engine e(config(kDepth + kRun + 1024));
    OrderId next = 1;
    fill_book(e, w, kDepth, next);

    std::vector<OrderId> live(kDepth);
    std::iota(live.begin(), live.end(), OrderId{1});
    std::mt19937_64 rng(11);

    std::size_t i = 0;
    for (auto _ : state) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 45 || live.size() < 1000) {
            const auto j = i & 0xFFFF;
            if (e.insert_passive(next, w.sides[j], scale.to_price(w.ticks[j]), w.qtys[j]) == Status::Ok)
                live.push_back(next);
            ++next;
        } else if (roll < 90) {
            const std::size_t k = rng() % live.size();
            e.cancel(live[k]);
            live[k] = live.back(); live.pop_back();
        } else if (roll < 98) {
            const std::size_t k = rng() % live.size();
            const auto j = i & 0xFFFF;
            if (e.replace_passive(live[k], next, scale.to_price(w.ticks[j]), w.qtys[j]) == Status::Ok)
                live[k] = next;
            ++next;
        } else {
            const std::size_t k = rng() % live.size();
            if (e.execute(live[k], kLotSize) != Status::Ok) {
                live[k] = live.back(); live.pop_back();
            }
        }
        ++i;

        if (next > kRun) {
            state.PauseTiming();
            e.clear();
            next = 1;
            fill_book(e, w, kDepth, next);
            live.resize(kDepth);
            std::iota(live.begin(), live.end(), OrderId{1});
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = levels;
}

using Array = MatchingEngine<NullSink>;
using Map   = MapMatchingEngine<NullSink>;


}  // namespace

// Book widths, from a tight single-name book to a very spread-out one.
#define LOB_WIDTHS {8, 64, 512, 4096}

// BM_CancelTopOfBook holds exactly one order per level, so a narrow book is
// exhausted in a few iterations and the harness spends its time refilling
// rather than measuring. It is a benchmark about wide books; give it wide ones.
#define LOB_WIDE_WIDTHS {256, 1024, 4096, 16384}

#define LOB_COMPARE_W(BM, WIDTHS)                                           \
    BENCHMARK_TEMPLATE(BM, Array)->ArgsProduct({WIDTHS})                    \
        ->Name(#BM "/array")->Unit(benchmark::kNanosecond);                 \
    BENCHMARK_TEMPLATE(BM, Map)->ArgsProduct({WIDTHS})                      \
        ->Name(#BM "/map")->Unit(benchmark::kNanosecond);

#define LOB_COMPARE(BM) LOB_COMPARE_W(BM, LOB_WIDTHS)

LOB_COMPARE(BM_Insert)
LOB_COMPARE(BM_Cancel)
LOB_COMPARE_W(BM_CancelTopOfBook, LOB_WIDE_WIDTHS)
LOB_COMPARE(BM_TopOfBook)
LOB_COMPARE(BM_MatchSingleFill)
LOB_COMPARE(BM_MixedFlow)

BENCHMARK_MAIN();
