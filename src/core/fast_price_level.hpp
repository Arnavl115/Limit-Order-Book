#pragma once

#include <cassert>
#include <cstddef>

#include "order_arena.hpp"

namespace lob {

// A single price level in FastOrderBook. Holds every resting order at one
// price as an intrusive doubly-linked list of arena-allocated OrderNode*,
// in FIFO order: head is the oldest order and therefore the next to execute
// under time priority.
//
// This is the Phase 2B analogue of PriceLevel (which wraps std::list). The
// intrusive list removes the per-order heap allocation and node indirection
// of std::list, at the cost of nodes living in an arena rather than owning
// their memory.
//
// Complexity guarantees:
//   - insert : O(1) (tail append through the node's own prev/next pointers)
//   - erase  : O(1) (unlink via the node's pointers; no list scan)
//   - size / totalQuantity : O(1)
class FastPriceLevel {
public:
    Price price = 0;   // owning level key (mirrors the price array index)
    Side side = Side::Buy;

    // Tail insert in FIFO order. The node must be a fresh arena node not
    // already linked anywhere. Adopts the node: its `level` back-pointer is
    // set here so cancellation can reach this level in O(1).
    void insert(OrderNode* n) noexcept {
        assert(n->prev == nullptr && n->next == nullptr && "node must be unlinked");
        n->level = this;
        n->prev = tail;
        n->next = nullptr;
        if (tail != nullptr) {
            tail->next = n;
        } else {
            head = n;
        }
        tail = n;
        ++size_;
        total_qty_ += n->order.remaining;
    }

    // Unlink `n` in O(1). Leaves the node unlinked (prev/next/level cleared)
    // so the arena can recycle it. Returns the following node (nullptr if `n`
    // was the tail), matching std::list::erase semantics.
    OrderNode* erase(OrderNode* n) noexcept {
        OrderNode* following = n->next;
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            head = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        } else {
            tail = n->prev;
        }
        --size_;
        total_qty_ -= n->order.remaining;
        n->prev = nullptr;
        n->next = nullptr;
        n->level = nullptr;
        return following;
    }

    // Reduce the resting quantity of `n` by `qty` (partial fill). Keeps
    // total_qty_ consistent with the book. O(1).
    void reduce(OrderNode* n, Quantity qty) noexcept {
        assert(qty <= n->order.remaining && "fill larger than resting quantity");
        n->order.remaining -= qty;
        n->order.status =
            n->order.isFilled() ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        total_qty_ -= qty;
    }

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] Quantity totalQuantity() const noexcept { return total_qty_; }

    // The oldest order at this level (next to execute under time priority).
    [[nodiscard]] OrderNode* front() noexcept { return head; }
    [[nodiscard]] const OrderNode* front() const noexcept { return head; }

    [[nodiscard]] OrderNode* begin() noexcept { return head; }
    [[nodiscard]] OrderNode* end() noexcept { return nullptr; }
    [[nodiscard]] const OrderNode* begin() const noexcept { return head; }
    [[nodiscard]] const OrderNode* end() const noexcept { return nullptr; }

private:
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
    std::size_t size_ = 0;
    Quantity total_qty_ = 0;
};

}  // namespace lob
