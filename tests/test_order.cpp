#include "core/order.hpp"

#include "test_framework.hpp"

using namespace lob;

TEST(order_initial_defaults) {
    Order o;
    CHECK(o.id == 0);
    CHECK(o.qty == 0);
    CHECK(o.remaining == 0);
    CHECK(o.filled() == 0);
    CHECK(o.isFilled());  // zero-qty order is trivially filled
}

TEST(order_fill_math) {
    Order o;
    o.id = 42;
    o.side = Side::Sell;
    o.price = 10000;
    o.qty = 100;
    o.remaining = 100;

    CHECK(o.filled() == 0);
    CHECK(o.isResting());
    CHECK(!o.isFilled());

    o.remaining = 40;  // simulate a 60-share partial fill
    CHECK(o.filled() == 60);
    CHECK(o.isResting());

    o.remaining = 0;  // fully filled
    CHECK(o.isFilled());
    CHECK(o.filled() == 100);
}

TEST(order_side_status_strings) {
    CHECK(std::string(toString(Side::Buy)) == "buy");
    CHECK(std::string(toString(Side::Sell)) == "sell");
    CHECK(std::string(toString(OrderStatus::New)) == "new");
    CHECK(std::string(toString(OrderStatus::Filled)) == "filled");
    CHECK(std::string(toString(OrderStatus::Cancelled)) == "cancelled");
}