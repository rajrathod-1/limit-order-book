// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <utility>

#include "lob/config.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// The baseline: the order book as it is usually written first.
//
// std::map keyed by price, std::list of orders per level, std::unordered_map
// from order id to a list iterator so cancels can find their order. This is the
// textbook design, and it is a fair opponent rather than a straw man -- it uses
// the right containers for the job, keeps the same O(1)-cancel trick via stored
// iterators, and maintains the same aggregates.
//
// What it cannot avoid is structural: every price lookup is a red-black tree
// descent with a comparison and a pointer chase per level; every resting order
// is an individual heap allocation with its own cache line; and erasing the top
// level rebalances the tree. Those are the three costs ArrayBook is designed to
// remove, and they are what the benchmark measures.
//
// The interface deliberately mirrors MatchingEngine so both can be driven by
// the same benchmark and the same conformance tests.
// ---------------------------------------------------------------------------
template <class Sink = NullSink>
class MapMatchingEngine {
public:
    explicit MapMatchingEngine(EngineConfig cfg = {}, Sink sink = {})
        : cfg_(cfg), sink_(std::move(sink)) {}

    MapMatchingEngine(const MapMatchingEngine&)            = delete;
    MapMatchingEngine& operator=(const MapMatchingEngine&) = delete;

    SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty,
                              Tif tif = Tif::Day) {
        SubmitResult r;
        r.id = id;
        if (qty == 0) return reject(r, Status::InvalidQuantity);

        if (!representable(price)) return reject(r, Status::PriceOutOfRange);
        if (locators_.count(id)) return reject(r, Status::DuplicateOrderId);

        if (tif == Tif::Fok && !can_fill_fully(side, price, qty))
            return reject(r, Status::FillOrKillUnfilled);

        Qty remaining = (side == Side::Buy) ? cross_asks(r, id, price, qty, true)
                                            : cross_bids(r, id, price, qty, true);

        if (remaining == 0 || tif != Tif::Day) {
            if (remaining > 0) sink_.on_cancelled(id, side, price, remaining);
            return r;
        }
        rest(id, side, price, remaining);
        r.resting_qty = remaining;
        sink_.on_accepted(id, side, price, remaining);
        return r;
    }

    SubmitResult submit_market(OrderId id, Side side, Qty qty) {
        SubmitResult r;
        r.id = id;
        if (qty == 0) return reject(r, Status::InvalidQuantity);
        const bool liquidity = (side == Side::Buy) ? !asks_.empty() : !bids_.empty();
        if (!liquidity) return reject(r, Status::NoLiquidity);

        Qty remaining = (side == Side::Buy) ? cross_asks(r, id, 0, qty, false)
                                            : cross_bids(r, id, 0, qty, false);
        if (remaining > 0) sink_.on_cancelled(id, side, 0, remaining);
        return r;
    }

    Status cancel(OrderId id) {
        auto it = locators_.find(id);
        if (it == locators_.end()) { sink_.on_rejected(id, Status::UnknownOrderId); return Status::UnknownOrderId; }
        const Loc loc = it->second;
        const Qty qty = loc.it->qty;
        erase(loc);
        locators_.erase(it);
        sink_.on_cancelled(id, loc.side, loc.price, qty);
        return Status::Ok;
    }

    Status reduce(OrderId id, Qty cancelled_qty) {
        auto it = locators_.find(id);
        if (it == locators_.end()) { sink_.on_rejected(id, Status::UnknownOrderId); return Status::UnknownOrderId; }
        Loc loc = it->second;
        if (cancelled_qty == 0 || cancelled_qty > loc.it->qty) {
            sink_.on_rejected(id, Status::InvalidQuantity);
            return Status::InvalidQuantity;
        }
        if (cancelled_qty == loc.it->qty) {
            erase(loc);
            locators_.erase(it);
        } else {
            loc.it->qty -= cancelled_qty;
            level_qty(loc.side, loc.price) -= cancelled_qty;
        }
        sink_.on_cancelled(id, loc.side, loc.price, cancelled_qty);
        return Status::Ok;
    }

    SubmitResult replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty) {
        SubmitResult r;
        r.id = new_id;
        auto it = locators_.find(old_id);
        if (it == locators_.end()) return reject(r, Status::UnknownOrderId);
        if (new_qty == 0) return reject(r, Status::InvalidQuantity);
        if (new_id != old_id && locators_.count(new_id)) return reject(r, Status::DuplicateOrderId);

        if (!representable(new_price)) return reject(r, Status::PriceOutOfRange);

        const Loc  loc  = it->second;
        const Side side = loc.side;
        erase(loc);
        locators_.erase(it);
        sink_.on_replaced(old_id, new_id, side, new_price, new_qty);

        Qty remaining = (side == Side::Buy) ? cross_asks(r, new_id, new_price, new_qty, true)
                                            : cross_bids(r, new_id, new_price, new_qty, true);
        if (remaining == 0) return r;
        rest(new_id, side, new_price, remaining);
        r.resting_qty = remaining;
        sink_.on_accepted(new_id, side, new_price, remaining);
        return r;
    }

    // ---- book reconstruction (mirrors MatchingEngine) ----------------------

    Status insert_passive(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0) { sink_.on_rejected(id, Status::InvalidQuantity); return Status::InvalidQuantity; }
        if (!representable(price)) { sink_.on_rejected(id, Status::PriceOutOfRange); return Status::PriceOutOfRange; }
        if (locators_.count(id)) { sink_.on_rejected(id, Status::DuplicateOrderId); return Status::DuplicateOrderId; }
        rest(id, side, price, qty);
        sink_.on_accepted(id, side, price, qty);
        return Status::Ok;
    }

    Status execute(OrderId id, Qty qty) {
        auto it = locators_.find(id);
        if (it == locators_.end()) { sink_.on_rejected(id, Status::UnknownOrderId); return Status::UnknownOrderId; }
        Loc loc = it->second;
        if (qty == 0 || qty > loc.it->qty) { sink_.on_rejected(id, Status::InvalidQuantity); return Status::InvalidQuantity; }
        const bool done = (qty == loc.it->qty);
        if (done) {
            erase(loc);
            locators_.erase(it);
        } else {
            loc.it->qty -= qty;
            level_qty(loc.side, loc.price) -= qty;
        }
        sink_.on_fill(Fill{0, id, loc.price, qty, opposite(loc.side), done});
        return Status::Ok;
    }

    Status replace_passive(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        auto it = locators_.find(old_id);
        if (it == locators_.end()) { sink_.on_rejected(old_id, Status::UnknownOrderId); return Status::UnknownOrderId; }
        if (qty == 0) { sink_.on_rejected(old_id, Status::InvalidQuantity); return Status::InvalidQuantity; }
        if (!representable(price)) { sink_.on_rejected(old_id, Status::PriceOutOfRange); return Status::PriceOutOfRange; }
        const Loc  loc  = it->second;
        const Side side = loc.side;
        erase(loc);
        locators_.erase(it);
        if (new_id != old_id && locators_.count(new_id)) { sink_.on_rejected(new_id, Status::DuplicateOrderId); return Status::DuplicateOrderId; }
        rest(new_id, side, price, qty);
        sink_.on_replaced(old_id, new_id, side, price, qty);
        return Status::Ok;
    }

    [[nodiscard]] Price best_bid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
    [[nodiscard]] Price best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }
    [[nodiscard]] std::uint64_t best_bid_qty() const { return bids_.empty() ? 0 : bids_.begin()->second.total_qty; }
    [[nodiscard]] std::uint64_t best_ask_qty() const { return asks_.empty() ? 0 : asks_.begin()->second.total_qty; }
    [[nodiscard]] std::size_t live_orders() const { return locators_.size(); }
    [[nodiscard]] std::size_t bid_levels()  const { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels()  const { return asks_.size(); }
    [[nodiscard]] Sink& sink() { return sink_; }

    // Full visible depth, price -> resting quantity, for cross-checking.
    [[nodiscard]] std::map<Price, std::uint64_t> depth(Side s) const {
        std::map<Price, std::uint64_t> d;
        if (s == Side::Buy) for (const auto& [px, l] : bids_) d[px] = l.total_qty;
        else                for (const auto& [px, l] : asks_) d[px] = l.total_qty;
        return d;
    }

    [[nodiscard]] std::uint64_t qty_at(Side s, Price p) const {
        if (s == Side::Buy) { auto i = bids_.find(p); return i == bids_.end() ? 0 : i->second.total_qty; }
        auto i = asks_.find(p); return i == asks_.end() ? 0 : i->second.total_qty;
    }

    void clear() { bids_.clear(); asks_.clear(); locators_.clear(); }

private:
    struct MapOrder { OrderId id; Qty qty; };
    struct Level {
        std::list<MapOrder> orders;
        std::uint64_t       total_qty = 0;
    };
    using Bids = std::map<Price, Level, std::greater<Price>>;  // highest first
    using Asks = std::map<Price, Level, std::less<Price>>;     // lowest first

    struct Loc {
        Side  side;
        Price price;
        std::list<MapOrder>::iterator it;
    };

    void rest(OrderId id, Side side, Price price, Qty qty) {
        if (side == Side::Buy) {
            Level& l = bids_[price];
            l.orders.push_back(MapOrder{id, qty});
            l.total_qty += qty;
            locators_.emplace(id, Loc{side, price, std::prev(l.orders.end())});
        } else {
            Level& l = asks_[price];
            l.orders.push_back(MapOrder{id, qty});
            l.total_qty += qty;
            locators_.emplace(id, Loc{side, price, std::prev(l.orders.end())});
        }
    }

    std::uint64_t& level_qty(Side s, Price p) {
        return s == Side::Buy ? bids_.find(p)->second.total_qty : asks_.find(p)->second.total_qty;
    }

    void erase(const Loc& loc) {
        if (loc.side == Side::Buy) {
            auto lit = bids_.find(loc.price);
            lit->second.total_qty -= loc.it->qty;
            lit->second.orders.erase(loc.it);
            if (lit->second.orders.empty()) bids_.erase(lit);
        } else {
            auto lit = asks_.find(loc.price);
            lit->second.total_qty -= loc.it->qty;
            lit->second.orders.erase(loc.it);
            if (lit->second.orders.empty()) asks_.erase(lit);
        }
    }

    Qty cross_asks(SubmitResult& r, OrderId aggressor, Price limit, Qty remaining, bool limited) {
        while (remaining > 0 && !asks_.empty()) {
            auto lit = asks_.begin();
            if (limited && lit->first > limit) break;
            Level& lvl = lit->second;
            while (remaining > 0 && !lvl.orders.empty()) {
                MapOrder& o = lvl.orders.front();
                const Qty traded = std::min(remaining, o.qty);
                remaining    -= traded;
                o.qty        -= traded;
                lvl.total_qty -= traded;
                r.filled_qty += traded;
                r.notional   += static_cast<std::uint64_t>(lit->first) * traded;
                ++r.fill_count;
                const bool consumed = (o.qty == 0);
                const OrderId rid = o.id;
                if (consumed) { locators_.erase(rid); lvl.orders.pop_front(); }
                sink_.on_fill(Fill{aggressor, rid, lit->first, traded, Side::Buy, consumed});
            }
            if (lvl.orders.empty()) asks_.erase(lit);
        }
        return remaining;
    }

    Qty cross_bids(SubmitResult& r, OrderId aggressor, Price limit, Qty remaining, bool limited) {
        while (remaining > 0 && !bids_.empty()) {
            auto lit = bids_.begin();
            if (limited && lit->first < limit) break;
            Level& lvl = lit->second;
            while (remaining > 0 && !lvl.orders.empty()) {
                MapOrder& o = lvl.orders.front();
                const Qty traded = std::min(remaining, o.qty);
                remaining    -= traded;
                o.qty        -= traded;
                lvl.total_qty -= traded;
                r.filled_qty += traded;
                r.notional   += static_cast<std::uint64_t>(lit->first) * traded;
                ++r.fill_count;
                const bool consumed = (o.qty == 0);
                const OrderId rid = o.id;
                if (consumed) { locators_.erase(rid); lvl.orders.pop_front(); }
                sink_.on_fill(Fill{aggressor, rid, lit->first, traded, Side::Sell, consumed});
            }
            if (lvl.orders.empty()) bids_.erase(lit);
        }
        return remaining;
    }

    bool can_fill_fully(Side side, Price limit, Qty qty) const {
        std::uint64_t avail = 0;
        if (side == Side::Buy) {
            for (const auto& [px, lvl] : asks_) {
                if (px > limit) break;
                avail += lvl.total_qty;
                if (avail >= qty) return true;
            }
        } else {
            for (const auto& [px, lvl] : bids_) {
                if (px < limit) break;
                avail += lvl.total_qty;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    // Mirrors MatchingEngine: the array book stores a price in a 32-bit field,
    // so the baseline must accept exactly the same set of prices for the
    // comparison to be meaningful.
    [[nodiscard]] static constexpr bool representable(Price p) noexcept {
        return p >= 0 && p <= std::numeric_limits<std::int32_t>::max();
    }

    SubmitResult& reject(SubmitResult& r, Status s) {
        r.status = s;
        sink_.on_rejected(r.id, s);
        return r;
    }

    EngineConfig                        cfg_;
    Sink                                sink_;
    Bids                                bids_;
    Asks                                asks_;
    std::unordered_map<OrderId, Loc>    locators_;
};

}  // namespace lob
