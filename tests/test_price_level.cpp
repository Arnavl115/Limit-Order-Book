#include "core/price_level.hpp"

#include "test_framework.hpp"

using namespace lob;

static Order make(OrderId id, Quantity qty) {
    Order o;
    o.id = id;
    o.qty = qty;
    o.remaining = qty;
    o.price = 100;
    return o;
}

TEST(price_level_insert_and_fifo_order) {
    PriceLevel level(100);
    CHECK(level.price() == 100);
    CHECK(level.empty());
    CHECK(level.size() == 0);
    CHECK(level.totalQuantity() == 0);

    level.insert(make(1, 10));
    level.insert(make(2, 20));
    level.insert(make(3, 30));

    CHECK(!level.empty());
    CHECK(level.size() == 3);
    CHECK(level.totalQuantity() == 60);

    // FIFO: oldest order (id=1) must be at the front, id=3 at the back.
    CHECK(level.front().id == 1);
    auto it = level.begin();
    CHECK(it->id == 1);
    ++it;
    CHECK(it->id == 2);
    ++it;
    CHECK(it->id == 3);
}

TEST(price_level_erase_is_constant_time_and_consistent) {
    PriceLevel level(100);
    auto it1 = level.insert(make(1, 10));
    auto it2 = level.insert(make(2, 20));
    auto it3 = level.insert(make(3, 30));
    CHECK(level.totalQuantity() == 60);

    // Erase the middle order; total quantity drops and order stays FIFO.
    level.erase(it2);
    CHECK(level.size() == 2);
    CHECK(level.totalQuantity() == 40);
    CHECK(level.front().id == 1);
    CHECK(std::prev(level.end())->id == 3);

    // Erase the head; order id=3 becomes the new head.
    level.erase(it1);
    CHECK(level.size() == 1);
    CHECK(level.totalQuantity() == 30);
    CHECK(level.front().id == 3);

    // Erase the last; level becomes empty.
    level.erase(it3);
    CHECK(level.empty());
    CHECK(level.totalQuantity() == 0);
}

TEST(price_level_insert_returns_stable_iterator) {
    PriceLevel level(100);
    auto it = level.insert(make(7, 5));
    CHECK(it->id == 7);

    // Inserting more orders must not invalidate the existing iterator.
    level.insert(make(8, 5));
    level.insert(make(9, 5));
    CHECK(it->id == 7);
    CHECK(it->remaining == 5);
}

TEST(price_level_reduce_partial_fill) {
    PriceLevel level(100);
    auto it = level.insert(make(1, 100));
    CHECK(level.totalQuantity() == 100);

    level.reduce(it, 60);
    CHECK(it->remaining == 40);
    CHECK(it->filled() == 60);
    CHECK(it->status == OrderStatus::PartiallyFilled);
    CHECK(level.totalQuantity() == 40);

    level.reduce(it, 40);
    CHECK(it->isFilled());
    CHECK(it->status == OrderStatus::Filled);
    CHECK(level.totalQuantity() == 0);
}