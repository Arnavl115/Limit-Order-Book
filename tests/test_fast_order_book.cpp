#include "core/fast_order_book.hpp"

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

// Bounded price domain used throughout this file.
static FastOrderBook makeBook(std::size_t chunk = OrderArena::kDefaultChunk) {
    return FastOrderBook(1, 300, chunk);
}

TEST(fastbook_empty_book_has_no_quotes) {
    FastOrderBook book = makeBook();
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

TEST(fastbook_add_sets_best_bid_and_ask) {
    FastOrderBook book = makeBook();
    CHECK(book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.bestAsk().has_value());
    CHECK(book.bestBidLevel() != nullptr);
    CHECK(book.bestBidLevel()->price == 100);
    CHECK(book.bestBidLevel()->totalQuantity() == 10);
    CHECK(book.orderCount() == 1);
    CHECK(book.levelCount(Side::Buy) == 1);

    CHECK(book.addOrder(make(2, Side::Sell, 200, 20)));
    CHECK(book.bestAsk().value() == 200);
    CHECK(book.bestAskLevel()->totalQuantity() == 20);
    CHECK(book.orderCount() == 2);
    CHECK(book.levelCount(Side::Sell) == 1);
}

TEST(fastbook_best_bid_is_highest_price) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 10));
    book.addOrder(make(3, Side::Buy, 102, 10));
    CHECK(book.bestBid().value() == 105);

    book.addOrder(make(4, Side::Buy, 110, 10));
    CHECK(book.bestBid().value() == 110);
    CHECK(book.levelCount(Side::Buy) == 4);
}

TEST(fastbook_best_ask_is_lowest_price) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Sell, 200, 10));
    book.addOrder(make(2, Side::Sell, 195, 10));
    book.addOrder(make(3, Side::Sell, 198, 10));
    CHECK(book.bestAsk().value() == 195);

    book.addOrder(make(4, Side::Sell, 190, 10));
    CHECK(book.bestAsk().value() == 190);
    CHECK(book.levelCount(Side::Sell) == 4);
}

TEST(fastbook_fifo_within_price_level) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 20));
    book.addOrder(make(3, Side::Buy, 100, 30));

    FastPriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    CHECK(lvl->size() == 3);
    CHECK(lvl->totalQuantity() == 60);
    CHECK(lvl->front()->order.id == 1);  // oldest order executes first
    CHECK(book.totalQuantity(Side::Buy, 100) == 60);

    const OrderNode* n = lvl->begin();
    CHECK(n->order.id == 1);
    n = n->next;
    CHECK(n->order.id == 2);
    n = n->next;
    CHECK(n->order.id == 3);
    CHECK(n->next == nullptr);
}

TEST(fastbook_cancel_order_updates_level) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 20));
    book.addOrder(make(3, Side::Buy, 100, 30));

    CHECK(book.cancelOrder(2));
    CHECK(!book.contains(2));
    CHECK(book.orderCount() == 2);
    CHECK(book.totalQuantity(Side::Buy, 100) == 40);

    FastPriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    CHECK(lvl->size() == 2);
    CHECK(lvl->front()->order.id == 1);
    CHECK(lvl->front()->next->order.id == 3);
}

TEST(fastbook_cancel_best_bid_reveals_next) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 105, 10));
    book.addOrder(make(2, Side::Buy, 100, 10));
    CHECK(book.bestBid().value() == 105);

    CHECK(book.cancelOrder(1));
    CHECK(book.bestBid().value() == 100);
    CHECK(book.levelCount(Side::Buy) == 1);
}

TEST(fastbook_cancel_last_order_removes_level) {
    FastOrderBook book = makeBook();
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

TEST(fastbook_cancel_unknown_id_fails) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    CHECK(!book.cancelOrder(99));
    CHECK(!book.cancelOrder(0));
    CHECK(book.orderCount() == 1);
}

TEST(fastbook_duplicate_id_rejected) {
    FastOrderBook book = makeBook();
    CHECK(book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(!book.addOrder(make(1, Side::Buy, 100, 10)));
    CHECK(!book.addOrder(make(1, Side::Sell, 200, 10)));  // ids unique book-wide
    CHECK(book.orderCount() == 1);
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.bestAsk().has_value());
}

TEST(fastbook_invalid_order_rejected) {
    FastOrderBook book = makeBook();
    CHECK(!book.addOrder(make(1, Side::Buy, 100, 0)));   // zero qty
    CHECK(!book.addOrder(make(2, Side::Buy, 0, 10)));    // zero price
    CHECK(!book.addOrder(make(3, Side::Buy, -5, 10)));   // negative price
    CHECK(!book.addOrder(make(4, Side::Buy, 301, 10)));  // above domain max
    CHECK(book.empty());
    CHECK(book.orderCount() == 0);
}

TEST(fastbook_assigns_sequence_numbers) {
    FastOrderBook book = makeBook();
    Order a = make(1, Side::Buy, 100, 10);
    Order b = make(2, Side::Buy, 100, 10);
    CHECK(a.seq == 0);  // unassigned before entry
    book.addOrder(a);
    book.addOrder(b);
    CHECK(book.findOrder(1)->seq > 0);
    CHECK(book.findOrder(2)->seq > book.findOrder(1)->seq);
}

TEST(fastbook_sides_are_isolated) {
    FastOrderBook book = makeBook();
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

TEST(fastbook_total_quantity_per_price) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 100, 5));
    book.addOrder(make(3, Side::Buy, 101, 7));
    CHECK(book.totalQuantity(Side::Buy, 100) == 15);
    CHECK(book.totalQuantity(Side::Buy, 101) == 7);
    CHECK(book.totalQuantity(Side::Buy, 102) == 0);
    CHECK(book.totalQuantity(Side::Sell, 100) == 0);
}

TEST(fastbook_mixed_sequence_state) {
    FastOrderBook book = makeBook();
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

TEST(fastbook_engine_hook_reduce_through_level) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 100));
    book.addOrder(make(2, Side::Buy, 100, 50));
    CHECK(book.totalQuantity(Side::Buy, 100) == 150);

    FastPriceLevel* lvl = book.bestBidLevel();
    CHECK(lvl != nullptr);
    lvl->reduce(lvl->front(), 60);  // id1: 100 -> 40 (partial fill)
    CHECK(book.findOrder(1) != nullptr);
    CHECK(book.findOrder(1)->remaining == 40);
    CHECK(book.findOrder(1)->status == OrderStatus::PartiallyFilled);
    CHECK(book.totalQuantity(Side::Buy, 100) == 90);

    lvl->reduce(lvl->front(), 40);  // id1 fully filled; engine removes it next
    CHECK(book.findOrder(1)->isFilled());
    CHECK(book.totalQuantity(Side::Buy, 100) == 50);

    CHECK(book.cancelOrder(1));  // engine removes a fully-filled order this way
    CHECK(!book.contains(1));
    CHECK(book.totalQuantity(Side::Buy, 100) == 50);
    CHECK(book.orderCount() == 1);
    CHECK(book.bestBid().value() == 100);
}

TEST(fastbook_arena_reuse_after_full_drain) {
    FastOrderBook book = makeBook();  // default chunk: big enough for the loop
    for (OrderId i = 1; i <= 1000; ++i) {
        CHECK(book.addOrder(make(i, Side::Buy, static_cast<Price>(i % 200 + 1), 1)));
    }
    CHECK(book.orderCount() == 1000);

    // Drain the book entirely.
    for (OrderId i = 1; i <= 1000; ++i) {
        CHECK(book.cancelOrder(i));
    }
    CHECK(book.empty());
    CHECK(book.bestBidLevel() == nullptr);
    CHECK(book.bestAskLevel() == nullptr);

    // Re-adding must work (arena free list and level pool are recycled).
    for (OrderId i = 1; i <= 1000; ++i) {
        CHECK(book.addOrder(make(i, Side::Buy, static_cast<Price>(i % 200 + 1), 1)));
    }
    CHECK(book.orderCount() == 1000);
    CHECK(book.bestBid().has_value());
}

TEST(fastbook_cancel_non_best_level_leaves_best_untouched) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 110, 10));
    book.addOrder(make(2, Side::Buy, 105, 10));
    book.addOrder(make(3, Side::Buy, 100, 10));

    CHECK(book.cancelOrder(3));  // deepest level gone; best must stay 110
    CHECK(book.bestBid().value() == 110);
    CHECK(book.levelCount(Side::Buy) == 2);

    CHECK(book.cancelOrder(2));
    CHECK(book.bestBid().value() == 110);
    CHECK(book.levelCount(Side::Buy) == 1);
}

TEST(fastbook_best_walk_skips_emptied_frontier) {
    FastOrderBook book = makeBook();
    // Ask levels 190, 195, 200; cancel the two best in reverse order.
    book.addOrder(make(1, Side::Sell, 190, 10));
    book.addOrder(make(2, Side::Sell, 195, 10));
    book.addOrder(make(3, Side::Sell, 200, 10));
    CHECK(book.bestAsk().value() == 190);

    CHECK(book.cancelOrder(1));
    CHECK(book.bestAsk().value() == 195);
    CHECK(book.cancelOrder(2));
    CHECK(book.bestAsk().value() == 200);
    CHECK(book.cancelOrder(3));
    CHECK(!book.bestAsk().has_value());
}

TEST(fastbook_for_each_level_best_first_order) {
    FastOrderBook book = makeBook();
    book.addOrder(make(1, Side::Buy, 100, 10));
    book.addOrder(make(2, Side::Buy, 105, 10));
    book.addOrder(make(3, Side::Buy, 102, 10));
    book.addOrder(make(4, Side::Sell, 200, 10));
    book.addOrder(make(5, Side::Sell, 190, 10));

    std::vector<Price> bids;
    book.forEachLevel(Side::Buy, [&](const FastPriceLevel* lvl) { bids.push_back(lvl->price); });
    CHECK(bids.size() == 3);
    CHECK(bids[0] == 105 && bids[1] == 102 && bids[2] == 100);

    std::vector<Price> asks;
    book.forEachLevel(Side::Sell, [&](const FastPriceLevel* lvl) { asks.push_back(lvl->price); });
    CHECK(asks.size() == 2);
    CHECK(asks[0] == 190 && asks[1] == 200);
}

TEST(fastbook_price_bounds_reported) {
    FastOrderBook book = makeBook();
    CHECK(book.minPrice() == 1);
    CHECK(book.maxPrice() == 300);
}
