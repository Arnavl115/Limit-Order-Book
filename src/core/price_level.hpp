#pragma once

#include <cassert>
#include <cstddef>
#include <list>

#include "order.hpp"

namespace lob {

// A single price level in the book. Holds every order resting at `price_`.
//
// Orders are stored in a std::list in FIFO order: the head of the list is the
// oldest order and therefore the next to be executed under time priority.
//
// Complexity guarantees:
//   - insert : O(1) (append to the tail)
//   - erase  : O(1) (std::list splice/erase via iterator)
//   - size/totalQuantity : O(1)
class PriceLevel {
public:
    using OrderList = std::list<Order>;
    using iterator = OrderList::iterator;
    using const_iterator = OrderList::const_iterator;

    explicit PriceLevel(Price price) noexcept : price_(price) {}

    PriceLevel(const PriceLevel&) = default;
    PriceLevel& operator=(const PriceLevel&) = default;
    PriceLevel(PriceLevel&&) noexcept = default;
    PriceLevel& operator=(PriceLevel&&) noexcept = default;

    // Append `order` to the tail (FIFO). Returns an iterator to the stored
    // node; the OrderMap will keep it for O(1) cancellation later.
    iterator insert(Order order) {
        total_qty_ += order.remaining;
        orders_.push_back(std::move(order));
        return std::prev(orders_.end());
    }

    // Erase the order at `it` in O(1). Returns the iterator that followed it
    // (matches std::list semantics). Decrements the level's total quantity.
    iterator erase(iterator it) noexcept {
        total_qty_ -= it->remaining;
        return orders_.erase(it);
    }

    // Reduce the resting quantity of the order at `it` by `qty`. Used when an
    // order is partially filled by the matching engine. Keeps total_qty_
    // consistent with the book. O(1).
    void reduce(iterator it, Quantity qty) noexcept {
        assert(qty <= it->remaining && "fill larger than resting quantity");
        it->remaining -= qty;
        it->status = it->isFilled() ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        total_qty_ -= qty;
    }

    [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }
    [[nodiscard]] Quantity totalQuantity() const noexcept { return total_qty_; }
    [[nodiscard]] Price price() const noexcept { return price_; }

    // The oldest order at this level (next to execute under time priority).
    [[nodiscard]] const Order& front() const noexcept { return orders_.front(); }
    [[nodiscard]] Order& front() noexcept { return orders_.front(); }

    iterator begin() noexcept { return orders_.begin(); }
    iterator end() noexcept { return orders_.end(); }
    const_iterator begin() const noexcept { return orders_.begin(); }
    const_iterator end() const noexcept { return orders_.end(); }

private:
    Price price_;
    OrderList orders_;
    Quantity total_qty_ = 0;
};

}  // namespace lob