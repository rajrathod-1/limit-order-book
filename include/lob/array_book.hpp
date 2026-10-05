// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "lob/bitset_index.hpp"
#include "lob/intrusive_list.hpp"
#include "lob/object_pool.hpp"
#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// One price level: the FIFO queue of orders resting at a single price, plus the
// aggregates that market data and the matcher want without walking the queue.
// ---------------------------------------------------------------------------
struct PriceLevel {
    using Queue = IntrusiveList<Order, OrderLinks, OrderPool>;

    Queue         queue;
    std::uint64_t total_qty   = 0;  // sum of resting qty, maintained incrementally
    std::uint32_t order_count = 0;

    [[nodiscard]] bool empty() const noexcept { return order_count == 0; }
};

static_assert(sizeof(PriceLevel) <= 24, "PriceLevel should stay compact: it is a large array");

// ---------------------------------------------------------------------------
// One side of the book.
//
// The structure is a dense flat array covering a configured price band, plus an
// ordered map holding whatever falls outside it.
//
// Why both. Real order flow is overwhelmingly concentrated: replaying Apple on
// 2019-12-30, 99.8% of resting orders priced within a few percent of the touch,
// on an exact penny grid. Those go in the array, where finding a price level is
// address arithmetic -- no comparisons, no tree descent, no pointer chasing --
// and where the top of book is maintained in O(1) by a hierarchical bitmap.
//
// The remaining 0.2% are real orders that must not be dropped: resting bids at
// $0.0001 and offers at $199,999, some of them off the penny grid entirely.
// Sizing the array to reach them would need two billion slots, so they live in
// a std::map keyed by raw price. It is almost always empty, and the fast path
// pays one predictable `empty()` test for it.
//
// This is the split a production book actually makes. The array is the book you
// trade against; the far map is the book you must still account for.
// ---------------------------------------------------------------------------
template <Side S>
class BookSide {
    // Ordered so that begin() is always the most aggressive price on this side.
    using FarCmp = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
    using FarMap = std::map<Price, PriceLevel, FarCmp>;

public:
    static constexpr Side kSide = S;

    // No order can rest at this price; it means "this side is empty".
    static constexpr Price kNoPrice = (S == Side::Buy)
        ? std::numeric_limits<Price>::min()
        : std::numeric_limits<Price>::max();

    BookSide(OrderPool& pool, TickScale scale, std::size_t capacity)
        : pool_(&pool), scale_(scale), levels_(capacity), occupied_(capacity) {
        if (capacity == 0) throw std::invalid_argument("BookSide capacity must be positive");
    }

    // ---- price band --------------------------------------------------------

    [[nodiscard]] bool in_band(Ticks t) const noexcept {
        return t >= 0 && static_cast<std::size_t>(t) < levels_.size();
    }

    // Where does this price live: the dense array, or the far map? A price must
    // be both on the tick grid and inside the band to be indexable.
    [[nodiscard]] bool is_indexable(Price p, Ticks& tick) const noexcept {
        return scale_.to_ticks(p, tick) && in_band(tick);
    }

    // ---- top of book -------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept {
        return array_best_ == kNoTick && far_.empty();
    }

    // The most aggressive price resting on this side, merging both structures.
    // When the far map is empty -- which is the overwhelmingly common case --
    // this is a load, a compare and a multiply-add.
    [[nodiscard]] Price best_price() const noexcept {
        const bool has_array = array_best_ != kNoTick;
        if (far_.empty()) return has_array ? scale_.to_price(array_best_) : kNoPrice;
        const Price far_px = far_.begin()->first;
        if (!has_array) return far_px;
        const Price arr_px = scale_.to_price(array_best_);
        return better(arr_px, far_px) ? arr_px : far_px;
    }

    [[nodiscard]] std::uint64_t best_qty() const noexcept {
        const PriceLevel* l = level_at(best_price());
        return l ? l->total_qty : 0;
    }

    [[nodiscard]] Handle best_front() const noexcept {
        const PriceLevel* l = level_at(best_price());
        return l ? l->queue.head() : kNullHandle;
    }

    // Best tick within the dense array only. Exposed for benchmarks and tests
    // that want to exercise the array path specifically.
    [[nodiscard]] Ticks best_tick() const noexcept { return array_best_; }

    // Is price `a` more aggressive than `b` on this side?
    [[nodiscard]] static constexpr bool better(Price a, Price b) noexcept {
        if constexpr (S == Side::Buy) return a > b;
        else                          return a < b;
    }
    [[nodiscard]] static constexpr bool better_or_equal(Price a, Price b) noexcept {
        return a == b || better(a, b);
    }

    // ---- mutation ----------------------------------------------------------

    // Rest an order. The caller has already populated the pool slot, including
    // `loc` and the kFar flag, via prepare().
    void insert(Handle h) noexcept {
        Order& o = (*pool_)[h];
        PriceLevel& lvl = o.is_far() ? far_[o.loc] : array_level(o.loc);

        if (!o.is_far() && lvl.order_count == 0) occupied_.set(static_cast<std::size_t>(o.loc));

        lvl.queue.push_back(*pool_, h);
        lvl.total_qty += o.qty;
        ++lvl.order_count;

        total_qty_ += o.qty;
        ++total_orders_;

        if (o.is_far()) {
            ++far_orders_;
            ++far_inserts_;
        } else if (array_best_ == kNoTick || tick_better(o.loc, array_best_)) {
            array_best_ = o.loc;
        }
    }

    // Fill in an order's location fields for a given price. Returns false only
    // if the price cannot be represented at all, which for a 64-bit price means
    // it exceeds what the far map's 32-bit `loc` field can hold.
    [[nodiscard]] bool prepare(Order& o, Price price) const noexcept {
        Ticks tick;
        if (is_indexable(price, tick)) {
            o.loc   = tick;
            o.flags = 0;
            return true;
        }
        if (price < std::numeric_limits<std::int32_t>::min() ||
            price > std::numeric_limits<std::int32_t>::max())
            return false;
        o.loc   = static_cast<std::int32_t>(price);
        o.flags = Order::kFar;
        return true;
    }

    [[nodiscard]] Price price_of(const Order& o) const noexcept {
        return o.is_far() ? static_cast<Price>(o.loc) : scale_.to_price(o.loc);
    }

    // Remove a resting order named by handle. O(1) in the array; O(log far) in
    // the far map, which holds a handful of orders.
    void remove(Handle h) noexcept {
        Order& o = (*pool_)[h];
        total_qty_ -= o.qty;
        --total_orders_;

        if (o.is_far()) {
            const auto it = far_.find(o.loc);
            assert(it != far_.end());
            PriceLevel& lvl = it->second;
            lvl.queue.unlink(*pool_, h);
            lvl.total_qty -= o.qty;
            --lvl.order_count;
            --far_orders_;
            if (lvl.order_count == 0) far_.erase(it);
            return;
        }

        const auto idx = static_cast<std::size_t>(o.loc);
        PriceLevel& lvl = levels_[idx];
        lvl.queue.unlink(*pool_, h);
        lvl.total_qty -= o.qty;
        --lvl.order_count;

        if (lvl.order_count == 0) {
            occupied_.clear_bit(idx);
            if (o.loc == array_best_) array_best_ = array_next(o.loc);
        }
    }

    // Shrink a resting order without changing its queue position. This is both
    // a partial fill and an ITCH order-cancel (which reduces, not deletes).
    void reduce(Handle h, Qty by) noexcept {
        Order& o = (*pool_)[h];
        assert(by <= o.qty);
        o.qty -= by;
        PriceLevel& lvl = o.is_far() ? far_.find(o.loc)->second
                                     : levels_[static_cast<std::size_t>(o.loc)];
        lvl.total_qty -= by;
        total_qty_ -= by;
    }

    // Resets only the levels that are actually populated, found through the
    // occupancy bitmap. Touching all `capacity` levels would make clearing an
    // almost-empty book as expensive as clearing a full one, which matters
    // because the band is deliberately sized far wider than the live book.
    void clear() noexcept {
        for (std::size_t i = occupied_.find_first_at_or_after(0);
             i != BitsetIndex::npos;
             i = (i + 1 < occupied_.capacity()) ? occupied_.find_first_at_or_after(i + 1)
                                                : BitsetIndex::npos) {
            PriceLevel& l = levels_[i];
            l.queue.clear();
            l.total_qty   = 0;
            l.order_count = 0;
        }
        occupied_.clear();
        far_.clear();
        array_best_   = kNoTick;
        total_qty_    = 0;
        total_orders_ = 0;
        far_orders_   = 0;
        far_inserts_  = 0;
    }

    // ---- inspection --------------------------------------------------------

    [[nodiscard]] const PriceLevel* level_at(Price p) const noexcept {
        if (p == kNoPrice) return nullptr;
        Ticks t;
        if (is_indexable(p, t)) {
            const PriceLevel& l = levels_[static_cast<std::size_t>(t)];
            return l.order_count ? &l : nullptr;
        }
        const auto it = far_.find(p);
        return it == far_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::uint64_t qty_at(Price p) const noexcept {
        const PriceLevel* l = level_at(p);
        return l ? l->total_qty : 0;
    }

    // The next populated price, walking away from the touch. Merges the array
    // and the far map so a depth walk sees one correctly ordered book.
    [[nodiscard]] Price next_price(Price p) const noexcept {
        // Best array price strictly less aggressive than p.
        Price arr = kNoPrice;
        if (array_best_ != kNoTick) {
            const Ticks t = array_next_from_price(p);
            if (t != kNoTick) arr = scale_.to_price(t);
        }
        // Best far price strictly less aggressive than p.
        Price fx = kNoPrice;
        if (!far_.empty()) {
            const auto it = far_.upper_bound(p);
            if (it != far_.end()) fx = it->first;
        }
        if (arr == kNoPrice) return fx;
        if (fx  == kNoPrice) return arr;
        return better(arr, fx) ? arr : fx;
    }

    // Visit up to `depth` populated levels from the touch outward.
    template <class Fn>
    void top_levels(std::size_t depth, Fn&& fn) const {
        Price p = best_price();
        for (std::size_t i = 0; i < depth && p != kNoPrice; ++i) {
            const PriceLevel* l = level_at(p);
            if (!l) break;
            fn(p, *l);
            p = next_price(p);
        }
    }

    [[nodiscard]] std::uint64_t total_qty()    const noexcept { return total_qty_; }
    [[nodiscard]] std::size_t   total_orders() const noexcept { return total_orders_; }
    [[nodiscard]] std::size_t   far_orders()   const noexcept { return far_orders_; }
    // Cumulative count of orders that ever went to the far map, so a replay can
    // report how often the slow path was actually needed.
    [[nodiscard]] std::size_t   far_inserts()  const noexcept { return far_inserts_; }
    [[nodiscard]] std::size_t   far_levels()   const noexcept { return far_.size(); }
    [[nodiscard]] TickScale     scale()        const noexcept { return scale_; }
    [[nodiscard]] std::size_t   capacity()     const noexcept { return levels_.size(); }

private:
    [[nodiscard]] PriceLevel& array_level(std::int32_t tick) noexcept {
        return levels_[static_cast<std::size_t>(tick)];
    }

    [[nodiscard]] static constexpr bool tick_better(Ticks a, Ticks b) noexcept {
        if constexpr (S == Side::Buy) return a > b;
        else                          return a < b;
    }

    // Next populated array tick strictly less aggressive than `t`, via the
    // hierarchical bitmap: a few count-leading-zeros, not a scan.
    [[nodiscard]] Ticks array_next(Ticks t) const noexcept {
        std::size_t r;
        if constexpr (S == Side::Buy) {
            if (t <= 0) return kNoTick;
            r = occupied_.find_last_at_or_before(static_cast<std::size_t>(t) - 1);
        } else {
            if (static_cast<std::size_t>(t) + 1 >= levels_.size()) return kNoTick;
            r = occupied_.find_first_at_or_after(static_cast<std::size_t>(t) + 1);
        }
        return r == BitsetIndex::npos ? kNoTick : static_cast<Ticks>(r);
    }

    // Same, but starting from an arbitrary price that may sit outside the band
    // entirely -- which happens when the walk is currently on a far level.
    [[nodiscard]] Ticks array_next_from_price(Price p) const noexcept {
        Ticks t = scale_.to_ticks_passive(p, S);
        if constexpr (S == Side::Buy) {
            // Want the highest occupied tick strictly below p.
            if (scale_.to_price(t) >= p) --t;
            if (t < 0) return kNoTick;
            if (static_cast<std::size_t>(t) >= levels_.size()) t = static_cast<Ticks>(levels_.size() - 1);
            const auto r = occupied_.find_last_at_or_before(static_cast<std::size_t>(t));
            return r == BitsetIndex::npos ? kNoTick : static_cast<Ticks>(r);
        } else {
            // Want the lowest occupied tick strictly above p.
            if (scale_.to_price(t) <= p) ++t;
            if (t < 0) t = 0;
            if (static_cast<std::size_t>(t) >= levels_.size()) return kNoTick;
            const auto r = occupied_.find_first_at_or_after(static_cast<std::size_t>(t));
            return r == BitsetIndex::npos ? kNoTick : static_cast<Ticks>(r);
        }
    }

    OrderPool*              pool_;
    TickScale               scale_;
    std::vector<PriceLevel> levels_;
    BitsetIndex             occupied_;
    FarMap                  far_;
    Ticks                   array_best_   = kNoTick;
    std::uint64_t           total_qty_    = 0;
    std::size_t             total_orders_ = 0;
    std::size_t             far_orders_   = 0;
    std::size_t             far_inserts_  = 0;
};

}  // namespace lob
