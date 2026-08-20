#include "core/matching_engine.hpp"
#include "core/order_book.hpp"
#include "core/fast_order_book.hpp"
#include "test_framework.hpp"

#include <random>
#include <vector>
#include <unordered_set>

using namespace lob;

static Order makeOrder(OrderId id, Side side, Price price, Quantity qty) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::Limit;
    o.price = price;
    o.qty = qty;
    o.remaining = qty;
    o.ts = id * 1000;
    return o;
}

// Compare two MatchResults for exact parity (except trade seq/tradeId which should also match)
static bool resultsEqual(const MatchResult& a, const MatchResult& b, std::string& reason) {
    if (a.status != b.status) { reason = "status mismatch"; return false; }
    if (a.filled != b.filled) { reason = "filled mismatch"; return false; }
    if (a.remaining != b.remaining) { reason = "remaining mismatch"; return false; }
    if (a.takerSeq != b.takerSeq) { reason = "takerSeq mismatch"; return false; }
    if (a.trades.size() != b.trades.size()) { reason = "trades size mismatch"; return false; }
    for (size_t i = 0; i < a.trades.size(); ++i) {
        const auto& ta = a.trades[i];
        const auto& tb = b.trades[i];
        if (ta.takerId != tb.takerId || ta.makerId != tb.makerId || ta.price != tb.price || ta.qty != tb.qty || ta.takerSide != tb.takerSide) {
            reason = "trade " + std::to_string(i) + " field mismatch";
            return false;
        }
        if (ta.tradeId != tb.tradeId) { reason = "tradeId mismatch"; return false; }
        if (ta.seq != tb.seq) { reason = "trade seq mismatch"; return false; }
    }
    if (a.bestBidBefore != b.bestBidBefore) { reason = "bestBidBefore mismatch"; return false; }
    if (a.bestAskBefore != b.bestAskBefore) { reason = "bestAskBefore mismatch"; return false; }
    if (a.bestBidAfter != b.bestBidAfter) { reason = "bestBidAfter mismatch"; return false; }
    if (a.bestAskAfter != b.bestAskAfter) { reason = "bestAskAfter mismatch"; return false; }
    if (a.reason != b.reason) { reason = "reason mismatch"; return false; }
    return true;
}

static bool booksEqual(const OrderBook& ob, const FastOrderBook& fb, std::string& reason) {
    if (ob.orderCount() != fb.orderCount()) { reason = "orderCount"; return false; }
    if (ob.levelCount(Side::Buy) != fb.levelCount(Side::Buy)) { reason = "buy levelCount"; return false; }
    if (ob.levelCount(Side::Sell) != fb.levelCount(Side::Sell)) { reason = "sell levelCount"; return false; }
    if (ob.bestBid() != fb.bestBid()) { reason = "bestBid"; return false; }
    if (ob.bestAsk() != fb.bestAsk()) { reason = "bestAsk"; return false; }
    // Compare all price levels present in OrderBook
    for (auto& kv : ob.bids()) {
        Price p = kv.first;
        Quantity oq = ob.totalQuantity(Side::Buy, p);
        Quantity fq = fb.totalQuantity(Side::Buy, p);
        if (oq != fq) { reason = "buy totalQty at " + std::to_string(p); return false; }
    }
    for (auto& kv : ob.asks()) {
        Price p = kv.first;
        Quantity oq = ob.totalQuantity(Side::Sell, p);
        Quantity fq = fb.totalQuantity(Side::Sell, p);
        if (oq != fq) { reason = "sell totalQty at " + std::to_string(p); return false; }
    }
    // Also check fb levels are subset of ob (no extra)
    // Iterate fb via forEachLevel
    bool extra = false;
    fb.forEachLevel(Side::Buy, [&](const FastPriceLevel* lvl){
        if (ob.totalQuantity(Side::Buy, lvl->price) == 0) extra = true;
    });
    if (extra) { reason = "fb has extra buy level"; return false; }
    fb.forEachLevel(Side::Sell, [&](const FastPriceLevel* lvl){
        if (ob.totalQuantity(Side::Sell, lvl->price) == 0) extra = true;
    });
    if (extra) { reason = "fb has extra sell level"; return false; }

    // Check each order's seq and remaining
    for (auto& kv : ob.bids()) {
        for (auto& ord : kv.second) {
            const Order* f = fb.findOrder(ord.id);
            if (!f) { reason = "missing buy order " + std::to_string(ord.id); return false; }
            if (f->remaining != ord.remaining) { reason = "remaining mismatch order " + std::to_string(ord.id); return false; }
            if (f->seq != ord.seq) { reason = "seq mismatch order " + std::to_string(ord.id); return false; }
            if (f->price != ord.price) { reason = "price mismatch order " + std::to_string(ord.id); return false; }
            if (f->side != ord.side) { reason = "side mismatch order " + std::to_string(ord.id); return false; }
        }
    }
    for (auto& kv : ob.asks()) {
        for (auto& ord : kv.second) {
            const Order* f = fb.findOrder(ord.id);
            if (!f) { reason = "missing sell order " + std::to_string(ord.id); return false; }
            if (f->remaining != ord.remaining) { reason = "remaining mismatch sell " + std::to_string(ord.id); return false; }
            if (f->seq != ord.seq) { reason = "seq mismatch sell " + std::to_string(ord.id); return false; }
        }
    }
    return true;
}

// Run a deterministic stream against both books and assert parity.
static void runParityStream(std::uint32_t seed, size_t steps, Price pmin, Price pmax,
                            const char* label) {
    OrderBook ob;
    FastOrderBook fb(pmin, pmax);
    fb.reserveOrders(8192);
    MatchingEngine<OrderBook> engOb(ob);
    MatchingEngine<FastOrderBook> engFb(fb);

    std::minstd_rand rng(seed);
    OrderId nextId = 1;
    std::vector<OrderId> liveIds;
    liveIds.reserve(1024);

    for (size_t i = 0; i < steps; ++i) {
        int op = rng() % 100;
        if (op < 15 && !liveIds.empty()) {
            // cancel random live order via book (both must agree)
            size_t idx = rng() % liveIds.size();
            OrderId cid = liveIds[idx];
            bool r1 = ob.cancelOrder(cid);
            bool r2 = fb.cancelOrder(cid);
            if (r1 != r2) {
                CHECK(false);
                // detail
                std::printf("  parity cancel mismatch step %zu id %llu: ob %d fb %d seed %u %s\n",
                    i, (unsigned long long)cid, (int)r1, (int)r2, seed, label);
                return;
            }
            if (r1) {
                liveIds[idx] = liveIds.back();
                liveIds.pop_back();
            }
            // book snapshots must still match (including after cancel)
            std::string reason;
            if (!booksEqual(ob, fb, reason)) {
                CHECK(false);
                std::printf("  parity book after cancel mismatch step %zu reason %s seed %u %s\n",
                    i, reason.c_str(), seed, label);
                return;
            }
        } else {
            // new order via engine
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = pmin + (rng() % (pmax - pmin + 1));
            Quantity qty = 1 + (rng() % 20);
            Order o = makeOrder(nextId, side, price, qty);
            // keep nextId increment even if rejected? Engine will handle duplicate etc.
            // For parity we use fresh ids, so no duplicate unless we intentionally test duplicate
            // Occasionally inject duplicate to test rejection parity
            if ((rng() % 100) < 3 && !liveIds.empty()) {
                // duplicate id attempt
                o.id = liveIds[rng() % liveIds.size()];
                // do not advance nextId
            } else {
                ++nextId;
            }

            auto rOb = engOb.processOrder(o);
            auto rFb = engFb.processOrder(o);

            std::string rreason;
            if (!resultsEqual(rOb, rFb, rreason)) {
                CHECK(false);
                std::printf("  parity result mismatch step %zu op id %llu side %s price %lld qty %llu reason %s seed %u %s\n",
                    i, (unsigned long long)o.id, o.side==Side::Buy?"buy":"sell", (long long)o.price, (unsigned long long)o.qty,
                    rreason.c_str(), seed, label);
                std::printf("    ob: %s\n", toString(rOb).c_str());
                std::printf("    fb: %s\n", toString(rFb).c_str());
                return;
            }

            // liveIds maintenance: if order resulted in resting (Resting or PartiallyFilled), it should be in book
            // if Filled or Rejected, not in book. For Resting/PartiallyFilled, add to liveIds unless duplicate
            if (rOb.status == ExecStatus::Resting || rOb.status == ExecStatus::PartiallyFilled) {
                // only if not duplicate (duplicate would be Rejected, not here)
                // check that book indeed contains it
                if (!ob.contains(o.id) || !fb.contains(o.id)) {
                    CHECK(false);
                    std::printf("  parity liveIds: resting order %llu not found in book step %zu seed %u %s\n",
                        (unsigned long long)o.id, i, seed, label);
                    return;
                }
                // avoid double insert of duplicate id that was rejected — already handled
                bool already = false;
                for (auto id : liveIds) if (id == o.id) already = true;
                if (!already) liveIds.push_back(o.id);
            } else if (rOb.status == ExecStatus::Filled) {
                // taker filled, makers may have been removed — need to sync liveIds for makers
                // Remove any maker ids that were fully filled (present in trades and now missing)
                for (auto& tr : rOb.trades) {
                    // maker id should no longer be in book if fully filled
                    // check if still present — then it's partially filled, keep it
                    if (!ob.contains(tr.makerId)) {
                        // remove from liveIds
                        for (size_t k = 0; k < liveIds.size(); ++k) {
                            if (liveIds[k] == tr.makerId) {
                                liveIds[k] = liveIds.back();
                                liveIds.pop_back();
                                break;
                            }
                        }
                    }
                }
            } else if (rOb.status == ExecStatus::Rejected) {
                // nothing to add, liveIds unchanged
            }

            // also need to keep liveIds in sync for any orders that were cancelled via engine? No cancellations here via engine, only via book cancel above.

            // book snapshots must match
            std::string breason;
            if (!booksEqual(ob, fb, breason)) {
                CHECK(false);
                std::printf("  parity book after order mismatch step %zu id %llu reason %s seed %u %s\n",
                    i, (unsigned long long)o.id, breason.c_str(), seed, label);
                // dump best
                auto obb = ob.bestBid(); auto oba = ob.bestAsk();
                auto fbb = fb.bestBid(); auto fba = fb.bestAsk();
                std::printf("    ob bestBid %s bestAsk %s | fb bestBid %s bestAsk %s\n",
                    obb.has_value()? std::to_string(*obb).c_str():"none",
                    oba.has_value()? std::to_string(*oba).c_str():"none",
                    fbb.has_value()? std::to_string(*fbb).c_str():"none",
                    fba.has_value()? std::to_string(*fba).c_str():"none");
                return;
            }

            // invariant: never crossed
            if (ob.bestBid().has_value() && ob.bestAsk().has_value()) {
                if (!(*ob.bestBid() < *ob.bestAsk())) {
                    CHECK(false);
                    std::printf("  book crossed after step %zu seed %u %s\n", i, seed, label);
                    return;
                }
            }
        }
    }
    // final check
    std::string freason;
    CHECK(booksEqual(ob, fb, freason));
    if (!freason.empty() && !booksEqual(ob, fb, freason)) {
        // already checked
    }
    // if we reach here, parity holds for this seed/stream
    CHECK(true);
}

TEST(parity_basic_small_random) {
    runParityStream(42, 500, 1, 1000, "basic_small");
}

TEST(parity_top_of_book_churn) {
    // Prices clustered at touch (99-101 buy, 102-104 sell) to stress best maintenance
    OrderBook ob;
    FastOrderBook fb(1, 1000);
    fb.reserveOrders(4096);
    MatchingEngine<OrderBook> engOb(ob);
    MatchingEngine<FastOrderBook> engFb(fb);
    std::minstd_rand rng(1234);
    OrderId nid = 1;
    for (int i = 0; i < 200; ++i) {
        // fill both sides near spread
        engOb.processOrder(makeOrder(nid, Side::Buy, 100, 10)); engFb.processOrder(makeOrder(nid, Side::Buy, 100, 10)); ++nid;
        engOb.processOrder(makeOrder(nid, Side::Sell, 102, 10)); engFb.processOrder(makeOrder(nid, Side::Sell, 102, 10)); ++nid;
    }
    CHECK(ob.bestBid().value() == 100);
    CHECK(fb.bestBid().value() == 100);
    // now churn top: repeatedly add and cancel at best, plus takers that cross by 1 tick
    for (int i = 0; i < 500; ++i) {
        int op = rng() % 3;
        if (op == 0) {
            Order o = makeOrder(nid, Side::Buy, 101, 1 + rng()%5);
            auto r1 = engOb.processOrder(o);
            auto r2 = engFb.processOrder(o);
            std::string rr; CHECK(resultsEqual(r1,r2,rr));
            ++nid;
        } else if (op == 1) {
            Order o = makeOrder(nid, Side::Sell, 101, 1 + rng()%5);
            auto r1 = engOb.processOrder(o);
            auto r2 = engFb.processOrder(o);
            std::string rr; CHECK(resultsEqual(r1,r2,rr));
            ++nid;
        } else {
            // cancel random best level order
            if (ob.orderCount() > 0) {
                // cancel best bid
                auto* lvl = ob.bestBidLevel();
                if (lvl && !lvl->empty()) {
                    OrderId cid = lvl->front().id;
                    bool c1 = ob.cancelOrder(cid);
                    bool c2 = fb.cancelOrder(cid);
                    CHECK(c1 == c2);
                }
            }
        }
        std::string br; CHECK(booksEqual(ob, fb, br));
        if (ob.bestBid().has_value() && ob.bestAsk().has_value()) {
            CHECK(*ob.bestBid() < *ob.bestAsk());
        }
    }
}

TEST(parity_one_price_flood) {
    // All orders at single price 100/101 to stress FIFO and level totals
    runParityStream(777, 1000, 100, 101, "one_price_flood");
}

TEST(parity_empty_spread_gaps) {
    // Prices far apart to create empty spread gaps, then sweep across gap
    OrderBook ob;
    FastOrderBook fb(1, 1000);
    MatchingEngine<OrderBook> engOb(ob);
    MatchingEngine<FastOrderBook> engFb(fb);
    // place bids at 90, asks at 110 (wide spread 20)
    for (int i = 0; i < 10; ++i) {
        engOb.processOrder(makeOrder(1+i, Side::Buy, 90, 10));
        engFb.processOrder(makeOrder(1+i, Side::Buy, 90, 10));
        engOb.processOrder(makeOrder(100+i, Side::Sell, 110, 10));
        engFb.processOrder(makeOrder(100+i, Side::Sell, 110, 10));
    }
    CHECK(ob.bestBid().value() == 90);
    CHECK(ob.bestAsk().value() == 110);
    // now aggressive orders that sweep across gap (price 110 buy should cross entire sell side)
    auto r1 = engOb.processOrder(makeOrder(1000, Side::Buy, 110, 50));
    auto r2 = engFb.processOrder(makeOrder(1000, Side::Buy, 110, 50));
    std::string rr; CHECK(resultsEqual(r1,r2,rr));
    CHECK(r1.trades.size() == 5); // 5 asks at 110 qty10 each, need 50
    CHECK(r1.filled == 50);
    CHECK(fb.bestAsk().has_value() && *fb.bestAsk() == 110);
    CHECK(fb.totalQuantity(Side::Sell, 110) == 50);
    CHECK(ob.totalQuantity(Side::Sell, 110) == 50);
    std::string br; CHECK(booksEqual(ob, fb, br));
}

TEST(parity_long_fuzz_5000) {
    runParityStream(0xC0FFEE, 5000, 1, 1000, "long_fuzz_5000");
}

TEST(parity_adversarial_interleaved_cancel_and_match) {
    runParityStream(99999, 2000, 95, 105, "adversarial_interleaved");
}
