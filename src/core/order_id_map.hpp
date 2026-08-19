#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "order.hpp"

namespace lob {

struct OrderNode;  // defined in order_arena.hpp; only pointers are stored here

// The OrderId -> OrderNode* side-table of FastOrderBook. Values are raw
// pointers into the OrderArena, so the table is allocation-free on the hot
// path.
//
// std::unordered_map cannot serve this role in a "no per-order allocation"
// book: MSVC's implementation allocates one heap node per element. This is an
// open-addressing (linear probing) map with:
//
//   - a pre-allocatable, growable slot buffer (rehash on load factor, so the
//     per-op cost is amortized to zero after warm-up);
//   - tombstone deletions that are compacted by rehash once the combined
//     live+deleted occupancy crosses the load factor, so heavy add/cancel
//     churn cannot degrade probe chains;
//   - a splitmix64 finalizer, which is cheap and well-distributed even for
//     sequential order ids.
//
// Complexity: insert/find/erase are O(1) amortized, exactly like
// std::unordered_map, but with zero per-element allocation and far better
// cache behavior (open addressing scans a contiguous array).
class OrderIdMap {
public:
    OrderIdMap() = default;

    OrderIdMap(const OrderIdMap&) = delete;
    OrderIdMap& operator=(const OrderIdMap&) = delete;

    // Pre-size the buffer to hold `expected` entries without rehashing.
    void reserve(std::size_t expected) {
        if (expected <= capacity_) {
            return;
        }
        rehash(nextCapacity(expected));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Insert `key -> value`. Returns false (without inserting) when the key is
    // already present.
    bool insert(OrderId key, OrderNode* value) {
        if (capacity_ == 0 ||
            (size_ + deleted_ + 1) * 10 > capacity_ * kLoadFactor10) {
            rehash(capacity_ == 0 ? kMinCapacity : capacity_ * 2);
        }
        std::size_t i = bucket(key);
        while (slots_[i].state == kOccupied) {
            if (slots_[i].key == key) {
                return false;  // duplicate id
            }
            i = (i + 1) & mask_;
        }
        slots_[i].key = key;
        slots_[i].value = value;
        slots_[i].state = kOccupied;
        ++size_;
        return true;
    }

    // Remove `key`. Returns false when it was not present.
    bool erase(OrderId key) noexcept {
        std::size_t i = bucket(key);
        while (slots_[i].state != kEmpty) {
            if (slots_[i].state == kOccupied && slots_[i].key == key) {
                slots_[i].state = kDeleted;
                --size_;
                ++deleted_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    // Returns the mapped OrderNode*, or nullptr when `key` is absent.
    [[nodiscard]] OrderNode* find(OrderId key) const noexcept {
        if (capacity_ == 0) {
            return nullptr;
        }
        std::size_t i = bucket(key);
        while (slots_[i].state != kEmpty) {
            if (slots_[i].state == kOccupied && slots_[i].key == key) {
                return slots_[i].value;
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

private:
    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::size_t kLoadFactor10 = 7;  // rehash at ~70% occupancy

    enum State : std::uint8_t { kEmpty = 0, kOccupied = 1, kDeleted = 2 };

    struct Slot {
        OrderId key = 0;
        OrderNode* value = nullptr;
        std::uint8_t state = kEmpty;
    };

    static std::uint64_t hash(OrderId key) noexcept {
        std::uint64_t x = key + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    std::size_t bucket(OrderId key) const noexcept {
        return static_cast<std::size_t>(hash(key)) & mask_;
    }

    static std::size_t nextCapacity(std::size_t min) noexcept {
        std::size_t cap = kMinCapacity;
        while (cap < min) {
            cap *= 2;
        }
        return cap;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        const std::size_t old_cap = capacity_;
        slots_.assign(new_cap, Slot{});
        capacity_ = new_cap;
        mask_ = new_cap - 1;
        size_ = 0;
        deleted_ = 0;
        for (std::size_t i = 0; i < old_cap; ++i) {
            if (old[i].state == kOccupied) {
                insert(old[i].key, old[i].value);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t deleted_ = 0;
};

}  // namespace lob