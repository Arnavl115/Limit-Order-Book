#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "fast_price_level.hpp"
#include "order.hpp"
#include "order_id_map.hpp"

namespace lob {

// Phase 2B high-performance order book. Keeps the same public API as the
// canonical OrderBook (add/cancel/best/...), but replaces the containers with
// Phase 2B structures:
//
//   - Order storage:  arena-pooled intrusive FIFO lists (OrderArena +
//                     FastPriceLevel). No per-order heap allocation.
//   - Order lookup:   OrderIdMap, an open-addressing OrderId -> OrderNode*
//                     table. No per-node allocation (unlike std::unordered_map)
//                     and contiguous-memory cache behavior.
//   - Price lookup:   bounded array indexed by price offset. O(1) lookup,
//                     not O(log N) like the canonical std::map.
//   - Best bid/ask:   cached index per side, refreshed by a walk when the best
//                     level empties. Amortized O(1); see below.
//
// The bounded price domain is fixed at construction: [min_price, max_price].
// Prices outside it are rejected by addOrder. This is the standard high
// frequency LOB trade-off: the O(1) price array buys speed, the price bounds
// buy memory (the array is `span` pointers, 8 bytes each).
//
// Best bid/ask maintenance: the cache is set on insert (if the new price
// improves the best). When the best level is emptied by a cancel/fill, we walk
// the array toward the interior (down for bids, up for asks) to the next
// non-empty level. Each emptied slot is passed over at most once per time it
// is at the frontier, so across a whole session the walk cost is amortized
// O(1) per operation; the worst case is O(price range) for a single op that
// empties a tall frontier.
//
// Invariants (maintained by every mutator, mirroring OrderBook):
//   1. one orders_ entry per resting order, each linked into exactly one level
//      on the side implied by its .side;
//   2. the level that owns a node has level.price == node.order.price;
//   3. no empty level is ever stored: the array slot is nulled and the level
//      returned to the pool when its last order leaves;
//   4. FastPriceLevel::total_qty_ == sum of remaining qty at that price;
//   5. seq strictly increasing in arrival order.
//
// Thread-safety: not thread-safe by design, like OrderBook.
class FastOrderBook {
public:
    using OrderMap = OrderIdMap;

    // Price domain is inclusive on both ends. `initial_chunk` is the first
    // arena chunk size in nodes (see OrderArena).
    FastOrderBook(Price min_price, Price max_price,
                  std::size_t initial_chunk = OrderArena::kDefaultChunk)
        : arena_(initial_chunk), min_price_(min_price), max_price_(max_price) {
        const std::size_t span = static_cast<std::size_t>(max_price - min_price) + 1;
        bids_.levels.assign(span, nullptr);
        asks_.levels.assign(span, nullptr);
        level_pool_.resize(span * 2);
        free_levels_.reserve(span * 2);
        for (std::size_t i = 0; i < span * 2; ++i) {
            free_levels_.push_back(&level_pool_[i]);
        }
    }

    // Reserve `n` slots in the order id map (avoids rehashing on the hot
    // path when the steady-state order count is known in advance).
    void reserveOrders(std::size_t n) { orders_.reserve(n); }

    // --- mutators -------------------------------------------------------

    // Place `order` as a resting limit order at the tail of its price level
    // (FIFO). Assigns the next engine sequence number. Returns false when the
    // id is already in the book, or when there is nothing to rest (qty == 0 or
    // remaining == 0), the price is non-positive, or the price falls outside
    // [min_price, max_price].
    // Complexity: O(1) price lookup + O(1) arena allocate + O(1) tail append.
    bool addOrder(Order order);

    // Remove the resting order `id` in O(1). Returns false when `id` is
    // unknown. Prunes the level (nulls the slot, recycles the level, refreshes
    // the best) when the last order at that price leaves.
    bool cancelOrder(OrderId id) noexcept;

    // --- quotes ---------------------------------------------------------

    [[nodiscard]] std::optional<Price> bestBid() const noexcept;
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
    [[nodiscard]] std::optional<Price> bestPrice(Side side) const noexcept;

    // Handles to the best level, for the matching engine (Phase 3). Null when
    // that side is empty. The returned pointer stays valid until the level is
    // emptied.
    [[nodiscard]] FastPriceLevel* bestBidLevel() noexcept;
    [[nodiscard]] FastPriceLevel* bestAskLevel() noexcept;
    [[nodiscard]] const FastPriceLevel* bestBidLevel() const noexcept;
    [[nodiscard]] const FastPriceLevel* bestAskLevel() const noexcept;

    // --- introspection --------------------------------------------------

    [[nodiscard]] std::size_t orderCount() const noexcept { return orders_.size(); }
    [[nodiscard]] bool contains(OrderId id) const noexcept {
        return orders_.find(id) != nullptr;
    }
    [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }
    [[nodiscard]] std::size_t levelCount(Side side) const noexcept {
        return sideState(side).active_count;
    }

    // Pointer to a resting order, or nullptr. Valid until the order leaves the
    // book (fill or cancel).
    [[nodiscard]] const Order* findOrder(OrderId id) const noexcept;

    // Total resting quantity at an exact price (0 when no such level exists or
    // the price is outside the domain).
    [[nodiscard]] Quantity totalQuantity(Side side, Price price) const noexcept;

    [[nodiscard]] Price minPrice() const noexcept { return min_price_; }
    [[nodiscard]] Price maxPrice() const noexcept { return max_price_; }

    // Visit every active level on `side` in best-first order (bids: high to
    // low; asks: low to high). O(price range) — introspection, not hot path.
    template <typename Fn>
    void forEachLevel(Side side, Fn&& fn) const {
        const SideState& st = sideState(side);
        if (side == Side::Buy) {
            for (std::size_t i = st.levels.size(); i-- > 0;) {
                if (st.levels[i] != nullptr) {
                    fn(st.levels[i]);
                }
            }
        } else {
            for (std::size_t i = 0; i < st.levels.size(); ++i) {
                if (st.levels[i] != nullptr) {
                    fn(st.levels[i]);
                }
            }
        }
    }

private:
    struct SideState {
        std::vector<FastPriceLevel*> levels;  // index = price - min_price
        std::int64_t best_idx = -1;           // -1 == no active level on this side
        std::size_t active_count = 0;
    };

    static std::size_t indexOf(Price price, Price min_price) noexcept {
        return static_cast<std::size_t>(price - min_price);
    }

    SideState& sideState(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
    const SideState& sideState(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    FastPriceLevel* acquireLevel() {
        if (free_levels_.empty()) {
            level_pool_backup_.push_back(std::make_unique<FastPriceLevel>());
            return level_pool_backup_.back().get();
        }
        FastPriceLevel* lvl = free_levels_.back();
        free_levels_.pop_back();
        return lvl;
    }

    void releaseLevel(FastPriceLevel* lvl) noexcept {
        lvl->price = 0;
        lvl->side = Side::Buy;
        free_levels_.push_back(lvl);
    }

    // The array slot `idx` on `side` just became active with `lvl`. Best bid =
    // highest price (largest idx), best ask = lowest price (smallest idx).
    void activateLevel(SideState& st, std::size_t idx, FastPriceLevel* lvl,
                       Side side) noexcept {
        st.levels[idx] = lvl;
        ++st.active_count;
        const std::int64_t i = static_cast<std::int64_t>(idx);
        const bool improves = side == Side::Buy
            ? (st.best_idx < 0 || i > st.best_idx)
            : (st.best_idx < 0 || i < st.best_idx);
        if (improves) {
            st.best_idx = i;
        }
    }

    // The level at array slot `idx` on `side` just emptied. O(amortized 1)
    // best refresh: walk toward the interior (down for bids, up for asks) to
    // the next non-empty level, or to -1 when the side is exhausted.
    void deactivateLevel(SideState& st, std::size_t idx, FastPriceLevel* lvl,
                          Side side) noexcept {
        st.levels[idx] = nullptr;
        --st.active_count;
        if (static_cast<std::int64_t>(idx) == st.best_idx) {
            if (side == Side::Buy) {
                while (st.best_idx >= 0 &&
                       st.levels[static_cast<std::size_t>(st.best_idx)] == nullptr) {
                    --st.best_idx;
                }
            } else {
                while (st.best_idx < static_cast<std::int64_t>(st.levels.size()) &&
                       st.levels[static_cast<std::size_t>(st.best_idx)] == nullptr) {
                    ++st.best_idx;
                }
                if (st.best_idx >= static_cast<std::int64_t>(st.levels.size())) {
                    st.best_idx = -1;
                }
            }
        }
        releaseLevel(lvl);
    }

    SideState bids_;
    SideState asks_;
    OrderMap orders_;
    OrderArena arena_;
    std::vector<FastPriceLevel> level_pool_;
    std::vector<std::unique_ptr<FastPriceLevel>> level_pool_backup_;
    std::vector<FastPriceLevel*> free_levels_;
    SeqNo next_seq_ = 1;  // 0 is reserved as "unassigned"
    Price min_price_;
    Price max_price_;
};

// ---------------------------------------------------------------------------
// Implementation (inline in the header: the hot path must be inlinable).
// ---------------------------------------------------------------------------

inline bool FastOrderBook::addOrder(Order order) {
    if (order.qty == 0 || order.remaining == 0 || order.price <= 0 ||
        order.price < min_price_ || order.price > max_price_) {
        return false;  // rejected: nothing to rest, out of domain
    }

    // Allocate the node first so the map's duplicate check and value store
    // are back-to-back (single cache-line touch for dense ids) before the
    // arena write and level link. A rejected (duplicate) id pays only a
    // free-list push.
    OrderNode* node = arena_.allocate();
    OrderIdMap::SlotHandle handle = orders_.findOrPrepareInsert(order.id);
    if (handle.is_duplicate) {
        arena_.deallocate(node);
        return false;  // duplicate id
    }
    orders_.commitInsert(handle, node);

    order.seq = next_seq_++;
    node->order = order;

    const std::size_t idx = indexOf(order.price, min_price_);
    SideState& st = sideState(order.side);
    FastPriceLevel* lvl = st.levels[idx];
    if (lvl == nullptr) {
        lvl = acquireLevel();
        lvl->price = order.price;
        lvl->side = order.side;
        activateLevel(st, idx, lvl, order.side);
    }
    lvl->insert(node);

    return true;
}

inline bool FastOrderBook::cancelOrder(OrderId id) noexcept {
    OrderNode* node = orders_.find(id);
    if (node == nullptr) {
        return false;  // unknown order id
    }

    FastPriceLevel* lvl = node->level;  // read off the node; no second lookup
    SideState& st = sideState(lvl->side);

    orders_.erase(id);      // drop the map entry first; the node stays alive
    lvl->erase(node);
    arena_.deallocate(node);

    if (lvl->empty()) {
        const std::size_t idx = indexOf(lvl->price, min_price_);
        deactivateLevel(st, idx, lvl, lvl->side);
    }
    return true;
}

inline std::optional<Price> FastOrderBook::bestBid() const noexcept {
    if (bids_.best_idx < 0) {
        return std::nullopt;
    }
    return bids_.levels[static_cast<std::size_t>(bids_.best_idx)]->price;
}

inline std::optional<Price> FastOrderBook::bestAsk() const noexcept {
    if (asks_.best_idx < 0) {
        return std::nullopt;
    }
    return asks_.levels[static_cast<std::size_t>(asks_.best_idx)]->price;
}

inline std::optional<Price> FastOrderBook::bestPrice(Side side) const noexcept {
    return side == Side::Buy ? bestBid() : bestAsk();
}

inline FastPriceLevel* FastOrderBook::bestBidLevel() noexcept {
    return bids_.best_idx < 0
        ? nullptr
        : bids_.levels[static_cast<std::size_t>(bids_.best_idx)];
}

inline FastPriceLevel* FastOrderBook::bestAskLevel() noexcept {
    return asks_.best_idx < 0
        ? nullptr
        : asks_.levels[static_cast<std::size_t>(asks_.best_idx)];
}

inline const FastPriceLevel* FastOrderBook::bestBidLevel() const noexcept {
    return bids_.best_idx < 0
        ? nullptr
        : bids_.levels[static_cast<std::size_t>(bids_.best_idx)];
}

inline const FastPriceLevel* FastOrderBook::bestAskLevel() const noexcept {
    return asks_.best_idx < 0
        ? nullptr
        : asks_.levels[static_cast<std::size_t>(asks_.best_idx)];
}

inline const Order* FastOrderBook::findOrder(OrderId id) const noexcept {
    OrderNode* node = orders_.find(id);
    return node == nullptr ? nullptr : &node->order;
}

inline Quantity FastOrderBook::totalQuantity(Side side, Price price) const noexcept {
    if (price < min_price_ || price > max_price_) {
        return 0;
    }
    const SideState& st = sideState(side);
    const FastPriceLevel* lvl = st.levels[indexOf(price, min_price_)];
    return lvl == nullptr ? 0 : lvl->totalQuantity();
}

}  // namespace lob
