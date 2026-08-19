#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "order.hpp"

namespace lob {

// Forward declaration: an order node points back at the level that owns it so
// cancellation can reach the level in O(1) without a second lookup.
class FastPriceLevel;

// A node of an order living inside an OrderArena. `order` holds the
// user-visible Order; `prev`/`next` wire the node into the intrusive FIFO list
// of its price level; `level` is the owning level (used by O(1) cancel).
//
// Nodes are allocated from the arena, so their addresses are stable until the
// node is explicitly deallocated. External references are raw OrderNode*
// (safe here: arena memory never relocates), unlike the canonical OrderBook
// which must use std::list iterators because std::list nodes do relocate.
struct OrderNode {
    Order order;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
    FastPriceLevel* level = nullptr;

    // When the node is on the arena's free list, `next` doubles as the
    // free-list link. `prev` and `level` are unused in that state.
};

// Bump-style arena pool for OrderNode. Allocates nodes in contiguous chunks
// and hands them out from an intrusive free list, so the hot paths (add,
// cancel) never touch the heap allocator. Chunk size grows geometrically to
// bound wasted memory on small books while staying cache-friendly on large
// ones.
//
//   - allocate : O(1) (pop from the free list; grow() on first exhaustion)
//   - deallocate : O(1) (push onto the free list)
//
// Not thread-safe, matching the rest of the engine.
class OrderArena {
public:
    static constexpr std::size_t kDefaultChunk = 1u << 16;  // 65,536 nodes
    static constexpr std::size_t kMaxChunk = 1u << 20;      // 1,048,576 nodes

    explicit OrderArena(std::size_t initial_chunk = kDefaultChunk) noexcept {
        if (initial_chunk > kMaxChunk) {
            initial_chunk = kMaxChunk;
        }
        initial_chunk_ = initial_chunk;
    }

    OrderArena(const OrderArena&) = delete;
    OrderArena& operator=(const OrderArena&) = delete;

    OrderNode* allocate() {
        if (free_head_ != nullptr) {
            OrderNode* n = free_head_;
            free_head_ = n->next;  // next doubles as the free-list link
            n->next = nullptr;
            n->prev = nullptr;
            n->level = nullptr;
            return n;
        }
        if (current_index_ >= current_cap_) {
            grow();
        }
        OrderNode* n = &current_chunk_[current_index_++];
        n->order.id = 0;
        n->next = nullptr;
        n->prev = nullptr;
        n->level = nullptr;
        return n;
    }

    void deallocate(OrderNode* n) noexcept {
        n->next = free_head_;
        n->prev = nullptr;
        n->level = nullptr;
        free_head_ = n;
    }

    [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return total_capacity_; }

private:
    struct Chunk {
        std::unique_ptr<OrderNode[]> data;
        std::size_t size = 0;
    };

    void grow() {
        const std::size_t cap = chunks_.empty()
            ? initial_chunk_
            : std::min(chunks_.back().size * 2, kMaxChunk);
        auto data = std::make_unique_for_overwrite<OrderNode[]>(cap);
        current_chunk_ = data.get();
        current_index_ = 0;
        current_cap_ = cap;
        total_capacity_ += cap;
        chunks_.push_back(Chunk{std::move(data), cap});
    }

    std::vector<Chunk> chunks_;
    OrderNode* free_head_ = nullptr;
    OrderNode* current_chunk_ = nullptr;
    std::size_t current_index_ = 0;
    std::size_t current_cap_ = 0;
    std::size_t total_capacity_ = 0;
    std::size_t initial_chunk_;
};

}  // namespace lob
