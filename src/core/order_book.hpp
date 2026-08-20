#pragma once

#include <cassert>
#include <cstddef>
#include <map>
#include <optional>
#include <unordered_map>

#include "order.hpp"
#include "price_level.hpp"

namespace lob {

// Central order book: resting bids and asks organised by price, with O(1)
// order cancellation and O(log N) best-quote queries.
//
// Representation (mandated in UNDERSTANDING.md):
//   - bids_ / asks_ : std::map<Price, PriceLevel> (red-black tree). Bids are
//     queried from rbegin() (descending -> highest price first), asks from
//     begin() (ascending -> lowest price first).
//   - orders_ : std::unordered_map<OrderId, PriceLevel::iterator> gives O(1)
//     lookup of the exact list node holding any resting order, which is what
//     makes cancellation O(1) instead of O(N).
//
// Why iterators and not raw pointers:
//   A PriceLevel stores orders in a std::list in FIFO order (head = oldest =
//   next to execute). std::list iterators survive insertions and erasures of
//   *other* elements, which is exactly the guarantee the orders_ map relies
//   on. An order's price/side are read off the Order node itself when
//   cancelling, so the map never duplicates key data that could drift.
//
// Invariants maintained by every mutator:
//   1. orders_ has exactly one entry per resting order, and each such order
//      lives in exactly one PriceLevel on the side implied by its .side.
//   2. The Price key of the owning level equals the stored Order.price.
//   3. No empty PriceLevel is ever stored: erasing the last order at a price
//      also erases the price key from the owning side map.
//   4. PriceLevel::total_qty_ == sum of remaining qty at that price.
//   5. Sequence numbers are strictly increasing in arrival order, making seq
//      the authoritative time-priority tie-breaker.
//
// Thread-safety: not thread-safe by design. A single-threaded matching engine
// owns the book; concurrent access requires an external lock.
class OrderBook {
public:
    using PriceMap = std::map<Price, PriceLevel>;
    using OrderMap = std::unordered_map<OrderId, PriceLevel::iterator>;

    // --- mutators -------------------------------------------------------

    // Place `order` as a resting limit order at the tail of its price level
    // (FIFO). Assigns the next engine sequence number. Returns false (rejected)
    // when the id is already in the book, or when there is nothing to rest
    // (qty == 0 or remaining == 0) or the price is non-positive.
    // Complexity: O(log N) price lookup + O(1) append to the level.
    bool addOrder(Order order);

    // Remove the resting order `id` in O(1). Returns false when `id` is
    // unknown. Also prunes the price level from the tree when it becomes
    // empty, so the book never retains dead levels.
    bool cancelOrder(OrderId id) noexcept;

    // --- quotes ---------------------------------------------------------

    [[nodiscard]] std::optional<Price> bestBid() const noexcept;
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
    [[nodiscard]] std::optional<Price> bestPrice(Side side) const noexcept;

    // Handles to the best level, for the matching engine (Phase 3). Null when
    // that side is empty. The returned pointer stays valid until the level is
    // emptied.
    [[nodiscard]] PriceLevel* bestBidLevel() noexcept;
    [[nodiscard]] PriceLevel* bestAskLevel() noexcept;
    [[nodiscard]] const PriceLevel* bestBidLevel() const noexcept;
    [[nodiscard]] const PriceLevel* bestAskLevel() const noexcept;

    // --- introspection --------------------------------------------------

    [[nodiscard]] std::size_t orderCount() const noexcept { return orders_.size(); }
    [[nodiscard]] bool contains(OrderId id) const noexcept {
        return orders_.find(id) != orders_.end();
    }
    [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }
    [[nodiscard]] std::size_t levelCount(Side side) const noexcept;

    // Pointer to a resting order, or nullptr. Valid until the order leaves the
    // book (fill or cancel).
    [[nodiscard]] const Order* findOrder(OrderId id) const noexcept;

    // Total resting quantity at an exact price (0 when no such level exists).
    [[nodiscard]] Quantity totalQuantity(Side side, Price price) const noexcept;

    // Read-only snapshots of the whole book (for tests / API layer).
    [[nodiscard]] const PriceMap& bids() const noexcept { return bids_; }
    [[nodiscard]] const PriceMap& asks() const noexcept { return asks_; }

    // Sequence management — the engine owns seq assignment for taker orders,
    // but resting orders inserted via addOrder still need a seq.  When the
    // incoming Order already carries a non-zero seq (assigned by the engine),
    // it is preserved and next_seq_ is advanced past it; otherwise a fresh
    // seq is minted.  This keeps seq strictly increasing across the lifetime
    // of the book even when the engine pre-assigns.
    [[nodiscard]] SeqNo nextSeq() const noexcept { return next_seq_; }
    SeqNo allocateSeq() noexcept { return next_seq_++; }

private:
    PriceMap& sideMap(Side side) noexcept;
    const PriceMap& sideMap(Side side) const noexcept;

    PriceMap bids_;
    PriceMap asks_;
    OrderMap orders_;
    SeqNo next_seq_ = 1;  // 0 is reserved as "unassigned"
};

// ---------------------------------------------------------------------------
// Implementation (inline in the header: the hot path must be inlinable).
// ---------------------------------------------------------------------------

inline bool OrderBook::addOrder(Order order) {
    if (order.qty == 0 || order.remaining == 0 || order.price <= 0 ||
        orders_.find(order.id) != orders_.end()) {
        return false;  // rejected: nothing to rest or duplicate id
    }
    if (order.seq == 0) {
        order.seq = next_seq_++;
    } else if (order.seq >= next_seq_) {
        next_seq_ = order.seq + 1;
    }

    PriceMap& side = sideMap(order.side);
    // try_emplace constructs the PriceLevel in place only if the price is new;
    // otherwise the existing level is reused. Either way we get its iterator.
    PriceMap::iterator levelIt = side.try_emplace(order.price, order.price).first;
    PriceLevel& level = levelIt->second;

    PriceLevel::iterator orderIt = level.insert(std::move(order));
    orders_.emplace(orderIt->id, orderIt);
    return true;
}

inline bool OrderBook::cancelOrder(OrderId id) noexcept {
    OrderMap::iterator found = orders_.find(id);
    if (found == orders_.end()) {
        return false;  // unknown order id
    }

    PriceLevel::iterator orderIt = found->second;
    const Price price = orderIt->price;  // read owning level key off the node
    const Side side = orderIt->side;

    orders_.erase(found);  // drop the map entry first; the list node stays alive

    PriceMap::iterator levelIt = sideMap(side).find(price);
    assert(levelIt != sideMap(side).end() && "price level missing for resting order");
    PriceLevel& level = levelIt->second;

    level.erase(orderIt);
    if (level.empty()) {
        sideMap(side).erase(levelIt);  // O(log N) prune; keeps the book tight
    }
    return true;
}

inline std::optional<Price> OrderBook::bestBid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.rbegin()->first;  // highest bid price
}

inline std::optional<Price> OrderBook::bestAsk() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;  // lowest ask price
}

inline std::optional<Price> OrderBook::bestPrice(Side side) const noexcept {
    return side == Side::Buy ? bestBid() : bestAsk();
}

inline PriceLevel* OrderBook::bestBidLevel() noexcept {
    return bids_.empty() ? nullptr : &bids_.rbegin()->second;
}

inline PriceLevel* OrderBook::bestAskLevel() noexcept {
    return asks_.empty() ? nullptr : &asks_.begin()->second;
}

inline const PriceLevel* OrderBook::bestBidLevel() const noexcept {
    return bids_.empty() ? nullptr : &bids_.rbegin()->second;
}

inline const PriceLevel* OrderBook::bestAskLevel() const noexcept {
    return asks_.empty() ? nullptr : &asks_.begin()->second;
}

inline std::size_t OrderBook::levelCount(Side side) const noexcept {
    return sideMap(side).size();
}

inline const Order* OrderBook::findOrder(OrderId id) const noexcept {
    OrderMap::const_iterator found = orders_.find(id);
    return found == orders_.end() ? nullptr : &*found->second;
}

inline Quantity OrderBook::totalQuantity(Side side, Price price) const noexcept {
    PriceMap::const_iterator levelIt = sideMap(side).find(price);
    return levelIt == sideMap(side).end() ? 0 : levelIt->second.totalQuantity();
}

inline OrderBook::PriceMap& OrderBook::sideMap(Side side) noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

inline const OrderBook::PriceMap& OrderBook::sideMap(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

}  // namespace lob
