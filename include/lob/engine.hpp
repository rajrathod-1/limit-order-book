// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

#include "lob/array_book.hpp"
#include "lob/config.hpp"
#include "lob/events.hpp"
#include "lob/order_index.hpp"
#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Price-time priority matching engine.
//
// Priority rule, in full: an incoming order trades against the opposite side
// best price first; within a price, in arrival order. The passive order's price
// is the trade price, so an aggressive order can be price improved but never
// pays worse than its limit.
//
// The engine reasons in prices, not ticks. Ticks are an indexing detail owned by
// BookSide -- an order priced off the grid or outside the band still rests, it
// just rests somewhere slower. Keeping the matching logic in price terms is what
// makes that possible without a special case in the matcher.
//
// The hot path allocates nothing. The order pool is sized up front, and an order
// that fully fills on arrival never touches the pool at all, because a handle is
// only acquired for a remainder that actually needs to rest.
// ---------------------------------------------------------------------------
template <class Sink = NullSink>
class MatchingEngine {
public:
    explicit MatchingEngine(EngineConfig cfg = {}, Sink sink = {})
        : cfg_(cfg),
          sink_(std::move(sink)),
          pool_(cfg.max_orders),
          bids_(pool_, cfg.scale, cfg.tick_capacity),
          asks_(pool_, cfg.scale, cfg.tick_capacity),
          index_(cfg.max_orders) {}

    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // ---- order entry -------------------------------------------------------

    SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty,
                              Tif tif = Tif::Day) noexcept {
        SubmitResult r;
        r.id = id;

        if (qty == 0)            return reject(r, Status::InvalidQuantity);
        if (!representable(price)) return reject(r, Status::PriceOutOfRange);

        // A resting order needs a unique reference. Check before we trade, so a
        // duplicate never half-executes.
        if (index_.find(id) != kNullHandle) return reject(r, Status::DuplicateOrderId);

        if (tif == Tif::Fok && !can_fill_fully(side, price, qty, true))
            return reject(r, Status::FillOrKillUnfilled);

        Qty remaining = qty;
        if (side == Side::Buy) remaining = cross(asks_, r, id, Side::Buy,  price, remaining, true);
        else                   remaining = cross(bids_, r, id, Side::Sell, price, remaining, true);

        if (remaining == 0 || tif != Tif::Day) {
            if (remaining > 0) sink_.on_cancelled(id, side, price, remaining);
            return r;  // fully filled, or IOC/FOK remainder cancelled
        }
        return rest(r, id, side, price, remaining, tif);
    }

    // A market order takes whatever the book offers and never rests. It also
    // needs no price validation, which is why it has its own entry point rather
    // than being a limit at an extreme price.
    SubmitResult submit_market(OrderId id, Side side, Qty qty) noexcept {
        SubmitResult r;
        r.id = id;

        if (qty == 0) return reject(r, Status::InvalidQuantity);

        const bool has_liquidity = (side == Side::Buy) ? !asks_.empty() : !bids_.empty();
        if (!has_liquidity) return reject(r, Status::NoLiquidity);

        Qty remaining = qty;
        if (side == Side::Buy) remaining = cross(asks_, r, id, Side::Buy,  0, remaining, false);
        else                   remaining = cross(bids_, r, id, Side::Sell, 0, remaining, false);

        if (remaining > 0) sink_.on_cancelled(id, side, 0, remaining);
        return r;
    }

    // ---- amendment ---------------------------------------------------------

    Status cancel(OrderId id) noexcept {
        const Handle h = index_.find(id);
        if (h == kNullHandle) return fail(id, Status::UnknownOrderId);

        const Order& o = pool_[h];
        const Side  side  = o.side;
        const Price price = price_of(o);
        const Qty   qty   = o.qty;

        remove_resting(h);
        sink_.on_cancelled(id, side, price, qty);
        return Status::Ok;
    }

    // Reduce a resting order's size while keeping its place in the queue. This
    // is a genuine exchange primitive, and it is what an ITCH Order Cancel (X)
    // message means: a partial reduction, not a delete.
    Status reduce(OrderId id, Qty cancelled_qty) noexcept {
        const Handle h = index_.find(id);
        if (h == kNullHandle) return fail(id, Status::UnknownOrderId);

        Order& o = pool_[h];
        if (cancelled_qty == 0 || cancelled_qty > o.qty)
            return fail(id, Status::InvalidQuantity);

        const Side  side  = o.side;
        const Price price = price_of(o);

        if (cancelled_qty == o.qty) {
            remove_resting(h);
        } else if (side == Side::Buy) {
            bids_.reduce(h, cancelled_qty);
        } else {
            asks_.reduce(h, cancelled_qty);
        }
        sink_.on_cancelled(id, side, price, cancelled_qty);
        return Status::Ok;
    }

    // Cancel/replace. Replacing loses time priority unconditionally: the order
    // is removed and the new one arrives at the back of its queue, and it is
    // re-matched on the way in because the new price may now be marketable.
    // That is Nasdaq's rule and the only one that is fair to the orders that
    // queued behind the original.
    SubmitResult replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty) noexcept {
        SubmitResult r;
        r.id = new_id;

        const Handle h = index_.find(old_id);
        if (h == kNullHandle) return reject(r, Status::UnknownOrderId);
        if (new_qty == 0)     return reject(r, Status::InvalidQuantity);
        if (!representable(new_price)) return reject(r, Status::PriceOutOfRange);

        // A different reference number must not already be live.
        if (new_id != old_id && index_.find(new_id) != kNullHandle)
            return reject(r, Status::DuplicateOrderId);

        const Side side = pool_[h].side;

        remove_resting(h);
        sink_.on_replaced(old_id, new_id, side, new_price, new_qty);

        Qty remaining = new_qty;
        if (side == Side::Buy) remaining = cross(asks_, r, new_id, Side::Buy,  new_price, remaining, true);
        else                   remaining = cross(bids_, r, new_id, Side::Sell, new_price, remaining, true);

        if (remaining == 0) return r;
        return rest(r, new_id, side, new_price, remaining, Tif::Day);
    }

    // ---- book reconstruction ----------------------------------------------
    //
    // A market-by-order feed such as ITCH describes a book the exchange has
    // *already* matched, so every add on the wire is passive by construction.
    // Replaying those adds through submit_limit() would re-match them and
    // diverge from the exchange's book, so reconstruction gets its own
    // primitives. They are the same operations the matcher uses internally --
    // the same insert, the same O(1) removal -- just without the crossing step.
    // These are also what a real gateway uses to rebuild state after a restart.

    Status insert_passive(OrderId id, Side side, Price price, Qty qty) noexcept {
        if (qty == 0)              return fail(id, Status::InvalidQuantity);
        if (!representable(price)) return fail(id, Status::PriceOutOfRange);
        if (index_.find(id) != kNullHandle) return fail(id, Status::DuplicateOrderId);

        SubmitResult scratch;
        rest(scratch, id, side, price, qty, Tif::Day);
        return scratch.status;
    }

    // The named resting order traded `qty`. This is the per-fill half of
    // matching -- locate the order, decrement the level aggregate, and unlink
    // it if it is now exhausted -- which is exactly the work an aggressive
    // order does against each order it consumes.
    Status execute(OrderId id, Qty qty) noexcept {
        const Handle h = index_.find(id);
        if (h == kNullHandle) return fail(id, Status::UnknownOrderId);

        Order& o = pool_[h];
        if (qty == 0 || qty > o.qty) return fail(id, Status::InvalidQuantity);

        const Side  side = o.side;
        const Price px   = price_of(o);
        const bool  done = (qty == o.qty);

        if (done) {
            remove_resting(h);
        } else if (side == Side::Buy) {
            bids_.reduce(h, qty);
        } else {
            asks_.reduce(h, qty);
        }
        sink_.on_fill(Fill{0, id, px, qty, opposite(side), done});
        return Status::Ok;
    }

    // Replace without re-matching, for reconstruction. Priority is still lost:
    // the order leaves the queue and rejoins at the back.
    Status replace_passive(OrderId old_id, OrderId new_id, Price price, Qty qty) noexcept {
        const Handle h = index_.find(old_id);
        if (h == kNullHandle)      return fail(old_id, Status::UnknownOrderId);
        if (qty == 0)              return fail(old_id, Status::InvalidQuantity);
        if (!representable(price)) return fail(old_id, Status::PriceOutOfRange);

        const Side side = pool_[h].side;
        remove_resting(h);
        if (new_id != old_id && index_.find(new_id) != kNullHandle)
            return fail(new_id, Status::DuplicateOrderId);

        SubmitResult scratch;
        rest(scratch, new_id, side, price, qty, Tif::Day);
        sink_.on_replaced(old_id, new_id, side, price, qty);
        return scratch.status;
    }

    // ---- market data -------------------------------------------------------

    [[nodiscard]] const BookSide<Side::Buy>&  bids() const noexcept { return bids_; }
    [[nodiscard]] const BookSide<Side::Sell>& asks() const noexcept { return asks_; }
    [[nodiscard]] const OrderPool&            pool() const noexcept { return pool_; }
    [[nodiscard]] const OrderIndex&           index() const noexcept { return index_; }
    [[nodiscard]] Sink&                       sink() noexcept { return sink_; }
    [[nodiscard]] EngineConfig                config() const noexcept { return cfg_; }

    // Zero when the side is empty, so callers can print without a special case.
    [[nodiscard]] Price best_bid() const noexcept {
        return bids_.empty() ? 0 : bids_.best_price();
    }
    [[nodiscard]] Price best_ask() const noexcept {
        return asks_.empty() ? 0 : asks_.best_price();
    }

    // Spread in Price units, or -1 when either side is empty.
    [[nodiscard]] Price spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return -1;
        return asks_.best_price() - bids_.best_price();
    }

    [[nodiscard]] bool crossed() const noexcept {
        return !bids_.empty() && !asks_.empty() && bids_.best_price() >= asks_.best_price();
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t far_orders() const noexcept {
        return bids_.far_orders() + asks_.far_orders();
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        index_.clear();
        pool_.reset();
        seq_ = 0;
    }

    // Fault in the pool so a latency run is not measuring first-touch faults.
    void prefault() noexcept { pool_.prefault(); }

private:
    // ---- matching ----------------------------------------------------------

    // Sweep `book` (the opposite side) while it is marketable against `limit`.
    // Returns the unfilled remainder. When `limited` is false the price test is
    // skipped entirely, which is the market-order case.
    template <class OppSide>
    Qty cross(OppSide& book, SubmitResult& r, OrderId aggressor, Side aggressor_side,
              Price limit, Qty remaining, bool limited) noexcept {
        while (remaining > 0) {
            if (book.empty()) break;
            const Price px = book.best_price();
            // Marketable if the aggressor's limit reaches the passive price.
            if (limited && !marketable(aggressor_side, limit, px)) break;

            const Handle h = book.best_front();
            if (h == kNullHandle) break;  // level accounting would be corrupt

            Order&    resting  = pool_[h];
            const Qty traded   = std::min(remaining, resting.qty);
            const bool consumed = (traded == resting.qty);
            const OrderId rid  = resting.id;

            remaining      -= traded;
            r.filled_qty   += traded;
            r.notional     += static_cast<std::uint64_t>(px) * traded;
            ++r.fill_count;

            if (consumed) remove_resting(h);
            else          book.reduce(h, traded);

            sink_.on_fill(Fill{aggressor, rid, px, traded, aggressor_side, consumed});
        }
        return remaining;
    }

    [[nodiscard]] static constexpr bool marketable(Side aggressor, Price limit, Price passive) noexcept {
        return aggressor == Side::Buy ? limit >= passive : limit <= passive;
    }

    // Can `qty` be filled outright at or better than `limit`? Walks the opposite
    // side without mutating it. Only fill-or-kill needs this, and FOK is rare,
    // so the walk is not on the common path.
    [[nodiscard]] bool can_fill_fully(Side side, Price limit, Qty qty, bool limited) const noexcept {
        std::uint64_t available = 0;
        if (side == Side::Buy) {
            for (Price p = asks_.best_price(); p != BookSide<Side::Sell>::kNoPrice;
                 p = asks_.next_price(p)) {
                if (limited && !marketable(Side::Buy, limit, p)) break;
                available += asks_.qty_at(p);
                if (available >= qty) return true;
            }
        } else {
            for (Price p = bids_.best_price(); p != BookSide<Side::Buy>::kNoPrice;
                 p = bids_.next_price(p)) {
                if (limited && !marketable(Side::Sell, limit, p)) break;
                available += bids_.qty_at(p);
                if (available >= qty) return true;
            }
        }
        return available >= qty;
    }

    // A price must fit the 32-bit location field an Order carries. Nasdaq's
    // maximum wire price is $199,999.9900, well inside that.
    [[nodiscard]] static constexpr bool representable(Price p) noexcept {
        return p >= 0 && p <= std::numeric_limits<std::int32_t>::max();
    }

    [[nodiscard]] Price price_of(const Order& o) const noexcept {
        return o.side == Side::Buy ? bids_.price_of(o) : asks_.price_of(o);
    }

    // ---- resting -----------------------------------------------------------

    SubmitResult& rest(SubmitResult& r, OrderId id, Side side, Price price,
                       Qty qty, Tif tif) noexcept {
        const Handle h = pool_.acquire();
        if (h == kNullHandle) {
            // Anything already traded stands; only the remainder is rejected.
            r.status = Status::PoolExhausted;
            sink_.on_rejected(id, Status::PoolExhausted);
            return r;
        }
        if (!index_.insert(id, h)) {
            pool_.release(h);
            r.status = Status::IndexFull;
            sink_.on_rejected(id, Status::IndexFull);
            return r;
        }

        Order& o = pool_[h];
        o.next = o.prev = kNullHandle;
        o.qty  = qty;
        o.id   = id;
        o.seq  = seq_++;
        o.side = side;
        o.tif  = tif;
        o._pad = 0;

        const bool ok = (side == Side::Buy) ? bids_.prepare(o, price) : asks_.prepare(o, price);
        if (!ok) {
            index_.erase(id);
            pool_.release(h);
            r.status = Status::PriceOutOfRange;
            sink_.on_rejected(id, Status::PriceOutOfRange);
            return r;
        }

        if (side == Side::Buy) bids_.insert(h);
        else                   asks_.insert(h);

        r.resting_qty = qty;
        sink_.on_accepted(id, side, price, qty);
        return r;
    }

    // Unlink from its side, drop the index entry, return the slot to the pool.
    void remove_resting(Handle h) noexcept {
        const Order& o = pool_[h];
        const OrderId id = o.id;
        if (o.side == Side::Buy) bids_.remove(h);
        else                     asks_.remove(h);
        index_.erase(id);
        pool_.release(h);
    }

    SubmitResult& reject(SubmitResult& r, Status s) noexcept {
        r.status = s;
        sink_.on_rejected(r.id, s);
        return r;
    }

    Status fail(OrderId id, Status s) noexcept {
        sink_.on_rejected(id, s);
        return s;
    }

    EngineConfig         cfg_;
    Sink                 sink_;
    OrderPool            pool_;
    BookSide<Side::Buy>  bids_;
    BookSide<Side::Sell> asks_;
    OrderIndex           index_;
    Seq                  seq_ = 0;
};

}  // namespace lob
