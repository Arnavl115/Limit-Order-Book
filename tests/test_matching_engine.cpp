#include "core/matching_engine.hpp"
#include "core/order_book.hpp"
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
    o.ts = 1000 + id;
    return o;
}

// -------------------- helpers templated --------------------

template<typename Book>
static void checkNoCrossRests(Book& book) {
    MatchingEngine<Book> eng(book);
    // empty book: buy at 100 should rest, no trades
    auto r = eng.processOrder(make(1, Side::Buy, 100, 10));
    CHECK(r.status == ExecStatus::Resting);
    CHECK(r.trades.empty());
    CHECK(r.filled == 0);
    CHECK(r.remaining == 10);
    CHECK(book.contains(1));
    CHECK(book.bestBid().value() == 100);
    CHECK(book.orderCount() == 1);
}

template<typename Book>
static void checkFullFillAtBestPrice(Book& book) {
    MatchingEngine<Book> eng(book);
    // maker ask 105 qty10
    eng.processOrder(make(1, Side::Sell, 105, 10));
    CHECK(book.bestAsk().value() == 105);
    // taker buy 110 qty10 should fully fill at 105
    auto r = eng.processOrder(make(2, Side::Buy, 110, 10));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.trades.size() == 1);
    CHECK(r.filled == 10);
    CHECK(r.trades[0].price == 105); // maker's price
    CHECK(r.trades[0].qty == 10);
    CHECK(r.trades[0].takerId == 2);
    CHECK(r.trades[0].makerId == 1);
    CHECK(!book.contains(1));
    CHECK(!book.contains(2)); // taker fully filled does not rest
    CHECK(book.empty());
}

template<typename Book>
static void checkMultiLevelSweep(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 5));
    eng.processOrder(make(2, Side::Sell, 101, 5));
    eng.processOrder(make(3, Side::Sell, 102, 5));
    CHECK(book.levelCount(Side::Sell) == 3);
    // taker buy 102 qty 12 should sweep 5+5+2 across 3 levels at makers prices
    auto r = eng.processOrder(make(10, Side::Buy, 102, 12));
    CHECK(r.trades.size() == 3);
    CHECK(r.filled == 12);
    CHECK(r.trades[0].price == 100 && r.trades[0].qty == 5);
    CHECK(r.trades[1].price == 101 && r.trades[1].qty == 5);
    CHECK(r.trades[2].price == 102 && r.trades[2].qty == 2);
    CHECK(!book.contains(1));
    CHECK(!book.contains(2));
    CHECK(book.contains(3));
    CHECK(book.findOrder(3)->remaining == 3);
    CHECK(r.status == ExecStatus::Filled);
    CHECK(book.totalQuantity(Side::Sell, 102) == 3);
}

template<typename Book>
static void checkPartialFillPlusResidual(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 10));
    // taker buy 100 qty 15 -> 10 filled, 5 rests as bid 100
    auto r = eng.processOrder(make(2, Side::Buy, 100, 15));
    CHECK(r.status == ExecStatus::PartiallyFilled);
    CHECK(r.filled == 10);
    CHECK(r.remaining == 5);
    CHECK(r.trades.size() == 1);
    CHECK(book.contains(2));
    CHECK(book.findOrder(2)->remaining == 5);
    CHECK(book.bestBid().value() == 100);
    CHECK(!book.contains(1));
}

template<typename Book>
static void checkFIFOWithinLevel(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 10));
    eng.processOrder(make(2, Side::Sell, 100, 10));
    eng.processOrder(make(3, Side::Sell, 100, 10));
    // taker buy 100 qty 25 should consume 1 (10), 2 (10), 3 (5) in FIFO
    auto r = eng.processOrder(make(10, Side::Buy, 100, 25));
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].makerId == 1);
    CHECK(r.trades[1].makerId == 2);
    CHECK(r.trades[2].makerId == 3);
    CHECK(r.trades[2].qty == 5);
    CHECK(!book.contains(1));
    CHECK(!book.contains(2));
    CHECK(book.contains(3));
    CHECK(book.findOrder(3)->remaining == 5);
}

template<typename Book>
static void checkCancelledMakerSkipped(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 10));
    eng.processOrder(make(2, Side::Sell, 101, 10));
    CHECK(book.cancelOrder(1));
    // best ask now 101, taker buy 101 qty10 should match 2 not 1
    auto r = eng.processOrder(make(10, Side::Buy, 101, 10));
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].makerId == 2);
    CHECK(r.trades[0].price == 101);
    CHECK(book.empty());
}

template<typename Book>
static void checkPriceEqualityCrosses(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 10));
    auto r = eng.processOrder(make(2, Side::Buy, 100, 10));
    CHECK(r.trades.size() == 1);
    CHECK(r.status == ExecStatus::Filled);
    // equality should cross
}

template<typename Book>
static void checkNoCrossWhenPriceDoesNotCross(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 105, 10));
    auto r = eng.processOrder(make(2, Side::Buy, 100, 10)); // buy 100 < ask 105
    CHECK(r.trades.empty());
    CHECK(r.status == ExecStatus::Resting);
    CHECK(book.contains(1));
    CHECK(book.contains(2));
    CHECK(book.bestBid().value() == 100);
    CHECK(book.bestAsk().value() == 105);
    CHECK(*book.bestBid() < *book.bestAsk());
}

template<typename Book>
static void checkNeverCrossedAfterMatch(Book& book) {
    MatchingEngine<Book> eng(book);
    for (int i = 0; i < 20; ++i) {
        eng.processOrder(make(100+i, Side::Sell, 110 + (i%3), 5));
        eng.processOrder(make(200+i, Side::Buy, 100 - (i%3), 5));
        auto b = book.bestBid();
        auto a = book.bestAsk();
        if (b.has_value() && a.has_value()) {
            CHECK(*b < *a);
        }
        // random taker that may or may not cross
        auto r = eng.processOrder(make(300+i, Side::Buy, 112, 3));
        if (book.bestBid().has_value() && book.bestAsk().has_value()) {
            CHECK(*book.bestBid() < *book.bestAsk());
        }
        (void)r;
    }
}

template<typename Book>
static void checkEventOrderingAndInvariants(Book& book) {
    CountingEventSink sink;
    MatchingEngine<Book> eng(book, &sink);
    eng.processOrder(make(1, Side::Sell, 100, 5));
    eng.processOrder(make(2, Side::Sell, 101, 5));
    sink.clear();
    auto r = eng.processOrder(make(10, Side::Buy, 101, 8));
    // trades seq ascending
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].seq < r.trades[1].seq);
    CHECK(r.trades[0].tradeId < r.trades[1].tradeId);
    CHECK(sink.tradeCount == 2);
    // onTrade called per maker, onOrderUpdate at least New + Filled/Partial
    CHECK(sink.orderUpdateCount >= 3); // New taker + 2 maker updates + final taker
    // sum trade qty = filled
    Quantity sum = 0;
    for (auto& tr : r.trades) sum += tr.qty;
    CHECK(sum == r.filled);
    CHECK(sum == 8);
    // each trade price == maker price
    CHECK(r.trades[0].price == 100);
    CHECK(r.trades[1].price == 101);
}

template<typename Book>
static void checkValidationRejects(Book& book) {
    CountingEventSink sink;
    MatchingEngine<Book> eng(book, &sink);
    // zero qty
    Order z = make(1, Side::Buy, 100, 0);
    auto r1 = eng.processOrder(z);
    CHECK(r1.status == ExecStatus::Rejected);
    CHECK(r1.trades.empty());
    CHECK(book.empty());
    // non-positive price
    auto r2 = eng.processOrder(make(2, Side::Buy, 0, 10));
    CHECK(r2.status == ExecStatus::Rejected);
    // duplicate id
    eng.processOrder(make(10, Side::Buy, 100, 10));
    auto r3 = eng.processOrder(make(10, Side::Buy, 100, 10));
    CHECK(r3.status == ExecStatus::Rejected);
    CHECK(r3.reason == "duplicate_id");
    // seq not consumed on reject: next valid order should still get seq
    auto r4 = eng.processOrder(make(11, Side::Buy, 100, 10));
    CHECK(r4.status == ExecStatus::Resting); // no cross (sell side empty except? but we have?
    // ensure book not corrupted
    CHECK(book.contains(10));
    CHECK(book.contains(11));
}

template<typename Book>
static void checkSeqStrictlyIncreasing(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 10));
    auto r2 = eng.processOrder(make(2, Side::Buy, 100, 5)); // partial fill maker 1
    auto r3 = eng.processOrder(make(3, Side::Buy, 100, 10)); // sweep remaining 5 + rest 5
    // resting orders seq should be increasing
    const Order* o1 = book.findOrder(3); // id3 resting 5
    // maker 1 should be filled and gone
    CHECK(!book.contains(1));
    CHECK(o1 != nullptr);
    CHECK(o1->seq > 1);
    // check taker seq ordering via MatchResult
    CHECK(r2.takerSeq < r3.takerSeq);
}

// -------------------- OrderBook tests --------------------

TEST(matching_engine_orderbook_no_cross_rests) {
    OrderBook book;
    checkNoCrossRests(book);
}
TEST(matching_engine_orderbook_full_fill_best) {
    OrderBook book;
    checkFullFillAtBestPrice(book);
}
TEST(matching_engine_orderbook_multi_level_sweep) {
    OrderBook book;
    checkMultiLevelSweep(book);
}
TEST(matching_engine_orderbook_partial_plus_residual) {
    OrderBook book;
    checkPartialFillPlusResidual(book);
}
TEST(matching_engine_orderbook_fifo) {
    OrderBook book;
    checkFIFOWithinLevel(book);
}
TEST(matching_engine_orderbook_cancelled_maker_skipped) {
    OrderBook book;
    checkCancelledMakerSkipped(book);
}
TEST(matching_engine_orderbook_price_equality) {
    OrderBook book;
    checkPriceEqualityCrosses(book);
}
TEST(matching_engine_orderbook_no_cross_price_gap) {
    OrderBook book;
    checkNoCrossWhenPriceDoesNotCross(book);
}
TEST(matching_engine_orderbook_never_crossed) {
    OrderBook book;
    checkNeverCrossedAfterMatch(book);
}
TEST(matching_engine_orderbook_event_ordering) {
    OrderBook book;
    checkEventOrderingAndInvariants(book);
}
TEST(matching_engine_orderbook_validation) {
    OrderBook book;
    checkValidationRejects(book);
}
TEST(matching_engine_orderbook_seq_increasing) {
    OrderBook book;
    checkSeqStrictlyIncreasing(book);
}

// -------------------- FastOrderBook tests (bounded domain 1..1000) --------------------

TEST(matching_engine_fastbook_no_cross_rests) {
    FastOrderBook book(1, 1000);
    checkNoCrossRests(book);
}
TEST(matching_engine_fastbook_full_fill_best) {
    FastOrderBook book(1, 1000);
    checkFullFillAtBestPrice(book);
}
TEST(matching_engine_fastbook_multi_level_sweep) {
    FastOrderBook book(1, 1000);
    checkMultiLevelSweep(book);
}
TEST(matching_engine_fastbook_partial_plus_residual) {
    FastOrderBook book(1, 1000);
    checkPartialFillPlusResidual(book);
}
TEST(matching_engine_fastbook_fifo) {
    FastOrderBook book(1, 1000);
    checkFIFOWithinLevel(book);
}
TEST(matching_engine_fastbook_cancelled_maker_skipped) {
    FastOrderBook book(1, 1000);
    checkCancelledMakerSkipped(book);
}
TEST(matching_engine_fastbook_price_equality) {
    FastOrderBook book(1, 1000);
    checkPriceEqualityCrosses(book);
}
TEST(matching_engine_fastbook_no_cross_price_gap) {
    FastOrderBook book(1, 1000);
    checkNoCrossWhenPriceDoesNotCross(book);
}
TEST(matching_engine_fastbook_never_crossed) {
    FastOrderBook book(1, 1000);
    checkNeverCrossedAfterMatch(book);
}
TEST(matching_engine_fastbook_event_ordering) {
    FastOrderBook book(1, 1000);
    checkEventOrderingAndInvariants(book);
}
TEST(matching_engine_fastbook_validation) {
    FastOrderBook book(1, 1000);
    checkValidationRejects(book);
}
TEST(matching_engine_fastbook_seq_increasing) {
    FastOrderBook book(1, 1000);
    checkSeqStrictlyIncreasing(book);
}

// Extra edge: empty book taker rests
TEST(matching_engine_empty_book_rests) {
    OrderBook ob;
    MatchingEngine<OrderBook> eng(ob);
    auto r = eng.processOrder(make(1, Side::Buy, 100, 10));
    CHECK(r.trades.empty());
    CHECK(r.status == ExecStatus::Resting);
    CHECK(ob.bestBid().value() == 100);

    FastOrderBook fb(1, 1000);
    MatchingEngine<FastOrderBook> feng(fb);
    auto fr = feng.processOrder(make(1, Side::Buy, 100, 10));
    CHECK(fr.trades.empty());
    CHECK(fr.status == ExecStatus::Resting);
}

// Opposite side isolation: buy should not sweep bids
TEST(matching_engine_side_isolation) {
    OrderBook book;
    MatchingEngine<OrderBook> eng(book);
    eng.processOrder(make(1, Side::Buy, 100, 10));
    eng.processOrder(make(2, Side::Buy, 101, 10));
    // taker buy should not match bids even though price 101 >= 100
    auto r = eng.processOrder(make(10, Side::Buy, 200, 5));
    CHECK(r.trades.empty());
    CHECK(r.status == ExecStatus::Resting);
    CHECK(book.levelCount(Side::Buy) == 3);
    CHECK(book.levelCount(Side::Sell) == 0);
}

// Maker exact-fill accounting
TEST(matching_engine_maker_exact_fill) {
    OrderBook book;
    MatchingEngine<OrderBook> eng(book);
    eng.processOrder(make(1, Side::Sell, 100, 7));
    auto r = eng.processOrder(make(2, Side::Buy, 100, 7));
    CHECK(r.filled == 7);
    CHECK(r.trades[0].qty == 7);
    CHECK(!book.contains(1));
    CHECK(book.empty());
}
