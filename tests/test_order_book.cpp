#include <iterator>

#include "core/order_book.hpp"

#include "test_framework.hpp"

using namespace lob;

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

TEST(orderbook_empty_book_has_no_quotes) {
    OrderBook book;
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
    CHECK(!book.bestPrice(Side::Buy).has_value());
    CHECK(!book.bestPrice(Side::Sell).has_value());
    CHECK(book.empty());
    CHECK(book.orderCount() == 0);
    CHECK(book.levelCount(Side::Buy) == 0);
    CHECK(book.levelCount(Side::Sell) == 0);
    CHECK(book.bestBidLevel() == nullptr);
    CHECK(book.bestAskLevel() == nullptr);
    CHECK(book.findOrder(1) == nullptr);
    CHECK(book.totalQuantity(Side::Buy, 100) == 0);
}

TEST(orderbook_add_sets_best_bid_and_ask) {
    OrderBook book;
    CHECK(book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.bestAsk().has_value());
    CHECK(book.bestBidLevel() != nullptr);
    CHECK(book.bestBidLevel()->price() == 100);
    CHECK(book.bestBidLevel()->totalQuantity() == 10);
    CHECK(book.orderCount() == 1);
    CHECK(book.levelCount(Side::Buy) == 1);

    CHECK(book.addOrder(make(2, Side::Sell, 200, 20)));
    CHECK(book.bestAsk().value() == 200);
    CHECK(book.bestAskLevel()->totalQuantity() == 20);
    CHECK(book.orderCount() == 2);
    CHECK(book.levelCount(Side::Sell) == 1);
}

TEST(orderbook_best_bid_is_highest_price) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 10));
    book.addOrder(make(3, Side::Buy, 102, 10));
    CHECK(book.bestBid().value() == 105);

    book.addOrder(make(4, Side::Buy, 110, 10));
    CHECK(book.bestBid().value() == 110);
    CHECK(book.levelCount(Side::Buy) == 4);
}

TEST(orderbook_best_ask_is_lowest_price) {
    OrderBook book;
    book.addOrder(make(1, Side::Sell, 200, 10));
    book.addOrder(make(2, Side::Sell, 195, 10));
    book.addOrder(make(3, Side::Sell, 198, 10));
    CHECK(book.bestAsk().value() == 195);

    book.addOrder(make(4, Side::Sell, 190, 10));
    CHECK(book.bestAsk().value() == 190);
    CHECK(book.levelCount(Side::Sell) == 4);
}

TEST(orderbook_fifo_within_price_level) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 20));
    book.addOrder(make(3, Side::Buy, 100, 30));

    PriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    CHECK(lvl->size() == 3);
    CHECK(lvl->totalQuantity() == 60);
    CHECK(lvl->front().id == 1);  // oldest order executes first
    CHECK(book.totalQuantity(Side::Buy, 100) == 60);

    auto it = lvl->begin();
    CHECK(it->id == 1);
    ++it;
    CHECK(it->id == 2);
    ++it;
    CHECK(it->id == 3);
}

TEST(orderbook_cancel_order_updates_level) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 20));
    book.addOrder(make(3, Side::Buy, 100, 30));

    CHECK(book.cancelOrder(2));
    CHECK(!book.contains(2));
    CHECK(book.orderCount() == 2);
    CHECK(book.totalQuantity(Side::Buy, 100) == 40);

    PriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    CHECK(lvl->size() == 2);
    CHECK(lvl->front().id == 1);
    CHECK(std::prev(lvl->end())->id == 3);
}

TEST(orderbook_cancel_best_bid_reveals_next) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 105, 10));
    book.addOrder(make(2, Side::Buy, 100, 10));
    CHECK(book.bestBid().value() == 105);

    CHECK(book.cancelOrder(1));
    CHECK(book.bestBid().value() == 100);
    CHECK(book.levelCount(Side::Buy) == 1);
}

TEST(orderbook_cancel_last_order_removes_level) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 10));

    CHECK(book.cancelOrder(2));
    CHECK(book.levelCount(Side::Buy) == 1);

    CHECK(book.cancelOrder(1));
    CHECK(book.empty());
    CHECK(book.levelCount(Side::Buy) == 0);
    CHECK(!book.bestBid().has_value());
    CHECK(book.bestBidLevel() == nullptr);
    CHECK(book.findOrder(1) == nullptr);
}

TEST(orderbook_cancel_unknown_id_fails) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    CHECK(!book.cancelOrder(99));
    CHECK(!book.cancelOrder(0));
    CHECK(book.orderCount() == 1);
}

TEST(orderbook_duplicate_id_rejected) {
    OrderBook book;
    CHECK(book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(!book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(!book.addOrder(make(1, Side::Sell, 200, 10)));  // ids unique book-wide
    CHECK(book.orderCount() == 1);
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.bestAsk().has_value());
}

TEST(orderbook_invalid_order_rejected) {
    OrderBook book;
    CHECK(!book.addOrder(make(1, Side::Buy, 100, 0)));   // zero qty
    CHECK(!book.addOrder(make(2, Side::Buy, 0, 10)));    // zero price
    CHECK(!book.addOrder(make(3, Side::Buy, -5, 10)));   // negative price
    CHECK(book.empty());
    CHECK(book.orderCount() == 0);
}

TEST(orderbook_assigns_sequence_numbers) {
    OrderBook book;
    Order a = make(1, Side::Buy, 100, 10);
    Order b = make(2, Side::Buy, 100, 10);
    CHECK(a.seq == 0);  // unassigned before entry
    book.addOrder(a);
    book.addOrder(b);
    CHECK(book.findOrder(1)->seq > 0);
    CHECK(book.findOrder(2)->seq > book.findOrder(1)->seq);
}

TEST(orderbook_sides_are_isolated) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 105, 10));
    book.addOrder(make(2, Side::Buy, 100, 10));
    book.addOrder(make(3, Side::Sell, 190, 10));
    book.addOrder(make(4, Side::Sell, 200, 10));

    CHECK(book.bestBid().value() == 105);
    CHECK(book.bestAsk().value() == 190);
    CHECK(book.bestPrice(Side::Buy).value() == 105);
    CHECK(book.bestPrice(Side::Sell).value() == 190);
    CHECK(book.levelCount(Side::Buy) == 2);
    CHECK(book.levelCount(Side::Sell) == 2);
}

TEST(orderbook_total_quantity_per_price) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 5));
    book.addOrder(make(3, Side::Buy, 101, 7));
    CHECK(book.totalQuantity(Side::Buy, 100) == 15);
    CHECK(book.totalQuantity(Side::Buy, 101) == 7);
    CHECK(book.totalQuantity(Side::Buy, 102) == 0);
    CHECK(book.totalQuantity(Side::Sell, 100) == 0);
}

TEST(orderbook_mixed_sequence_state) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 20));
    book.addOrder(make(3, Side::Sell, 110, 15));
    book.addOrder(make(4, Side::Sell, 108, 5));
    CHECK(book.bestBid().value() == 105);
    CHECK(book.bestAsk().value() == 108);

    book.cancelOrder(2);
    CHECK(book.bestBid().value() == 100);

    book.addOrder(make(5, Side::Buy, 107, 30));
    CHECK(book.bestBid().value() == 107);

    book.addOrder(make(6, Side::Sell, 106, 8));
    CHECK(book.bestAsk().value() == 106);

    book.cancelOrder(6);
    CHECK(book.bestAsk().value() == 108);
    CHECK(book.orderCount() == 4);
    CHECK(book.levelCount(Side::Buy) == 2);
    CHECK(book.levelCount(Side::Sell) == 2);
}

TEST(orderbook_book_snapshot_accessors) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 20));
    book.addOrder(make(3, Side::Sell, 110, 15));

    CHECK(book.bids().size() == 2);
    CHECK(book.asks().size() == 1);
    CHECK(book.bids().rbegin()->first == 105);
    CHECK(book.asks().begin()->first == 110);
    CHECK(book.bids().rbegin()->second.totalQuantity() == 20);
}

TEST(orderbook_engine_hook_reduce_through_level) {
    OrderBook book;
    book.addOrder(make(1, Side::Buy, 100, 100));
    book.addOrder(make(2, Side::Buy, 100, 50));
    CHECK(book.totalQuantity(Side::Buy, 100) == 150);

    PriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    lvl->reduce(lvl->begin(), 60);  // id1: 100 -> 40 (partial fill)
    CHECK(book.findOrder(1) != nullptr);
    CHECK(book.findOrder(1)->remaining == 40);
    CHECK(book.findOrder(1)->status == OrderStatus::PartiallyFilled);
    CHECK(book.totalQuantity(Side::Buy, 100) == 90);

    lvl->reduce(lvl->begin(), 40);  // id1 fully filled; engine removes it next
    CHECK(book.findOrder(1)->isFilled());
    CHECK(book.totalQuantity(Side::Buy, 100) == 50);

    CHECK(book.cancelOrder(1));  // engine removes a fully-filled order this way
    CHECK(!book.contains(1));
    CHECK(book.totalQuantity(Side::Buy, 100) == 50);
    CHECK(book.orderCount() == 1);
    CHECK(book.bestBid().value() == 100);
}
