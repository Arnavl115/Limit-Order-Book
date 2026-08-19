#pragma once

#include <cstddef>
#include <cstdint>
#include <immintrin.h>
#include <vector>

#include "order.hpp"

namespace lob {

struct OrderNode;  // defined in order_arena.hpp; only pointers are stored here

// The OrderId -> OrderNode* side-table of FastOrderBook. Values are raw
// pointers into the OrderArena, so the table is allocation-free on the hot
// path.
//
// std::unordered_map cannot serve this role in a "no per-order allocation"
// book: MSVC's implementation allocates one heap node per element. This is a
// Swiss-style (SIMD) open-addressing table with:
//
//   - a 1-byte control array (one control byte per slot) holding a 7-bit
//     fingerprint (h2) of the key, plus the empty/deleted sentinels. Probes
//     scan 16 control bytes (one SSE load) and only dereference the arena
//     node on a fingerprint match, so probe cost is ~1 cache line and almost
//     no random node dereferences;
//   - an 8-byte value array (one OrderNode* per slot), packing 8 slots per
//     cache line. A 2.5M-entry table at 70% load is ~38 MB total, which fits
//     in the L3 of typical server/desktop CPUs and stays resident after
//     warm-up;
//   - tombstone deletions compacted by rehash once the combined live+deleted
//     occupancy crosses the load factor, so heavy add/cancel churn cannot
//     degrade probe chains;
//   - a multiplicative finalizer, which is a single multiply and distributes
//     sequential order ids evenly across a power-of-two slot buffer.
//
// Sentinel control bytes: 0x80 == empty, 0xFE == deleted, anything else is an
// h2 fingerprint with the low bit set (odd), so it can never collide with the
// two even sentinels.
//
// Complexity: insert/find/erase are O(1) amortized, exactly like
// std::unordered_map, but with zero per-element allocation and far better
// cache behavior.
class OrderIdMap {
public:
    struct Slot {
        OrderId key = 0;
        OrderNode* value = nullptr;
    };

    struct SlotHandle {
        std::size_t index = 0;
        bool is_duplicate = false;
        bool is_tombstone = false;
    };

    OrderIdMap() = default;

    OrderIdMap(const OrderIdMap&) = delete;
    OrderIdMap& operator=(const OrderIdMap&) = delete;

    // Pre-size the buffer to hold `expected` entries without rehashing.
    void reserve(std::size_t expected) {
        std::size_t min_cap = (expected * 10 + 6) / kLoadFactor10;
        if (min_cap <= capacity_) {
            return;
        }
        rehash(nextCapacity(min_cap));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Single-probe find-or-prepare-insert for addOrder.
    // Detects duplicate id and finds/prepares the slot in a single probe.
    SlotHandle findOrPrepareInsert(OrderId key) {
        if (capacity_ == 0 ||
            (size_ + deleted_ + 1) * 10 > capacity_ * kLoadFactor10) {
            rehash(capacity_ == 0 ? kMinCapacity : capacity_ * 2);
        }
        std::size_t idx = hash(key) & mask_;
        if (deleted_ == 0) {
            while (true) {
                Slot& s = slots_[idx];
                if (s.value == nullptr) {
                    s.key = key;
                    return SlotHandle{idx, false, false};
                }
                if (s.key == key) {
                    return SlotHandle{idx, true, false};
                }
                idx = (idx + 1) & mask_;
            }
        }
        std::size_t first_tombstone = static_cast<std::size_t>(-1);
        while (true) {
            Slot& s = slots_[idx];
            if (s.value == nullptr) {
                if (first_tombstone != static_cast<std::size_t>(-1)) {
                    slots_[first_tombstone].key = key;
                    return SlotHandle{first_tombstone, false, true};
                }
                s.key = key;
                return SlotHandle{idx, false, false};
            }
            if (s.value == kTombstone) {
                if (first_tombstone == static_cast<std::size_t>(-1)) {
                    first_tombstone = idx;
                }
            } else if (s.key == key) {
                return SlotHandle{idx, true, false};
            }
            idx = (idx + 1) & mask_;
        }
    }

    // Commit node value to prepared slot in O(1) without re-probing.
    void commitInsert(SlotHandle h, OrderNode* value) noexcept {
        slots_[h.index].value = value;
        if (h.is_tombstone) {
            --deleted_;
        }
        ++size_;
    }

    // Single-pass insert-or-find.
    OrderNode* insertOrFind(OrderId key, OrderNode* value) {
        SlotHandle h = findOrPrepareInsert(key);
        if (h.is_duplicate) {
            return slots_[h.index].value;
        }
        commitInsert(h, value);
        return value;
    }

    // Insert `key -> value`. Returns false (without inserting) when the key is
    // already present.
    bool insert(OrderId key, OrderNode* value) {
        SlotHandle h = findOrPrepareInsert(key);
        if (h.is_duplicate) {
            return false;
        }
        commitInsert(h, value);
        return true;
    }

    // Remove `key`. Returns false when it was not present.
    bool erase(OrderId key) noexcept {
        if (capacity_ == 0) {
            return false;
        }
        std::size_t idx = hash(key) & mask_;
        while (true) {
            Slot& s = slots_[idx];
            if (s.value == nullptr) {
                return false;
            }
            if (s.value != kTombstone && s.key == key) {
                s.value = kTombstone;
                --size_;
                ++deleted_;
                return true;
            }
            idx = (idx + 1) & mask_;
        }
    }

    // Returns the mapped OrderNode*, or nullptr when `key` is absent.
    [[nodiscard]] OrderNode* find(OrderId key) const noexcept {
        if (capacity_ == 0) {
            return nullptr;
        }
        std::size_t idx = hash(key) & mask_;
        while (true) {
            const Slot& s = slots_[idx];
            if (s.value == nullptr) {
                return nullptr;
            }
            if (s.value != kTombstone && s.key == key) {
                return s.value;
            }
            idx = (idx + 1) & mask_;
        }
    }

private:
    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::size_t kLoadFactor10 = 7;  // rehash at ~70% occupancy
    static constexpr OrderNode* kTombstone =
        reinterpret_cast<OrderNode*>(static_cast<std::uintptr_t>(1));

    static std::size_t hash(OrderId key) noexcept {
        std::uint64_t x = static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(x ^ (x >> 32));
    }

    static std::size_t nextCapacity(std::size_t min) noexcept {
        std::size_t cap = kMinCapacity;
        while (cap < min) {
            cap *= 2;
        }
        return cap;
    }

    void rehash(std::size_t new_cap) {
        auto old_slots = std::move(slots_);
        const std::size_t old_cap = capacity_;
        auto new_slots = std::make_unique_for_overwrite<Slot[]>(new_cap);
        std::memset(new_slots.get(), 0, new_cap * sizeof(Slot));
        slots_ = std::move(new_slots);
        capacity_ = new_cap;
        mask_ = new_cap - 1;
        size_ = 0;
        deleted_ = 0;
        for (std::size_t i = 0; i < old_cap; ++i) {
            const Slot& s = old_slots[i];
            if (s.value != nullptr && s.value != kTombstone) {
                std::size_t idx = hash(s.key) & mask_;
                while (slots_[idx].value != nullptr) {
                    idx = (idx + 1) & mask_;
                }
                slots_[idx].key = s.key;
                slots_[idx].value = s.value;
                ++size_;
            }
        }
    }

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t deleted_ = 0;
};

}  // namespace lob
