#include "core/book_backend.hpp"
#include "test_framework.hpp"

using namespace lob;

// Compile-time proof the concept is satisfied is already in the header via
// static_assert. This TU adds runtime smoke checks that the trait shim behaves
// identically for both books.

static Order make(OrderId id, Side side, Price price, Quantity qty) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::Limit;
    o.price = price;
    o.qty = qty;
    o.remaining = qty;
    return o;
}

TEST(book_backend_orderbook_satisfies_concept) {
    static_assert(BookBackend<OrderBook>);
    OrderBook b;
    CHECK(b.empty());
    CHECK(!b.bestBidLevel());
    // trait helpers on null/empty level
    CHECK(LevelTrait<OrderBook>::empty(b.bestBidLevel()));
}

TEST(book_backend_fastbook_satisfies_concept) {
    static_assert(BookBackend<FastOrderBook>);
    FastOrderBook b(1, 1000);
    CHECK(b.empty());
    CHECK(!b.bestAskLevel());
    CHECK(LevelTrait<FastOrderBook>::empty(b.bestAskLevel()));
}

TEST(book_backend_trait_front_and_reduce_parity) {
    OrderBook ob;
    ob.addOrder(make(1, Side::Buy, 100, 10));
    ob.addOrder(make(2, Side::Buy, 100, 20));
    auto* lvl = ob.bestBidLevel();
    CHECK(lvl != nullptr);
    CHECK(LevelTrait<OrderBook>::price(lvl) == 100);
    CHECK(LevelTrait<OrderBook>::totalQuantity(lvl) == 30);
    CHECK(LevelTrait<OrderBook>::size(lvl) == 2);
    auto h = LevelTrait<OrderBook>::front(lvl);
    CHECK(LevelTrait<OrderBook>::getOrder(h).id == 1);
    LevelTrait<OrderBook>::reduce(lvl, h, 5);
    CHECK(LevelTrait<OrderBook>::getOrder(h).remaining == 5);
    CHECK(LevelTrait<OrderBook>::totalQuantity(lvl) == 25);

    FastOrderBook fb(1, 1000);
    fb.addOrder(make(1, Side::Buy, 100, 10));
    fb.addOrder(make(2, Side::Buy, 100, 20));
    auto* flvl = fb.bestBidLevel();
    CHECK(flvl != nullptr);
    CHECK(LevelTrait<FastOrderBook>::price(flvl) == 100);
    CHECK(LevelTrait<FastOrderBook>::totalQuantity(flvl) == 30);
    CHECK(LevelTrait<FastOrderBook>::size(flvl) == 2);
    auto fh = LevelTrait<FastOrderBook>::front(flvl);
    CHECK(LevelTrait<FastOrderBook>::getOrder(fh).id == 1);
    LevelTrait<FastOrderBook>::reduce(flvl, fh, 5);
    CHECK(LevelTrait<FastOrderBook>::getOrder(fh).remaining == 5);
    CHECK(LevelTrait<FastOrderBook>::totalQuantity(flvl) == 25);
}

TEST(book_backend_opposite_best_level) {
    OrderBook ob;
    ob.addOrder(make(1, Side::Buy, 100, 10));
    ob.addOrder(make(2, Side::Sell, 105, 10));
    CHECK(oppositeBestLevel(ob, Side::Buy)->price() == 105);
    CHECK(oppositeBestLevel(ob, Side::Sell)->price() == 100);

    FastOrderBook fb(1, 1000);
    fb.addOrder(make(1, Side::Buy, 100, 10));
    fb.addOrder(make(2, Side::Sell, 105, 10));
    CHECK(oppositeBestLevel(fb, Side::Buy)->price == 105);
    CHECK(oppositeBestLevel(fb, Side::Sell)->price == 100);
}
