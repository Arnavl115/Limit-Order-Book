#include "core/matching_engine.hpp"
#include "core/order_book.hpp"
#include "core/fast_order_book.hpp"
#include "test_framework.hpp"

using namespace lob;

static Order mk(OrderId id, Side side, Price price, Quantity qty, OrderType type = OrderType::Limit) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = type;
    o.price = price;
    o.qty = qty;
    o.remaining = qty;
    o.ts = id * 1000;
    return o;
}

// ---------------- helpers ----------------

template<typename Book>
void testMarketEmptyRejected(Book& book) {
    MatchingEngine<Book> eng(book);
    auto r = eng.processOrder(mk(1, Side::Buy, 0, 10, OrderType::Market));
    CHECK(r.status == ExecStatus::Rejected);
    CHECK(r.reason == "no_liquidity");
    CHECK(r.trades.empty());
    CHECK(book.empty());
}

template<typename Book>
void testMarketSweepsMultipleLevels(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 5, OrderType::Limit));
    eng.processOrder(mk(2, Side::Sell, 101, 5, OrderType::Limit));
    eng.processOrder(mk(3, Side::Sell, 102, 5, OrderType::Limit));
    auto r = eng.processOrder(mk(10, Side::Buy, 0, 12, OrderType::Market));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.filled == 12);
    CHECK(r.trades.size() == 3);
    // market sweeps at maker prices regardless of taker price (0)
    CHECK(r.trades[0].price == 100 && r.trades[0].qty == 5);
    CHECK(r.trades[1].price == 101 && r.trades[1].qty == 5);
    CHECK(r.trades[2].price == 102 && r.trades[2].qty == 2);
    CHECK(!book.contains(1) && !book.contains(2) && book.contains(3));
    CHECK(book.findOrder(3)->remaining == 3);
    // market residual never rests
    CHECK(!book.contains(10));
}

template<typename Book>
void testMarketPartialInsufficientDepth(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 5, OrderType::Limit));
    auto r = eng.processOrder(mk(10, Side::Buy, 0, 10, OrderType::Market));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.filled == 5);
    CHECK(r.trades.size() == 1);
    CHECK(book.empty()); // sell side consumed, market residual discarded
    CHECK(!book.contains(10));
}

template<typename Book>
void testMarketIgnoresPrice(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 200, 10, OrderType::Limit));
    // market buy with price 0 should still cross 200
    auto r = eng.processOrder(mk(2, Side::Buy, 0, 10, OrderType::Market));
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].price == 200);
}

template<typename Book>
void testIOCNoCrossCancelled(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 105, 10, OrderType::Limit));
    auto r = eng.processOrder(mk(2, Side::Buy, 100, 10, OrderType::IOC));
    CHECK(r.status == ExecStatus::Cancelled);
    CHECK(r.trades.empty());
    CHECK(!book.contains(2)); // IOC never rests
    CHECK(book.contains(1));
}

template<typename Book>
void testIOCFullFill(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    auto r = eng.processOrder(mk(2, Side::Buy, 100, 10, OrderType::IOC));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.filled == 10);
    CHECK(!book.contains(1) && !book.contains(2));
    CHECK(book.empty());
}

template<typename Book>
void testIOCPartialFillCancelled(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    eng.processOrder(mk(2, Side::Sell, 101, 10, OrderType::Limit));
    // IOC buy 100 qty15 -> should fill 10 at 100, cancel residual 5
    auto r = eng.processOrder(mk(10, Side::Buy, 100, 15, OrderType::IOC));
    CHECK(r.status == ExecStatus::Cancelled);
    CHECK(r.filled == 10);
    CHECK(r.remaining == 5);
    CHECK(r.trades.size() == 1);
    CHECK(!book.contains(10));
    CHECK(!book.contains(1));
    CHECK(book.contains(2)); // second ask not touched
    CHECK(book.bestAsk().value() == 101);
}

template<typename Book>
void testFOKInsufficientRejectedAtomically(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 5, OrderType::Limit));
    eng.processOrder(mk(2, Side::Sell, 101, 5, OrderType::Limit));
    // need 12, available crossing at price 101 is 10 (5+5)
    auto r = eng.processOrder(mk(10, Side::Buy, 101, 12, OrderType::FOK));
    CHECK(r.status == ExecStatus::Rejected);
    CHECK(r.reason == "fok_insufficient_liquidity");
    CHECK(r.trades.empty());
    CHECK(r.filled == 0);
    // book unchanged atomically
    CHECK(book.contains(1) && book.contains(2));
    CHECK(book.orderCount() == 2);
    CHECK(book.totalQuantity(Side::Sell, 100) == 5);
}

template<typename Book>
void testFOKSufficientFilled(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 5, OrderType::Limit));
    eng.processOrder(mk(2, Side::Sell, 101, 5, OrderType::Limit));
    auto r = eng.processOrder(mk(10, Side::Buy, 101, 10, OrderType::FOK));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.filled == 10);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].price == 100 && r.trades[1].price == 101);
    CHECK(book.empty());
}

template<typename Book>
void testFOKOneLevelExact(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    auto r = eng.processOrder(mk(2, Side::Buy, 100, 10, OrderType::FOK));
    CHECK(r.status == ExecStatus::Filled);
    CHECK(r.filled == 10);
    CHECK(book.empty());
}

template<typename Book>
void testFOKPriceGapInsufficient(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    eng.processOrder(mk(2, Side::Sell, 105, 10, OrderType::Limit));
    // FOK buy 100 qty15 would need 15 at price 100, but only 10 available at crossing price 100; total depth 20 but only 10 crosses, so should reject
    auto r = eng.processOrder(mk(10, Side::Buy, 100, 15, OrderType::FOK));
    CHECK(r.status == ExecStatus::Rejected);
    CHECK(r.trades.empty());
    CHECK(book.contains(1) && book.contains(2));
}

template<typename Book>
void testModifyRequeueAndCross(Book& book) {
    MatchingEngine<Book> eng(book);
    // place two buys at same price 100: id1 then id2
    eng.processOrder(mk(1, Side::Buy, 100, 10, OrderType::Limit));
    eng.processOrder(mk(2, Side::Buy, 100, 10, OrderType::Limit));
    // modify id1 to price 99 (worse) and qty 20: should requeue at tail
    bool ok = eng.modifyOrder(1, 99, 20);
    CHECK(ok);
    CHECK(book.contains(1));
    CHECK(book.findOrder(1)->price == 99);
    CHECK(book.findOrder(1)->remaining == 20);
    // best bid still 100 (id2), not 99, because modified order went to worse price
    CHECK(book.bestBid().value() == 100);
    // add ask that crosses 100 but not 99
    auto r = eng.processOrder(mk(10, Side::Sell, 100, 10, OrderType::Limit));
    // should match id2 (price 100) not modified id1 (price 99)
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].makerId == 2);
    CHECK(!book.contains(2));
    CHECK(book.contains(1));
}

template<typename Book>
void testModifyCancelReaddSameId(Book& book) {
    MatchingEngine<Book> eng(book);
    eng.processOrder(mk(1, Side::Buy, 100, 10, OrderType::Limit));
    bool ok = eng.modifyOrder(1, 101, 5);
    CHECK(ok);
    CHECK(book.contains(1));
    CHECK(book.findOrder(1)->price == 101);
    CHECK(book.findOrder(1)->remaining == 5);
    // seq should be new (higher than original)
    CHECK(book.findOrder(1)->seq > 1);
}

template<typename Book>
void testCancelEngineEmits(Book& book) {
    CountingEventSink sink;
    MatchingEngine<Book> eng(book, &sink);
    eng.processOrder(mk(1, Side::Buy, 100, 10, OrderType::Limit));
    sink.clear();
    bool ok = eng.cancelOrder(1);
    CHECK(ok);
    CHECK(!book.contains(1));
    CHECK(sink.orderUpdateCount == 1);
    CHECK(sink.lastReport.status == ExecStatus::Cancelled);
    CHECK(sink.bookTickCount == 1);
}

// -------------------- OrderBook tests --------------------

TEST(order_types_market_empty_rejected_orderbook) { OrderBook b; testMarketEmptyRejected(b); }
TEST(order_types_market_sweep_orderbook) { OrderBook b; testMarketSweepsMultipleLevels(b); }
TEST(order_types_market_partial_orderbook) { OrderBook b; testMarketPartialInsufficientDepth(b); }
TEST(order_types_market_ignores_price_orderbook) { OrderBook b; testMarketIgnoresPrice(b); }

TEST(order_types_ioc_no_cross_orderbook) { OrderBook b; testIOCNoCrossCancelled(b); }
TEST(order_types_ioc_full_fill_orderbook) { OrderBook b; testIOCFullFill(b); }
TEST(order_types_ioc_partial_orderbook) { OrderBook b; testIOCPartialFillCancelled(b); }

TEST(order_types_fok_insufficient_orderbook) { OrderBook b; testFOKInsufficientRejectedAtomically(b); }
TEST(order_types_fok_sufficient_orderbook) { OrderBook b; testFOKSufficientFilled(b); }
TEST(order_types_fok_exact_orderbook) { OrderBook b; testFOKOneLevelExact(b); }
TEST(order_types_fok_price_gap_orderbook) { OrderBook b; testFOKPriceGapInsufficient(b); }

TEST(order_types_modify_requeue_orderbook) { OrderBook b; testModifyRequeueAndCross(b); }
TEST(order_types_modify_same_id_orderbook) { OrderBook b; testModifyCancelReaddSameId(b); }
TEST(order_types_cancel_engine_orderbook) { OrderBook b; testCancelEngineEmits(b); }

// -------------------- FastOrderBook tests --------------------

TEST(order_types_market_empty_rejected_fastbook) { FastOrderBook b(1,1000); testMarketEmptyRejected(b); }
TEST(order_types_market_sweep_fastbook) { FastOrderBook b(1,1000); testMarketSweepsMultipleLevels(b); }
TEST(order_types_market_partial_fastbook) { FastOrderBook b(1,1000); testMarketPartialInsufficientDepth(b); }
TEST(order_types_market_ignores_price_fastbook) { FastOrderBook b(1,1000); testMarketIgnoresPrice(b); }

TEST(order_types_ioc_no_cross_fastbook) { FastOrderBook b(1,1000); testIOCNoCrossCancelled(b); }
TEST(order_types_ioc_full_fill_fastbook) { FastOrderBook b(1,1000); testIOCFullFill(b); }
TEST(order_types_ioc_partial_fastbook) { FastOrderBook b(1,1000); testIOCPartialFillCancelled(b); }

TEST(order_types_fok_insufficient_fastbook) { FastOrderBook b(1,1000); testFOKInsufficientRejectedAtomically(b); }
TEST(order_types_fok_sufficient_fastbook) { FastOrderBook b(1,1000); testFOKSufficientFilled(b); }
TEST(order_types_fok_exact_fastbook) { FastOrderBook b(1,1000); testFOKOneLevelExact(b); }
TEST(order_types_fok_price_gap_fastbook) { FastOrderBook b(1,1000); testFOKPriceGapInsufficient(b); }

TEST(order_types_modify_requeue_fastbook) { FastOrderBook b(1,1000); testModifyRequeueAndCross(b); }
TEST(order_types_modify_same_id_fastbook) { FastOrderBook b(1,1000); testModifyCancelReaddSameId(b); }
TEST(order_types_cancel_engine_fastbook) { FastOrderBook b(1,1000); testCancelEngineEmits(b); }

// Parity for order types: same sequence on both books yields same results
TEST(order_types_parity_market_ioc_fok) {
    OrderBook ob;
    FastOrderBook fb(1,1000);
    MatchingEngine<OrderBook> eob(ob);
    MatchingEngine<FastOrderBook> efb(fb);
    // Setup same book state
    eob.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    efb.processOrder(mk(1, Side::Sell, 100, 10, OrderType::Limit));
    eob.processOrder(mk(2, Side::Sell, 101, 10, OrderType::Limit));
    efb.processOrder(mk(2, Side::Sell, 101, 10, OrderType::Limit));

    auto r1ob = eob.processOrder(mk(10, Side::Buy, 100, 15, OrderType::IOC));
    auto r1fb = efb.processOrder(mk(10, Side::Buy, 100, 15, OrderType::IOC));
    CHECK(r1ob.status == r1fb.status);
    CHECK(r1ob.filled == r1fb.filled);
    CHECK(r1ob.trades.size() == r1fb.trades.size());

    // FOK that should succeed on remaining ask at 101
    auto r2ob = eob.processOrder(mk(11, Side::Buy, 101, 10, OrderType::FOK));
    auto r2fb = efb.processOrder(mk(11, Side::Buy, 101, 10, OrderType::FOK));
    CHECK(r2ob.status == r2fb.status);
    CHECK(r2ob.filled == r2fb.filled);

    // Market sweep
    eob.processOrder(mk(20, Side::Sell, 102, 5, OrderType::Limit));
    efb.processOrder(mk(20, Side::Sell, 102, 5, OrderType::Limit));
    auto r3ob = eob.processOrder(mk(30, Side::Buy, 0, 5, OrderType::Market));
    auto r3fb = efb.processOrder(mk(30, Side::Buy, 0, 5, OrderType::Market));
    CHECK(r3ob.status == r3fb.status);
    CHECK(r3ob.filled == r3fb.filled);
}
