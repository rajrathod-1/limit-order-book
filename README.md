# Limit Order Book & Matching Engine

A price–time priority matching engine in C++20, benchmarked against a `std::map`
baseline on replayed Nasdaq TotalView-ITCH 5.0 data.

Resting orders live in intrusive doubly linked lists per price level, indexed by
a flat tick-keyed array, so best bid/ask lookup and cancellation are O(1) rather
than a tree traversal. The hot path performs no allocation: order nodes come
from a preallocated pool. Latency is measured per operation with the CPU cycle
counter and reported as exact percentiles.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build
build/itch_gen --out /tmp/synth.itch --messages 2000000   # no download needed
build/lob_replay --data /tmp/synth.itch
```

**In the browser.** The same headers also compile to WebAssembly (`web/`): a live
book you can sweep with market orders, and the array-versus-`std::map` benchmark
running in your own tab. `web/build.sh` rebuilds it with Emscripten; the output
is committed, so the page deploys as plain static files.

---

## Results

Replaying a full Nasdaq session for AAPL, 2019-12-30 — 1,519,870 real ITCH
messages driving 1,512,179 book operations. Apple M4 Pro, Apple clang 21,
`-O3 -mcpu=native`.

**Throughput.** Neither book is timed per operation here, so the comparison is
like for like. Median of three runs:

| book | throughput | per operation |
|---|---:|---:|
| flat tick-indexed array | **27.04 M ops/s** | **37.0 ns** |
| `std::map` baseline | 12.22 M ops/s | 81.8 ns |
| | **2.20× faster** | |

**Per-operation latency**, flat array book, in nanoseconds. Every operation is
individually timestamped with `cntvct_el0`; percentiles are exact, computed from
the raw samples rather than histogram buckets:

| operation | count | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| insert (ITCH add) | 698,744 | 108 | 149 | 192 | 316 | 3,900 | 117,679 |
| cancel (ITCH delete) | 654,457 | 24 | 25 | 150 | 275 | 524 | 13,275 |
| reduce (ITCH cancel) | 1,467 | <17 | 25 | 108 | 233 | 233 | 233 |
| match (ITCH execute) | 60,543 | 24 | 25 | 108 | 192 | 441 | 11,108 |
| replace (ITCH replace) | 96,968 | 108 | 150 | 192 | 316 | 4,400 | 10,334 |

**Match latency** — genuine aggressive orders submitted into the rebuilt book at
its deepest point in the session (27,110 resting orders), sized to sweep exactly
N price levels:

| sweep | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| 1 level | 108 | 149 | 191 | 233 |
| 2 levels | 108 | 191 | 275 | 358 |
| 5 levels | 233 | 316 | 400 | 608 |
| 10 levels | 441 | 525 | 650 | 899 |

**Across five symbols from the same session**, median of three runs each. The
speedup tracks how wide the book is, which is exactly what the design predicts —
array indexing is insensitive to book width, a red-black tree is not:

| symbol | messages | book width | array | `std::map` | speedup |
|---|---:|---:|---:|---:|---:|
| TSLA | 545,569 | 156,890 ticks | 23.42 M/s | 10.32 M/s | **2.27×** |
| AAPL | 1,519,870 | 53,577 ticks | 27.04 M/s | 12.22 M/s | **2.20×** |
| MSFT | 1,221,483 | 53,077 ticks | 28.17 M/s | 14.40 M/s | **1.95×** |
| AMD | 1,512,679 | 16,001 ticks | 30.91 M/s | 16.74 M/s | **1.85×** |
| INTC | 753,348 | 18,793 ticks | 28.06 M/s | 17.37 M/s | **1.62×** |

Run-to-run spread on the speedup is roughly ±0.1× on an unpinned laptop; the
ordering across symbols is stable.

### Microbenchmarks

Google Benchmark, isolating single operations against a book of a given width.
Width is the parameter that matters: array indexing is insensitive to how many
distinct price levels exist, a red-black tree is not.

**Insert** — array stays flat, the tree does not:

| price levels | array | `std::map` | speedup |
|---:|---:|---:|---:|
| 8 | 32.2 ns | 31.6 ns | 0.98× |
| 64 | 37.2 ns | 39.5 ns | 1.06× |
| 512 | 33.1 ns | 50.1 ns | 1.51× |
| 4,096 | 33.6 ns | 67.9 ns | **2.02×** |

**Cancel an arbitrary resting order** — the operation a real cancel is:

| price levels | array | `std::map` | speedup |
|---:|---:|---:|---:|
| 8 | 21.2 ns | 59.5 ns | 2.81× |
| 64 | 20.8 ns | 67.6 ns | 3.25× |
| 512 | 21.6 ns | 91.7 ns | 4.25× |
| 4,096 | 24.1 ns | 128 ns | **5.31×** |

**Cancel at the touch** — this is the case the whole design is aimed at. Emptying
the best level forces the book to find the next best price: the array walks three
bitmap words, the tree erases a node, rebalances, and re-finds `begin()`.

| price levels | array | `std::map` | speedup |
|---:|---:|---:|---:|
| 256 | 7.09 ns | 45.8 ns | **6.46×** |
| 1,024 | 8.17 ns | 46.3 ns | 5.67× |
| 4,096 | 9.94 ns | 49.8 ns | 5.01× |
| 16,384 | 13.9 ns | 55.6 ns | 4.00× |

**Realistic mixed flow** — 45% adds, 45% cancels, 8% replaces, 2% executions,
weighted like the measured ITCH day:

| price levels | array | `std::map` | speedup |
|---:|---:|---:|---:|
| 8 | 52.0 ns | 99.7 ns | 1.92× |
| 64 | 53.6 ns | 134 ns | 2.50× |
| 512 | 50.7 ns | 148 ns | 2.92× |
| 4,096 | 51.5 ns | 208 ns | **4.04×** |

**Where the array does not win.** Reading top of book is *slower*: 1.27 ns versus
0.55 ns, because `std::map` caches its leftmost node and returns it with one
dereference, while the array checks the far map, checks the cached tick, and
converts it to a price. Both are around a nanosecond and neither is a bottleneck,
but the honest summary is that the array wins on **mutation**, not on reads. Single
fills are similar: 10.8 ns versus 28.9 ns on a narrow book, converging to a tie
(27.2 vs 28.5 ns) once the book is wide enough that both are memory-bound.

### Reading these numbers honestly

- **`<17` means below the measurement floor.** A serialised counter read costs
  17 ns on this machine. That cost is measured at startup, reported, and
  subtracted from every sample, but it also means an operation faster than ~17 ns
  cannot be resolved individually. Half of all executes land there. The
  throughput column is the reliable number for operations that small.
- **The maxima are the operating system, not the book.** A 24 µs cancel is a
  scheduler preemption or a page fault, not a data structure. This is a
  benchmark on a laptop under a general-purpose OS, with no core pinning and no
  isolated CPU. The p99.9 column is the last one that describes the code.
- **The baseline is a real opponent, not a straw man.** It uses `std::map` for
  price levels, `std::list` per level, and an `unordered_map` from order id to a
  stored list iterator so that cancels are O(1) there too. It is the design most
  people write first, implemented properly.

---

## Design

### Flat tick-keyed array, not a tree

A price level is found by address arithmetic: `levels_[tick]`. No comparisons,
no tree descent, no pointer chasing. Prices arrive as fixed-point integers with
four implied decimals (ITCH's own representation), and a tick scale converts
them to a dense array index.

Keeping the top of book correct is the part that actually needs care. Caching
`best_bid` makes the query O(1), but the moment the best level is fully consumed
or cancelled away, something has to find the next populated price. Scanning the
array degrades badly across a wide, sparse book — exactly the shape a real
symbol has.

So occupancy is tracked in a **three-level hierarchical bitmap**: one bit per
tick, one bit per 64 ticks, one bit per 4,096 ticks. Finding the next populated
level is at most three `countl_zero`/`countr_zero` operations regardless of how
far apart the levels are, and a set/clear touches at most three words.

### The far book

Real feeds are not tidy. Replaying AAPL, 99.8% of orders price within a few
percent of the touch on an exact penny grid — but the rest include resting bids
at **$0.0001** and offers at **$199,999**, some off the penny grid entirely.
Sizing a flat array to reach those would need two billion slots.

Rejecting them was not acceptable either: they are real orders, and a book that
silently drops them is a book that disagrees with the exchange.

So each side is a dense array covering a measured price band **plus** an ordered
map holding whatever falls outside it. The two are merged into one correctly
ordered book by `best_price()` and `next_price()`. On the AAPL session this slow
path took **52 of 795,712 inserts — 0.0065%**, and the fast path pays one
predictable `empty()` test for it.

The band itself is derived from the data rather than guessed: a pre-pass
computes the 0.1st and 99.9th percentiles of the price distribution, and the
tick grid as the GCD of the prices inside that core. Computing the GCD over the
*whole* distribution would be dominated by a single sub-penny outlier and
collapse the grid to $0.0001, inflating the array a hundredfold. Measured this
way, AAPL and TSLA come out on a penny grid; MSFT, AMD and INTC come out on a
half-cent grid, which is genuinely what their price improvement traffic uses.

### Intrusive lists, 32-byte orders

Each price level owns a FIFO queue threaded directly through the order nodes.
Links are 32-bit **pool indices**, not pointers — that halves the link fields
and is what keeps an order at exactly 32 bytes, so two share a cache line:

```cpp
struct Order {
    Handle       next, prev;   // pool indices, not pointers
    Qty          qty;
    std::int32_t loc;          // tick index, or raw price when kFar is set
    OrderId      id;
    Seq          seq;
    Side side; Tif tif; std::uint8_t flags, _pad;
};
static_assert(sizeof(Order) == 32);
```

A `static_assert` enforces the size, so adding a field fails the build rather
than quietly halving match throughput. `loc` does double duty — tick index for
in-band orders, raw price for far ones — which is what buys the far book without
growing the struct. Nasdaq's maximum wire price fits in 32 bits.

Cancellation names an order, not a position, so it must not walk the queue.
Holding a handle means unlinking is a constant number of stores.

### No allocation on the hot path

Order nodes come from a fixed-capacity slab with an intrusive free list threaded
through the free slots themselves. `acquire()` and `release()` are a handful of
instructions and cannot fail except by exhausting the slab, which is **reported
as a status, not papered over with a fallback allocation** — a full book is an
operational condition, not a bug.

Two consequences worth noting:

- An order that fully fills on arrival never touches the pool at all: a handle
  is only acquired for a remainder that actually needs to rest.
- `OrderId → Handle` is a purpose-built open-addressing table with **backward-shift
  deletion**, not `std::unordered_map`. A trading day is overwhelmingly add/cancel
  churn, and a tombstoned table degrades toward a linear scan over the session.
  Backward-shift keeps every probe sequence contiguous, so the table performs the
  same at the close as it did at the open.

### Matching semantics

Price first, then arrival time. The passive order's price is the trade price, so
an aggressive order can be price-improved but never pays worse than its limit.

- **Limit**, **market**, **cancel**, **cancel/replace**, and partial **reduce**.
- **IOC** cancels its remainder; **FOK** is genuinely atomic — it checks
  fillability before touching anything, so a killed FOK leaves the book bit-identical.
- **Replace loses time priority**, unconditionally, and is re-matched on the way
  in because the new price may now cross. That is Nasdaq's rule, and the only one
  fair to the orders that queued behind the original.
- **Reduce keeps queue position**, which is what an ITCH Order Cancel (`X`) means:
  a partial reduction, not a delete.

---

## Correctness

A performance claim about two data structures is meaningless unless they agree
on the answer.

**Differential testing.** Both engines are driven through hundreds of thousands
of randomly generated operations and must agree on *every fill, in order, at the
same price and size*, and on the full book after. 3% of generated prices land
outside the band or off the grid on purpose, so the far-book path is covered by
the same comparison. Unit tests check the cases somebody thought of; this checks
the ones nobody did.

**Cross-check on real data, during the session.** `lob_replay` drives both books
through the identical ITCH stream and compares them repeatedly *mid-session* —
every populated level on both sides, plus top of book and live order count.

This detail matters. An earlier version compared only at the end of the file and
reported a clean pass — but Nasdaq cancels every resting order at the close, so
it was comparing two empty books and proving nothing. The check now runs at
intervals and **fails loudly if it never compared a populated book**:

```
1519870 messages, 75 full-book comparisons
352992 price levels compared (deepest snapshot: 5304 levels)
peak depth 27110 resting orders at message 1491240
OK: the two books agreed at every checkpoint.
```

**The ITCH decoder is validated against the real feed**: all 268,744,780
messages of the 2019-12-30 session parse with zero unknown message types and
zero framing errors, every spec length matching the wire framing.

73 tests cover the pool, the hierarchical bitmap (against a `std::set` oracle),
the order index under 200,000 operations of churn, book mechanics, matching
semantics, wire decoding, and the two differential suites. CI builds Debug and
Release on Linux and macOS, plus an ASan/UBSan run.

---

## Layout

```
include/lob/
  types.hpp          strong types, fixed-point prices, tick scale
  object_pool.hpp    preallocated slab, intrusive free list
  intrusive_list.hpp doubly linked list threaded through the pool
  bitset_index.hpp   three-level hierarchical occupancy bitmap
  array_book.hpp     one side: flat tick array + far map
  order_index.hpp    open-addressing OrderId -> Handle, backward-shift delete
  engine.hpp         the matching engine
  map_book.hpp       the std::map baseline
include/itch/
  itch50.hpp         ITCH 5.0 wire decoding
  reader.hpp         BinaryFILE framing, mmap and streaming gzip
  replay.hpp         ITCH -> book operations
include/bench/
  clock.hpp          rdtscp / cntvct_el0 with calibration and overhead
  latency.hpp        exact-percentile latency recorder
apps/
  lob_replay.cpp     the driver: cross-check, latency, throughput, match
  itch_filter.cpp    slice a Nasdaq day file into per-symbol corpora
  itch_gen.cpp       synthetic ITCH generator, so no download is required
web/
  wasm.cpp           the engine's C ABI for the browser, plus the mixed-flow benchmark
  build.sh           em++ -> engine.js + engine.wasm (committed)
  index.html, app.js the live demo page
```

---

## Data

Everything runs without any download — `itch_gen` emits a synthetic ITCH 5.0
stream through the same decoder and framing as the real thing, with a book depth
that mean-reverts to a target the way a real one does.

For real data, Nasdaq publishes full sample days at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` — no account, no key:

```
./scripts/fetch_itch.sh                 # ~3.5 GB download, then per-symbol slices
build/lob_replay --data data/symbols/AAPL.itch
```

A day file is 3.5 GB compressed and 8.25 GB of messages. `itch_filter` streams
the gzip and never writes the decompressed form to disk, extracting every
requested symbol in a single pass (AAPL comes out at 46 MB).

The one genuinely fiddly part: only Add Order messages carry a stock symbol.
Execute, Cancel, Delete and Replace name an order reference number and nothing
else, so the filter tracks which references belong to which symbol and follows
Replace as it renumbers an order mid-flight.

---

## What this is not

- **Single-threaded and single-symbol per book.** That is the right shape for a
  matching engine core — a real venue shards by symbol and keeps each book on one
  thread precisely to avoid synchronisation on the hot path — but there is no
  threading story here, and no lock-free input queue in front of it.
- **No session layer.** It reads Nasdaq's BinaryFILE format, not live MoldUDP64
  multicast, so there is no gap detection, no recovery and no sequencing.
- **No exchange surround.** No auctions, no halts, no LULD bands, no self-trade
  prevention, no fee tiers, no risk checks, no persistence or replay journal.
- **Benchmarked on a laptop.** No core pinning, no isolated CPUs, no tickless
  kernel. The tail beyond p99.9 is the operating system, and it is reported
  rather than trimmed.
