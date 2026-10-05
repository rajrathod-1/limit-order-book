// SPDX-License-Identifier: MIT
//
// itch_gen -- generate a synthetic but structurally valid ITCH 5.0 stream.
//
// The point of this tool is that the project is runnable and benchmarkable with
// no 3.5 GB download and no Nasdaq dependency: `make && ctest && lob_replay`
// works on a clean checkout. The output goes through exactly the same decoder,
// framing and replay path as a real Nasdaq file.
//
// It is a synthetic *order flow*, not a market simulation, and it does not try
// to be one. What it does reproduce is the structure the book cares about: a
// mid that moves, depth that thins as you go away from the touch, a heavy
// add/cancel ratio, replaces that renumber orders, and executions that always
// hit the front of a price level. Every reference it emits is live, and every
// execution is within the named order's remaining size, so the stream is
// internally consistent and a correct book will never reject a message from it.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/itch50.hpp"

namespace {

// ---- big-endian encoders (mirror of the decoders in itch50.hpp) ------------

void put16(std::vector<std::byte>& b, std::uint16_t v) {
    v = itch::bswap(v);
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    b.insert(b.end(), p, p + 2);
}
void put32(std::vector<std::byte>& b, std::uint32_t v) {
    v = itch::bswap(v);
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    b.insert(b.end(), p, p + 4);
}
void put64(std::vector<std::byte>& b, std::uint64_t v) {
    v = itch::bswap(v);
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    b.insert(b.end(), p, p + 8);
}
void put48(std::vector<std::byte>& b, std::uint64_t v) {
    const std::uint64_t sw = itch::bswap(v);
    const auto* p = reinterpret_cast<const std::byte*>(&sw);
    b.insert(b.end(), p + 2, p + 8);
}
void put8(std::vector<std::byte>& b, char c) { b.push_back(static_cast<std::byte>(c)); }
void puttext(std::vector<std::byte>& b, std::string_view s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
        b.push_back(static_cast<std::byte>(i < s.size() ? s[i] : ' '));
}

class Emitter {
public:
    explicit Emitter(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) { std::perror("fopen"); std::exit(1); }
    }
    ~Emitter() { flush(); if (f_) std::fclose(f_); }

    // Frames a built message with its big-endian length prefix.
    void emit(const std::vector<std::byte>& msg) {
        put16(out_, static_cast<std::uint16_t>(msg.size()));
        out_.insert(out_.end(), msg.begin(), msg.end());
        ++count_;
        if (out_.size() >= (1u << 20)) flush();
    }
    void flush() {
        if (f_ && !out_.empty()) { std::fwrite(out_.data(), 1, out_.size(), f_); out_.clear(); }
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    std::FILE*             f_ = nullptr;
    std::vector<std::byte> out_;
    std::uint64_t          count_ = 0;
};

std::vector<std::byte> header(char type, std::uint16_t locate, std::uint64_t ts) {
    std::vector<std::byte> m;
    m.reserve(64);
    put8(m, type);
    put16(m, locate);
    put16(m, 0);
    put48(m, ts);
    return m;
}

struct Options {
    std::string   output   = "synthetic.itch";
    std::string   symbol   = "SYNTH";
    std::uint64_t messages = 2'000'000;
    std::uint32_t ref_price = 1'000'000;  // $100.0000
    std::uint64_t seed     = 20191230;
};

void usage(const char* a0) {
    std::fprintf(stderr,
        "usage: %s [--out FILE] [--symbol SYM] [--messages N] [--price CENTS] [--seed S]\n"
        "  --out       output path (default synthetic.itch)\n"
        "  --symbol    symbol to stamp (default SYNTH)\n"
        "  --messages  number of messages to emit (default 2000000)\n"
        "  --price     reference price in 1/10000 dollars (default 1000000 = $100)\n"
        "  --seed      RNG seed for a reproducible stream (default 20191230)\n", a0);
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
        if      (a == "--out")      opt.output = next();
        else if (a == "--symbol")   opt.symbol = next();
        else if (a == "--messages") opt.messages = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--price")    opt.ref_price = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--seed")     opt.seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    Emitter out(opt.output);
    std::mt19937_64 rng(opt.seed);
    constexpr std::uint16_t kLocate = 1;
    std::uint64_t ts = 34'200'000'000'000ull;  // 09:30:00.000 in ns after midnight

    // ---- session preamble --------------------------------------------------
    {
        auto m = header('S', 0, ts);
        put8(m, 'O');  // start of messages
        out.emit(m);
    }
    {
        auto m = header('R', kLocate, ts);
        puttext(m, opt.symbol, 8);
        put8(m, 'Q');            // market category: Nasdaq Global Select
        put8(m, 'N');            // financial status
        put32(m, 100);           // round lot size
        put8(m, 'N');            // round lots only
        put8(m, 'C');            // issue classification: common stock
        puttext(m, "", 2);       // issue sub-type
        put8(m, 'P');            // authenticity: production
        put8(m, 'N');            // short sale threshold
        put8(m, 'N');            // IPO flag
        put8(m, '1');            // LULD reference price tier
        put8(m, 'N');            // ETP flag
        put32(m, 0);             // ETP leverage
        put8(m, 'N');            // inverse indicator
        out.emit(m);
    }
    {
        auto m = header('S', 0, ts);
        put8(m, 'Q');  // start of regular market hours
        out.emit(m);
    }

    // ---- model book --------------------------------------------------------
    // Tracked only so the emitted stream stays self-consistent: every delete,
    // cancel, replace and execution names an order that is genuinely live and
    // asks for no more shares than it has left.
    struct Live { std::uint32_t price; std::uint32_t shares; char side; };
    std::unordered_map<std::uint64_t, Live> live;
    std::map<std::uint32_t, std::deque<std::uint64_t>> bid_q, ask_q;  // price -> FIFO of refs
    live.reserve(1u << 16);

    std::uint64_t next_ref = 1;
    std::int64_t  mid      = opt.ref_price;
    constexpr std::int64_t kTick = 100;  // one cent

    auto tick_of = [&](std::int64_t p) { return static_cast<std::uint32_t>(p); };

    auto add_order = [&](char side, std::uint32_t px, std::uint32_t shares) {
        const std::uint64_t ref = next_ref++;
        auto m = header('A', kLocate, ts);
        put64(m, ref);
        put8(m, side);
        put32(m, shares);
        puttext(m, opt.symbol, 8);
        put32(m, px);
        out.emit(m);
        live.emplace(ref, Live{px, shares, side});
        (side == 'B' ? bid_q : ask_q)[px].push_back(ref);
    };

    auto forget = [&](std::uint64_t ref, const Live& l) {
        auto& book = (l.side == 'B' ? bid_q : ask_q);
        auto it = book.find(l.price);
        if (it != book.end()) {
            auto& q = it->second;
            q.erase(std::remove(q.begin(), q.end(), ref), q.end());
            if (q.empty()) book.erase(it);
        }
        live.erase(ref);
    };

    // Pick a live order uniformly. Sampling from the hash map's buckets is
    // biased but cheap, and for generating a workload that is fine.
    auto pick_live = [&]() -> std::uint64_t {
        if (live.empty()) return 0;
        auto it = live.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng() % live.size()));
        return it->first;
    };

    // Real single-name books hold a roughly stable number of resting orders:
    // adds and cancels balance because market makers replenish what they pull.
    // A fixed add/cancel ratio would instead random-walk the depth to zero or
    // to infinity, so the add probability leans against the current depth to
    // hold it near a target.
    constexpr std::size_t kTargetDepth = 4000;

    std::uniform_int_distribution<int>          action(0, 999);
    std::uniform_int_distribution<int>          depth(0, 9);
    std::uniform_int_distribution<std::uint32_t> lots(1, 20);
    std::normal_distribution<double>            drift(0.0, 1.2);

    // Best bid and best ask of the model book. Needed to keep the generated
    // stream uncrossed: on a real feed the exchange has already matched
    // anything that would cross, so a bid above the offer never reaches the
    // wire. A generator that emits one produces a book no replay should have
    // to make sense of.
    auto best_bid = [&]() -> std::int64_t {
        return bid_q.empty() ? -1 : static_cast<std::int64_t>(bid_q.rbegin()->first);
    };
    auto best_ask = [&]() -> std::int64_t {
        return ask_q.empty() ? -1 : static_cast<std::int64_t>(ask_q.begin()->first);
    };

    // Seed an initial book so the first cancels and executions have something
    // to name.
    for (int d = 1; d <= 12; ++d) {
        add_order('B', tick_of(mid - d * kTick), lots(rng) * 100);
        add_order('S', tick_of(mid + d * kTick), lots(rng) * 100);
        ts += 1000;
    }

    // ---- main flow ---------------------------------------------------------
    // Mix chosen to resemble the real file measured on 2019-12-30, where adds
    // and deletes each account for ~43% of order traffic, replaces ~8%, and
    // executions ~2%.
    while (out.count() < opt.messages) {
        ts += 200 + (rng() % 4000);

        // Mean-reverting mid. A pure random walk over a million messages
        // wanders to the clamp and parks there, dragging the whole book with
        // it; real intraday prices oscillate around a level.
        mid += static_cast<std::int64_t>(drift(rng)) * kTick;
        mid -= (mid - static_cast<std::int64_t>(opt.ref_price)) / 512;
        mid = std::clamp<std::int64_t>(mid, opt.ref_price / 2, opt.ref_price * 3 / 2);
        // Snap back to the grid: the mean-reversion divide leaves a remainder,
        // and a mid drifting off the penny grid would quietly turn every price
        // in the file sub-penny.
        mid = (mid / kTick) * kTick;

        const int roll = action(rng);

        // Bias the add/delete split by how full the book is, leaving the
        // replace, partial-cancel and execute rates untouched. An earlier
        // version scaled the whole roll instead, which silently squeezed the
        // execute branch out of the distribution entirely.
        const double fill = static_cast<double>(live.size()) / static_cast<double>(kTargetDepth);
        const int add_share = std::clamp(
            static_cast<int>(430.0 + 260.0 * (1.0 - fill)), 140, 720);

        if (roll < add_share || live.size() < 64) {
            // Add, priced away from the mid with depth thinning outward, then
            // pulled back to the near side of the touch so it cannot cross.
            const bool buy = (rng() & 1) != 0;
            const int  d   = 1 + depth(rng);
            std::int64_t px = buy ? mid - d * kTick : mid + d * kTick;

            // If it would cross, drop it rather than repricing it. On a real
            // feed a crossing order never appears as an Add at all -- the
            // exchange matched it, and what reaches the wire is an execution.
            // Repricing it to the touch instead would pile implausible size
            // onto a single level.
            const bool crosses = buy ? (best_ask() > 0 && px >= best_ask())
                                     : (best_bid() > 0 && px <= best_bid());
            if (!crosses && px > kTick)
                add_order(buy ? 'B' : 'S', tick_of(px), lots(rng) * 100);

        } else if (roll < 860) {
            // Delete.
            const std::uint64_t ref = pick_live();
            if (!ref) continue;
            const Live l = live[ref];
            auto m = header('D', kLocate, ts);
            put64(m, ref);
            out.emit(m);
            forget(ref, l);

        } else if (roll < 940) {
            // Replace: renumber, reprice, resize. Loses time priority.
            const std::uint64_t ref = pick_live();
            if (!ref) continue;
            const Live l = live[ref];
            const std::uint64_t nref = next_ref++;
            const int d = 1 + depth(rng);
            std::int64_t px = (l.side == 'B') ? mid - d * kTick : mid + d * kTick;

            // The replacement must not cross either.
            const bool crosses = (l.side == 'B') ? (best_ask() > 0 && px >= best_ask())
                                                 : (best_bid() > 0 && px <= best_bid());
            if (crosses || px <= kTick) continue;
            const std::uint32_t shares = lots(rng) * 100;
            auto m = header('U', kLocate, ts);
            put64(m, ref);
            put64(m, nref);
            put32(m, shares);
            put32(m, tick_of(px));
            out.emit(m);
            forget(ref, l);
            live.emplace(nref, Live{tick_of(px), shares, l.side});
            (l.side == 'B' ? bid_q : ask_q)[tick_of(px)].push_back(nref);

        } else if (roll < 975) {
            // Partial cancel: reduce, keep queue position.
            const std::uint64_t ref = pick_live();
            if (!ref) continue;
            Live& l = live[ref];
            if (l.shares <= 100) continue;
            const std::uint32_t cut = 100 * (1 + (rng() % (l.shares / 100 - 1)));
            auto m = header('X', kLocate, ts);
            put64(m, ref);
            put32(m, cut);
            out.emit(m);
            l.shares -= cut;

        } else {
            // Execute against the front of the touch level, which is what an
            // aggressive order would actually hit.
            const bool hit_bid = (rng() & 1) != 0;
            auto& book = hit_bid ? bid_q : ask_q;
            if (book.empty()) continue;
            // Best bid is the highest price, best ask the lowest.
            auto lvl = hit_bid ? std::prev(book.end()) : book.begin();
            if (lvl->second.empty()) { book.erase(lvl); continue; }
            const std::uint64_t ref = lvl->second.front();
            auto lit = live.find(ref);
            if (lit == live.end()) { lvl->second.pop_front(); continue; }
            Live& l = lit->second;
            const std::uint32_t exec = (rng() % 3 == 0 && l.shares > 100)
                                     ? 100u * (1 + static_cast<std::uint32_t>(rng() % (l.shares / 100 - 1)))
                                     : l.shares;
            auto m = header('E', kLocate, ts);
            put64(m, ref);
            put32(m, exec);
            put64(m, next_ref++);  // match number
            out.emit(m);
            if (exec == l.shares) { const Live copy = l; forget(ref, copy); }
            else                  { l.shares -= exec; }
        }
    }

    {
        auto m = header('S', 0, ts);
        put8(m, 'M');  // end of regular market hours
        out.emit(m);
    }
    {
        auto m = header('S', 0, ts);
        put8(m, 'C');  // end of messages
        out.emit(m);
    }
    out.flush();

    std::fprintf(stderr, "wrote %llu messages to %s (symbol %s, %zu orders left live)\n",
                 (unsigned long long)out.count(), opt.output.c_str(), opt.symbol.c_str(), live.size());
    return 0;
}
