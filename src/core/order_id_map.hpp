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
// Two-tier design, because exchange engines assign order ids from a
// monotonically increasing counter:
//
//   Tier 1 (fast path): dense ids [0, kDirectCap) index a flat pointer array
//     (direct_[id]). Add/find/erase are a single array load/store with no
//     hashing and no probing — the whole hot path is allocation-free and the
//     access pattern for sequential ids is perfectly prefetch-friendly.
//
//   Tier 2 (fallback): ids at or above kDirectCap go into a Swiss-style SIMD
//     open-addressing hash table, so arbitrary/64-bit ids keep working.
//     - a 1-byte control array (one control byte per slot) holds a 7-bit
//       fingerprint (h2) plus empty/deleted sentinels; probes scan 16 control
//       bytes (one SSE load) and dereference the arena node only on a
//       fingerprint match;
//     - tombstone deletions are compacted by rehash once the combined
//       live+deleted occupancy crosses the load factor.
//
// Sentinel control bytes: 0x80 == empty, 0xFE == deleted, anything else is an
// h2 fingerprint with the low bit set (odd), so it can never collide with the
// two even sentinels.
//
// Complexity: insert/find/erase are O(1) amortized on either tier, with zero
// per-element allocation.
class OrderIdMap {
public:
    struct SlotHandle {
        std::size_t index = 0;  // direct_ index, or hash slot in values_/ctrl_
        std::uint8_t h2 = 0;    // fingerprint to write on hash commit
        bool is_duplicate = false;
        bool is_direct = false;  // true: index addresses direct_; false: hash
    };

    OrderIdMap() = default;

    OrderIdMap(const OrderIdMap&) = delete;
    OrderIdMap& operator=(const OrderIdMap&) = delete;

    // Pre-size the buffers to hold `expected` entries without reallocating.
    void reserve(std::size_t expected) {
        direct_.reserve(expected);
        std::size_t min_cap = (expected * 10 + kLoadFactor10 - 1) / kLoadFactor10;
        if (min_cap > capacity_) {
            rehash(nextCapacity(min_cap));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Single-probe find-or-prepare-insert for addOrder. Detects a duplicate id
    // and returns the handle the value will be committed to.
    SlotHandle findOrPrepareInsert(OrderId key) {
        if (key < kDirectCap) {
            if (key >= direct_.size()) {
                direct_.resize(static_cast<std::size_t>(key) + 1, nullptr);
            }
            if (direct_[key] != nullptr) {
                return SlotHandle{key, 0, true, true};
            }
            return SlotHandle{key, 0, false, true};
        }
        return findOrPrepareInsertHash(key);
    }

    // Commit `value` to a slot prepared by findOrPrepareInsert in O(1) without
    // re-probing. The caller must not commit a handle with is_duplicate set.
    void commitInsert(SlotHandle h, OrderNode* value) noexcept {
        if (h.is_direct) {
            direct_[h.index] = value;
            ++size_;
            return;
        }
        ctrl_[h.index] = h.h2;
        values_[h.index] = reinterpret_cast<std::uintptr_t>(value);
        ++size_;
        ++hash_size_;
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
        if (key < kDirectCap) {
            if (key >= direct_.size() || direct_[key] == nullptr) {
                return false;
            }
            direct_[key] = nullptr;
            --size_;
            return true;
        }
        return eraseHash(key);
    }

    // Returns the mapped OrderNode*, or nullptr when `key` is absent.
    [[nodiscard]] OrderNode* find(OrderId key) const noexcept {
        if (key < kDirectCap) {
            return key < direct_.size() ? direct_[key] : nullptr;
        }
        return findHash(key);
    }

private:
    static constexpr std::size_t kDirectCap = 1u << 22;  // 4,194,304 direct ids

    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::size_t kLoadFactor10 = 7;  // rehash at ~70% occupancy
    static constexpr std::size_t kGroupSize = 16;

    static constexpr std::uint8_t kEmpty = 0x80;
    static constexpr std::uint8_t kDeleted = 0xFE;

    static std::uint64_t hash(OrderId key) noexcept {
        return static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ull;
    }

    // 7-bit fingerprint with the low bit set: odd, so it can never equal the
    // even sentinels 0x80/0xFE.
    static std::uint8_t fingerprint(std::uint64_t h) noexcept {
        return static_cast<std::uint8_t>((h >> 57) | 1u);
    }

    // Group-aligned start bucket for `h`.
    std::size_t bucket(std::uint64_t h) const noexcept {
        return (static_cast<std::size_t>(h) & mask_) & ~(kGroupSize - 1);
    }

    // SSE2 load of the 16 control bytes for the group starting at slot `i`.
    __m128i ctrlGroup(std::size_t i) const noexcept {
        return _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(ctrl_.data() + i));
    }

    // Bitmask (bit j set) of the 16 slots whose control byte equals `h2`.
    int matchMask(std::uint8_t h2, std::size_t i) const noexcept {
        const __m128i v = ctrlGroup(i);
        const __m128i m = _mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(h2)));
        return _mm_movemask_epi8(m);
    }

    // Bitmask (bit j set) of the 16 empty slots in the group.
    int emptyMask(std::size_t i) const noexcept {
        const __m128i v = ctrlGroup(i);
        const __m128i m =
            _mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(kEmpty)));
        return _mm_movemask_epi8(m);
    }

    static int ctz(int mask) noexcept {
        unsigned long idx = 0;
        _BitScanForward(&idx, static_cast<unsigned long>(mask));
        return static_cast<int>(idx);
    }

    static std::size_t nextCapacity(std::size_t min) noexcept {
        std::size_t cap = kMinCapacity;
        while (cap < min) {
            cap *= 2;
        }
        return cap;
    }

    // --- hash tier -------------------------------------------------------

    SlotHandle findOrPrepareInsertHash(OrderId key) {
        if (capacity_ == 0 ||
            (hash_size_ + hash_deleted_ + 1) * 10 > capacity_ * kLoadFactor10) {
            rehash(capacity_ == 0 ? kMinCapacity : capacity_ * 2);
        }
        const std::uint64_t h = hash(key);
        const std::uint8_t h2 = fingerprint(h);
        std::size_t i = bucket(h);
        while (true) {
            int match = matchMask(h2, i);
            while (match != 0) {
                const int b = ctz(match);
                const std::size_t s = i + static_cast<std::size_t>(b);
                OrderNode* n = reinterpret_cast<OrderNode*>(values_[s]);
                if (n->order.id == key) {
                    return SlotHandle{s, h2, true, false};  // duplicate
                }
                match &= match - 1;
            }
            const int empty = emptyMask(i);
            if (empty != 0) {
                return SlotHandle{i + static_cast<std::size_t>(ctz(empty)),
                                  h2, false, false};
            }
            i = (i + kGroupSize) & mask_;
        }
    }

    bool eraseHash(OrderId key) noexcept {
        if (capacity_ == 0) {
            return false;
        }
        const std::uint64_t h = hash(key);
        const std::uint8_t h2 = fingerprint(h);
        std::size_t i = bucket(h);
        while (true) {
            int match = matchMask(h2, i);
            while (match != 0) {
                const int b = ctz(match);
                const std::size_t s = i + static_cast<std::size_t>(b);
                OrderNode* n = reinterpret_cast<OrderNode*>(values_[s]);
                if (n->order.id == key) {
                    ctrl_[s] = kDeleted;
                    values_[s] = 0;
                    --size_;
                    --hash_size_;
                    ++hash_deleted_;
                    return true;
                }
                match &= match - 1;
            }
            if (emptyMask(i) != 0) {
                return false;
            }
            i = (i + kGroupSize) & mask_;
        }
    }

    OrderNode* findHash(OrderId key) const noexcept {
        if (capacity_ == 0) {
            return nullptr;
        }
        const std::uint64_t h = hash(key);
        const std::uint8_t h2 = fingerprint(h);
        std::size_t i = bucket(h);
        while (true) {
            int match = matchMask(h2, i);
            while (match != 0) {
                const int b = ctz(match);
                const std::size_t s = i + static_cast<std::size_t>(b);
                OrderNode* n = reinterpret_cast<OrderNode*>(values_[s]);
                if (n->order.id == key) {
                    return n;
                }
                match &= match - 1;
            }
            if (emptyMask(i) != 0) {
                return nullptr;
            }
            i = (i + kGroupSize) & mask_;
        }
    }

    void rehash(std::size_t new_cap) {
        std::vector<std::uint8_t> old_ctrl = std::move(ctrl_);
        std::vector<std::uintptr_t> old_vals = std::move(values_);
        const std::size_t old_cap = capacity_;
        ctrl_.assign(new_cap, kEmpty);
        values_.resize(new_cap);
        capacity_ = new_cap;
        mask_ = new_cap - 1;
        hash_size_ = 0;
        hash_deleted_ = 0;
        for (std::size_t i = 0; i < old_cap; ++i) {
            if (old_ctrl[i] != kEmpty && old_ctrl[i] != kDeleted) {
                OrderNode* n = reinterpret_cast<OrderNode*>(old_vals[i]);
                SlotHandle h = findOrPrepareInsertHash(n->order.id);
                ctrl_[h.index] = h.h2;
                values_[h.index] = reinterpret_cast<std::uintptr_t>(n);
                ++hash_size_;
            }
        }
    }

    // --- state -----------------------------------------------------------

    std::vector<OrderNode*> direct_;  // tier 1: id -> node
    std::vector<std::uint8_t> ctrl_;  // tier 2: control bytes
    std::vector<std::uintptr_t> values_;  // tier 2: node pointers
    std::size_t capacity_ = 0;       // tier 2 slot count
    std::size_t mask_ = 0;
    std::size_t size_ = 0;           // total (tier 1 + tier 2)
    std::size_t hash_size_ = 0;      // tier 2 live entries
    std::size_t hash_deleted_ = 0;   // tier 2 tombstones
};

}  // namespace lob