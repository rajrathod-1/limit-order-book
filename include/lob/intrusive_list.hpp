// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>

#include "lob/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// An intrusive doubly linked list threaded through a flat pool of nodes.
//
// Links are 32-bit *pool indices*, not pointers. That halves the size of the
// link fields, which is what lets an order node stay inside 32 bytes and so two
// orders share a cache line. It also makes the whole structure trivially
// relocatable: the pool can be memcpy'd or snapshotted without pointer fixups.
//
// The list stores no nodes of its own -- it only holds head/tail indices and
// borrows `prev`/`next` fields from the node type. `Accessor` supplies those:
//
//     struct A {
//         static Handle& next(Node&);
//         static Handle& prev(Node&);
//     };
//
// Every operation is O(1), including unlink-from-the-middle, which is the point:
// a cancel names an order, not a position, and must not walk the queue.
// ---------------------------------------------------------------------------
template <class Node, class Accessor, class Pool>
class IntrusiveList {
public:
    [[nodiscard]] Handle head() const noexcept { return head_; }
    [[nodiscard]] Handle tail() const noexcept { return tail_; }
    [[nodiscard]] bool   empty() const noexcept { return head_ == kNullHandle; }

    void clear() noexcept { head_ = tail_ = kNullHandle; }

    // Append to the back. New arrivals always go to the tail, which is what
    // makes the queue FIFO and therefore time-prioritised.
    void push_back(Pool& pool, Handle h) noexcept {
        Node& n = pool[h];
        Accessor::next(n) = kNullHandle;
        Accessor::prev(n) = tail_;
        if (tail_ == kNullHandle) {
            head_ = tail_ = h;
        } else {
            Accessor::next(pool[tail_]) = h;
            tail_ = h;
        }
    }

    // Push to the front. Only used when restoring priority (e.g. rolling back a
    // partially applied replace), never on the normal arrival path.
    void push_front(Pool& pool, Handle h) noexcept {
        Node& n = pool[h];
        Accessor::prev(n) = kNullHandle;
        Accessor::next(n) = head_;
        if (head_ == kNullHandle) {
            head_ = tail_ = h;
        } else {
            Accessor::prev(pool[head_]) = h;
            head_ = h;
        }
    }

    // Unlink a node we already hold a handle to. This is the O(1) cancel.
    void unlink(Pool& pool, Handle h) noexcept {
        Node& n = pool[h];
        const Handle p = Accessor::prev(n);
        const Handle x = Accessor::next(n);

        if (p == kNullHandle) head_ = x;
        else                  Accessor::next(pool[p]) = x;

        if (x == kNullHandle) tail_ = p;
        else                  Accessor::prev(pool[x]) = p;

        Accessor::prev(n) = kNullHandle;
        Accessor::next(n) = kNullHandle;
    }

    Handle pop_front(Pool& pool) noexcept {
        const Handle h = head_;
        if (h != kNullHandle) unlink(pool, h);
        return h;
    }

    // Walks the queue. Only used by tests, market-data snapshots and the
    // invariant checker -- never by the matching path.
    template <class Fn>
    void for_each(const Pool& pool, Fn&& fn) const {
        for (Handle h = head_; h != kNullHandle;) {
            const Handle nxt = Accessor::next(const_cast<Pool&>(pool)[h]);
            fn(h, pool[h]);
            h = nxt;
        }
    }

    [[nodiscard]] std::size_t size_slow(const Pool& pool) const noexcept {
        std::size_t n = 0;
        for_each(pool, [&](Handle, const Node&) { ++n; });
        return n;
    }

private:
    Handle head_ = kNullHandle;
    Handle tail_ = kNullHandle;
};

}  // namespace lob
