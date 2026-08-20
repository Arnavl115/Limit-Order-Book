#pragma once

// Phase 3A — Book backend abstraction.
//
// One engine template `MatchingEngine<Book>` drives both
//   OrderBook      (mandated std::map / std::list / unordered_map)
//   FastOrderBook  (arena + open-addressing map + bounded price array)
// via a single concept `BookBackend` and a trait shim `LevelTrait<Book>`.
//
// There is no virtual dispatch on the hot path: the engine is a class
// template and all trait helpers are force-inlined static functions.
// A thin non-template adapter for the gateway (type-erased `IBookAdapter`)
// is introduced later in Phase 4 (`book_adapter.hpp`) and is intentionally
// not part of the hot path.
//
// Design notes
//   - PriceLevel vs FastPriceLevel have different shapes:
//       PriceLevel  : std::list<Order> FIFO, price() method, front() -> Order&
//       FastLevel   : intrusive doubly-linked OrderNode* FIFO, price field,
//                     front() -> OrderNode*
//     `LevelTrait` unifies reduce/erase/front/price so engine code is written
//     once. The engine never touches `orders_` directly; it goes through the
//     book's addOrder/cancelOrder/findOrder and the trait's level helpers.
//   - `seq` is assigned by the book (OrderBook/FastOrderBook::addOrder) but
//     the engine may pre-assign via allocateSeq() so that a taker that never
//     rests still gets a monotonic seq.  Books preserve a non-zero incoming
//     seq and advance next_seq_ past it.
//   - All LevelTrait helpers are `[[nodiscard]]` where applicable and noexcept
//     where the underlying operations are.

#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>

#include "fast_order_book.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "price_level.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// LevelTrait — per-book level shim
// ---------------------------------------------------------------------------

template <typename Book>
struct LevelTrait;  // primary — no definition; must be specialized

// Canonical OrderBook specialization
template <>
struct LevelTrait<OrderBook> {
    using BookType = OrderBook;
    using Level = PriceLevel;
    using Handle = PriceLevel::iterator;
    using ConstHandle = PriceLevel::const_iterator;

    [[nodiscard]] static bool empty(const Level* lvl) noexcept {
        return lvl == nullptr || lvl->empty();
    }
    [[nodiscard]] static Price price(const Level* lvl) noexcept {
        return lvl->price();
    }
    [[nodiscard]] static Quantity totalQuantity(const Level* lvl) noexcept {
        return lvl->totalQuantity();
    }
    [[nodiscard]] static std::size_t size(const Level* lvl) noexcept {
        return lvl->size();
    }
    [[nodiscard]] static Handle front(Level* lvl) noexcept {
        return lvl->begin();
    }
    [[nodiscard]] static ConstHandle front(const Level* lvl) noexcept {
        return lvl->begin();
    }
    [[nodiscard]] static Order& getOrder(Handle h) noexcept { return *h; }
    [[nodiscard]] static const Order& getOrder(ConstHandle h) noexcept { return *h; }

    static void reduce(Level* lvl, Handle h, Quantity qty) noexcept {
        lvl->reduce(h, qty);
    }
};

// FastOrderBook specialization
template <>
struct LevelTrait<FastOrderBook> {
    using BookType = FastOrderBook;
    using Level = FastPriceLevel;
    using Handle = OrderNode*;
    using ConstHandle = const OrderNode*;

    [[nodiscard]] static bool empty(const Level* lvl) noexcept {
        return lvl == nullptr || lvl->empty();
    }
    [[nodiscard]] static Price price(const Level* lvl) noexcept {
        return lvl->price;
    }
    [[nodiscard]] static Quantity totalQuantity(const Level* lvl) noexcept {
        return lvl->totalQuantity();
    }
    [[nodiscard]] static std::size_t size(const Level* lvl) noexcept {
        return lvl->size();
    }
    [[nodiscard]] static Handle front(Level* lvl) noexcept { return lvl->front(); }
    [[nodiscard]] static ConstHandle front(const Level* lvl) noexcept {
        return lvl->front();
    }
    [[nodiscard]] static Order& getOrder(Handle h) noexcept { return h->order; }
    [[nodiscard]] static const Order& getOrder(ConstHandle h) noexcept {
        return h->order;
    }

    static void reduce(Level* lvl, Handle h, Quantity qty) noexcept {
        lvl->reduce(h, qty);
    }
};

// ---------------------------------------------------------------------------
// Helper: get best level for a side (engine needs the opposite side)
// ---------------------------------------------------------------------------

template <typename Book>
[[nodiscard]] inline typename LevelTrait<Book>::Level* bestLevelForSide(
    Book& book, Side side) noexcept {
    return side == Side::Buy ? book.bestAskLevel() : book.bestBidLevel();
}

template <typename Book>
[[nodiscard]] inline const typename LevelTrait<Book>::Level* bestLevelForSide(
    const Book& book, Side side) noexcept {
    return side == Side::Buy ? book.bestAskLevel() : book.bestBidLevel();
}

// Opposite-side accessor: taker side buys -> best ask, sell -> best bid.
template <typename Book>
[[nodiscard]] inline typename LevelTrait<Book>::Level* oppositeBestLevel(
    Book& book, Side taker_side) noexcept {
    return taker_side == Side::Buy ? book.bestAskLevel() : book.bestBidLevel();
}

template <typename Book>
[[nodiscard]] inline const typename LevelTrait<Book>::Level* oppositeBestLevel(
    const Book& book, Side taker_side) noexcept {
    return taker_side == Side::Buy ? book.bestAskLevel() : book.bestBidLevel();
}

// ---------------------------------------------------------------------------
// Concept: BookBackend
// ---------------------------------------------------------------------------

template <typename B>
concept BookBackend = requires(B& b, const B& cb, Order o, OrderId id) {
    // mutators the engine needs
    { b.addOrder(o) } -> std::convertible_to<bool>;
    { b.cancelOrder(id) } -> std::convertible_to<bool>;
    { b.allocateSeq() } -> std::convertible_to<SeqNo>;
    { cb.nextSeq() } -> std::convertible_to<SeqNo>;

    // best-level handles (engine hot loop)
    { b.bestBidLevel() } -> std::convertible_to<typename LevelTrait<B>::Level*>;
    { b.bestAskLevel() } -> std::convertible_to<typename LevelTrait<B>::Level*>;
    { cb.bestBidLevel() } -> std::convertible_to<const typename LevelTrait<B>::Level*>;
    { cb.bestAskLevel() } -> std::convertible_to<const typename LevelTrait<B>::Level*>;

    // quoting / introspection (for reports & invariant checks)
    { cb.bestBid() } -> std::convertible_to<std::optional<Price>>;
    { cb.bestAsk() } -> std::convertible_to<std::optional<Price>>;
    { cb.findOrder(id) } -> std::convertible_to<const Order*>;
    { cb.contains(id) } -> std::convertible_to<bool>;
    { cb.empty() } -> std::convertible_to<bool>;
    { cb.orderCount() } -> std::convertible_to<std::size_t>;

    // LevelTrait structural requirements
    requires requires(typename LevelTrait<B>::Level* lvl) {
        { LevelTrait<B>::empty(lvl) } -> std::convertible_to<bool>;
        { LevelTrait<B>::price(lvl) } -> std::convertible_to<Price>;
        { LevelTrait<B>::totalQuantity(lvl) } -> std::convertible_to<Quantity>;
        { LevelTrait<B>::size(lvl) } -> std::convertible_to<std::size_t>;
        { LevelTrait<B>::front(lvl) } -> std::convertible_to<typename LevelTrait<B>::Handle>;
    };
    requires requires(typename LevelTrait<B>::Level* lvl,
                      typename LevelTrait<B>::Handle h, Quantity q) {
        { LevelTrait<B>::getOrder(h) } -> std::convertible_to<Order&>;
        { LevelTrait<B>::reduce(lvl, h, q) } -> std::same_as<void>;
    };
};

// Compile-time proof that both books satisfy the concept.
// If either fails, the engine cannot be instantiated for it.
static_assert(BookBackend<OrderBook>,
              "OrderBook must satisfy BookBackend");
static_assert(BookBackend<FastOrderBook>,
              "FastOrderBook must satisfy BookBackend");

}  // namespace lob
