#pragma once

// Phase 3C-3E — Matching engine.
//
// One template `MatchingEngine<Book>` drives both OrderBook and
// FastOrderBook via BookBackend + LevelTrait. No virtual dispatch on hot path.
//
// Semantics
//   Limit (GTC): price-time priority, FIFO per level, crosses when
//                buy price >= best ask (sell <= best bid). Residual rests.
//   Market:      no price limit, sweeps until filled or book empty. Never rests.
//                If no trades, Rejected (no_liquidity); else Filled (partial
//                fill is also Filled, residual discarded — market cannot rest).
//   IOC:         like Limit but residual is cancelled, never rests. Status
//                Filled if fully filled, Cancelled otherwise (whether or not
//                any fill occurred).
//   FOK:         all-or-nothing. Compute total crossable qty before any trade;
//                if insufficient, reject atomically with no trades/book change.
//                Otherwise sweep like Limit and must fully fill (no residual).
//   Modify:      cancel + re-add as new order with same id, re-queued at tail
//                (loses time priority). Preserves id, new price/qty, new seq.
//
// Invariants: book never left crossed; sum(trade qty)==filled; seq strictly
// increasing; FOK atomicity; Market/IOC never leave residual resting.

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <vector>

#include "book_backend.hpp"
#include "event_sink.hpp"
#include "match_types.hpp"
#include "order.hpp"

namespace lob {

template <BookBackend Book>
class MatchingEngine {
public:
    explicit MatchingEngine(Book& book, IEventSink* sink = nullptr) noexcept
        : book_(book), sink_(sink ? sink : &null_sink_) {}

    MatchResult processOrder(Order taker);

    // Engine-level cancel that emits OrderUpdate + BookTick. Returns false
    // when id unknown.
    bool cancelOrder(OrderId id);

    // Modify resting order: cancel + re-add with new price/qty (same side/type).
    // Re-queued at tail (new seq). If the modified order would cross, it will
    // match immediately via processOrder. Returns false when id unknown or
    // modified order is rejected.
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQty);

    // Replace order with an entirely new Order (must carry same id). Cancel
    // + process. Returns MatchResult of the replacement.
    MatchResult replaceOrder(const Order& newOrder);

    [[nodiscard]] Book& book() noexcept { return book_; }
    [[nodiscard]] const Book& book() const noexcept { return book_; }
    [[nodiscard]] std::uint64_t nextTradeId() const noexcept { return next_trade_id_; }

private:
    [[nodiscard]] static bool crosses(Price takerPrice, Price makerPrice,
                                      Side takerSide) noexcept {
        if (takerSide == Side::Buy) return takerPrice >= makerPrice;
        return takerPrice <= makerPrice;
    }

    // Total crossable qty on opposite side at prices that taker would cross.
    // Used for FOK atomicity check. Returns sum of remaining qty at crossing levels.
    [[nodiscard]] Quantity totalCrossableQty(Price takerPrice, Side takerSide) const;

    Book& book_;
    IEventSink* sink_;
    NullEventSink null_sink_;
    std::uint64_t next_trade_id_ = 1;
    SeqNo next_trade_seq_ = 1;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <BookBackend Book>
Quantity MatchingEngine<Book>::totalCrossableQty(Price takerPrice, Side takerSide) const {
    Quantity sum = 0;
    if constexpr (std::is_same_v<Book, OrderBook>) {
        const Side opp = (takerSide == Side::Buy) ? Side::Sell : Side::Buy;
        const auto& sideMap = (opp == Side::Buy) ? book_.bids() : book_.asks();
        if (takerSide == Side::Buy) {
            // opposite is asks: ascending
            for (auto it = sideMap.begin(); it != sideMap.end(); ++it) {
                if (!crosses(takerPrice, it->first, takerSide)) break;
                sum += it->second.totalQuantity();
            }
        } else {
            // opposite is bids: descending (rbegin)
            for (auto it = sideMap.rbegin(); it != sideMap.rend(); ++it) {
                if (!crosses(takerPrice, it->first, takerSide)) break;
                sum += it->second.totalQuantity();
            }
        }
    } else {
        // FastOrderBook — forEachLevel visits best-first, which is sweep order
        const Side opp = (takerSide == Side::Buy) ? Side::Sell : Side::Buy;
        bool stop = false;
        book_.forEachLevel(opp, [&](const FastPriceLevel* lvl) {
            if (stop) return;
            if (!crosses(takerPrice, lvl->price, takerSide)) {
                stop = true;
                return;
            }
            sum += lvl->totalQuantity();
        });
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Engine-level cancel
// ---------------------------------------------------------------------------

template <BookBackend Book>
bool MatchingEngine<Book>::cancelOrder(OrderId id) {
    const Order* ord = book_.findOrder(id);
    if (ord == nullptr) return false;
    const Side side = ord->side;
    const Price price = ord->price;
    const Quantity qty = ord->qty;
    const SeqNo seq = ord->seq;
    const Quantity filled = ord->filled();
    const auto bestBefore = book_.bestPrice(side);
    const bool wasBest = bestBefore.has_value() && *bestBefore == price;

    const bool ok = book_.cancelOrder(id);
    if (!ok) return false;

    ExecutionReport rep;
    rep.orderId = id;
    rep.side = side;
    rep.type = ord->type;
    rep.price = price;
    rep.qty = qty;
    rep.filled = filled;
    rep.remaining = 0;
    rep.status = ExecStatus::Cancelled;
    rep.seq = seq;
    sink_->onOrderUpdate(rep);

    const Quantity after = book_.totalQuantity(side, price);
    BookTick tick;
    tick.side = side;
    tick.price = price;
    tick.totalQuantity = after;
    tick.removed = (after == 0);
    tick.isBest = wasBest;
    sink_->onBookTick(tick);
    return true;
}

template <BookBackend Book>
bool MatchingEngine<Book>::modifyOrder(OrderId id, Price newPrice, Quantity newQty) {
    const Order* existing = book_.findOrder(id);
    if (existing == nullptr) return false;
    const Side side = existing->side;
    const OrderType type = existing->type;
    const Timestamp ts = existing->ts;
    // Cancel existing (emits Cancelled)
    if (!cancelOrder(id)) return false;
    Order neo;
    neo.id = id;
    neo.side = side;
    neo.type = type;
    neo.price = newPrice;
    neo.qty = newQty;
    neo.remaining = newQty;
    neo.ts = ts;
    neo.seq = 0; // will be allocated by processOrder
    MatchResult r = processOrder(neo);
    return r.status != ExecStatus::Rejected;
}

template <BookBackend Book>
MatchResult MatchingEngine<Book>::replaceOrder(const Order& newOrder) {
    const Order* existing = book_.findOrder(newOrder.id);
    MatchResult rejected;
    rejected.takerId = newOrder.id;
    rejected.takerSide = newOrder.side;
    rejected.status = ExecStatus::Rejected;
    rejected.reason = "unknown_id";
    if (existing == nullptr) {
        // No existing order to replace — reject
        ExecutionReport rep;
        rep.orderId = newOrder.id;
        rep.side = newOrder.side;
        rep.type = newOrder.type;
        rep.price = newOrder.price;
        rep.qty = newOrder.qty;
        rep.status = ExecStatus::Rejected;
        rep.reason = "unknown_id";
        sink_->onOrderUpdate(rep);
        return rejected;
    }
    // Cancel old
    cancelOrder(newOrder.id);
    // Process new (same id, may have different side/type/price/qty)
    Order neo = newOrder;
    neo.seq = 0;
    neo.remaining = (neo.remaining == 0 ? neo.qty : neo.remaining);
    return processOrder(neo);
}

// ---------------------------------------------------------------------------
// Main entry: processOrder
// ---------------------------------------------------------------------------

template <BookBackend Book>
MatchResult MatchingEngine<Book>::processOrder(Order taker) {
    MatchResult result;
    result.takerId = taker.id;
    result.takerSide = taker.side;
    result.bestBidBefore = book_.bestBid();
    result.bestAskBefore = book_.bestAsk();

    // ---------- validation (no seq consumed on reject) ----------
    const char* rejectReason = nullptr;
    if (taker.qty == 0 || taker.remaining == 0) {
        rejectReason = "zero_qty";
    } else if (taker.type != OrderType::Market && taker.price <= 0) {
        rejectReason = "invalid_price";
    } else if (book_.contains(taker.id)) {
        rejectReason = "duplicate_id";
    }

    if (rejectReason != nullptr) {
        result.status = ExecStatus::Rejected;
        result.reason = rejectReason;
        result.remaining = taker.remaining;
        result.bestBidAfter = book_.bestBid();
        result.bestAskAfter = book_.bestAsk();
        ExecutionReport rep;
        rep.orderId = taker.id;
        rep.side = taker.side;
        rep.type = taker.type;
        rep.price = taker.price;
        rep.qty = taker.qty;
        rep.filled = 0;
        rep.remaining = taker.remaining;
        rep.status = ExecStatus::Rejected;
        rep.seq = 0;
        rep.reason = rejectReason;
        sink_->onOrderUpdate(rep);
        return result;
    }

    // FOK atomicity pre-check (before seq assignment? FOK that fails should
    // still consume seq per spec's strictly-increasing invariant? We consume
    // seq only if we go ahead; failed FOK before matching should not consume
    // seq, matching the validation path. But if we allocate seq before check,
    // seq would be consumed even on FOK reject. For strict monotonicity across
    // all orders that enter the engine, failed FOK could be considered an order
    // that was seen, so seq could be consumed. We choose to allocate before the
    // FOK check so that seq always advances for every processOrder call that
    // passes basic validation, matching the Limit path.
    const SeqNo takerSeq = book_.allocateSeq();
    taker.seq = takerSeq;
    result.takerSeq = takerSeq;
    if (taker.remaining == 0) taker.remaining = taker.qty;

    // FOK: check total crossable depth atomically, before any matching
    if (taker.type == OrderType::FOK) {
        const Quantity need = taker.remaining;
        const Quantity avail = totalCrossableQty(taker.price, taker.side);
        if (avail < need) {
            // Emit New then Rejected atomically, no trades, no book change
            ExecutionReport newRep;
            newRep.orderId = taker.id;
            newRep.side = taker.side;
            newRep.type = taker.type;
            newRep.price = taker.price;
            newRep.qty = taker.qty;
            newRep.filled = 0;
            newRep.remaining = taker.remaining;
            newRep.status = ExecStatus::New;
            newRep.seq = takerSeq;
            sink_->onOrderUpdate(newRep);

            result.status = ExecStatus::Rejected;
            result.reason = "fok_insufficient_liquidity";
            result.remaining = taker.remaining;
            result.bestBidAfter = book_.bestBid();
            result.bestAskAfter = book_.bestAsk();

            ExecutionReport rej;
            rej.orderId = taker.id;
            rej.side = taker.side;
            rej.type = taker.type;
            rej.price = taker.price;
            rej.qty = taker.qty;
            rej.filled = 0;
            rej.remaining = taker.remaining;
            rej.status = ExecStatus::Rejected;
            rej.seq = takerSeq;
            rej.reason = "fok_insufficient_liquidity";
            sink_->onOrderUpdate(rej);
            return result;
        }
        // else fall through to normal matching loop — guaranteed to fully fill
    }

    ExecutionReport newRep;
    newRep.orderId = taker.id;
    newRep.side = taker.side;
    newRep.type = taker.type;
    newRep.price = taker.price;
    newRep.qty = taker.qty;
    newRep.filled = 0;
    newRep.remaining = taker.remaining;
    newRep.status = ExecStatus::New;
    newRep.seq = takerSeq;
    sink_->onOrderUpdate(newRep);

    Quantity remaining = taker.remaining;
    const bool isMarket = (taker.type == OrderType::Market);

    // ---------- match loop ----------
    while (remaining > 0) {
        auto* level = oppositeBestLevel(book_, taker.side);
        if (LevelTrait<Book>::empty(level)) break;
        const Price makerPrice = LevelTrait<Book>::price(level);
        if (!isMarket && !crosses(taker.price, makerPrice, taker.side)) break;

        auto handle = LevelTrait<Book>::front(level);
        Order& maker = LevelTrait<Book>::getOrder(handle);
        const OrderId makerId = maker.id;
        const Side makerSide = maker.side;
        const Price makerPriceCopy = maker.price;
        const Quantity makerQty = maker.qty;
        const SeqNo makerSeq = maker.seq;
        const Quantity makerRemainingBefore = maker.remaining;
        const Quantity tradeQty = (remaining < makerRemainingBefore) ? remaining : makerRemainingBefore;
        assert(tradeQty > 0);

        Trade tr;
        tr.tradeId = next_trade_id_++;
        tr.seq = next_trade_seq_++;
        tr.takerId = taker.id;
        tr.makerId = makerId;
        tr.takerSide = taker.side;
        tr.price = makerPriceCopy;
        tr.qty = tradeQty;
        tr.ts = taker.ts;
        result.trades.push_back(tr);
        result.filled += tradeQty;
        sink_->onTrade(tr);

        if (tradeQty == makerRemainingBefore) {
            LevelTrait<Book>::reduce(level, handle, tradeQty);
            ExecutionReport makerRep;
            makerRep.orderId = makerId;
            makerRep.side = makerSide;
            makerRep.type = OrderType::Limit;
            makerRep.price = makerPriceCopy;
            makerRep.qty = makerQty;
            makerRep.filled = makerQty;
            makerRep.remaining = 0;
            makerRep.status = ExecStatus::Filled;
            makerRep.seq = makerSeq;
            sink_->onOrderUpdate(makerRep);
            const bool ok = book_.cancelOrder(makerId);
            (void)ok; assert(ok);
            const Quantity after = book_.totalQuantity(makerSide, makerPriceCopy);
            BookTick tick;
            tick.side = makerSide;
            tick.price = makerPriceCopy;
            tick.totalQuantity = after;
            tick.removed = (after == 0);
            tick.isBest = true;
            sink_->onBookTick(tick);
        } else {
            LevelTrait<Book>::reduce(level, handle, tradeQty);
            const Quantity makerAfter = LevelTrait<Book>::getOrder(handle).remaining;
            ExecutionReport makerRep;
            makerRep.orderId = makerId;
            makerRep.side = makerSide;
            makerRep.type = OrderType::Limit;
            makerRep.price = makerPriceCopy;
            makerRep.qty = makerQty;
            makerRep.filled = makerQty - makerAfter;
            makerRep.remaining = makerAfter;
            makerRep.status = ExecStatus::PartiallyFilled;
            makerRep.seq = makerSeq;
            sink_->onOrderUpdate(makerRep);
            const Quantity after = LevelTrait<Book>::totalQuantity(level);
            BookTick tick;
            tick.side = makerSide;
            tick.price = makerPriceCopy;
            tick.totalQuantity = after;
            tick.removed = false;
            tick.isBest = true;
            sink_->onBookTick(tick);
        }
        remaining -= tradeQty;
    }

    // ---------- residual handling per order type ----------
    if (taker.type == OrderType::Market) {
        if (result.trades.empty()) {
            result.status = ExecStatus::Rejected;
            result.reason = "no_liquidity";
            result.remaining = remaining;
            ExecutionReport rej;
            rej.orderId = taker.id;
            rej.side = taker.side;
            rej.type = taker.type;
            rej.price = taker.price;
            rej.qty = taker.qty;
            rej.filled = 0;
            rej.remaining = remaining;
            rej.status = ExecStatus::Rejected;
            rej.seq = takerSeq;
            rej.reason = "no_liquidity";
            sink_->onOrderUpdate(rej);
        } else if (remaining == 0) {
            result.status = ExecStatus::Filled;
            result.remaining = 0;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = 0;
            rep.status = ExecStatus::Filled;
            rep.seq = takerSeq;
            sink_->onOrderUpdate(rep);
        } else {
            // Partial fill, book empty — market residual discarded
            result.status = ExecStatus::Filled;
            result.remaining = 0; // discarded
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = 0;
            rep.status = ExecStatus::Filled;
            rep.seq = takerSeq;
            sink_->onOrderUpdate(rep);
        }
    } else if (taker.type == OrderType::IOC) {
        if (remaining == 0) {
            result.status = ExecStatus::Filled;
            result.remaining = 0;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = 0;
            rep.status = ExecStatus::Filled;
            rep.seq = takerSeq;
            sink_->onOrderUpdate(rep);
        } else {
            // Residual cancelled, whether or not any fill occurred
            result.status = ExecStatus::Cancelled;
            result.remaining = remaining;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = remaining;
            rep.status = ExecStatus::Cancelled;
            rep.seq = takerSeq;
            // reason distinguish partial vs no fill
            if (result.trades.empty()) rep.reason = "ioc_no_fill";
            sink_->onOrderUpdate(rep);
        }
    } else if (taker.type == OrderType::FOK) {
        // FOK already guaranteed to fully fill if we reached here (atomic check passed)
        // So remaining must be 0; if not, something went wrong (should be Rejected earlier)
        if (remaining == 0) {
            result.status = ExecStatus::Filled;
            result.remaining = 0;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = 0;
            rep.status = ExecStatus::Filled;
            rep.seq = takerSeq;
            sink_->onOrderUpdate(rep);
        } else {
            // Should not happen because we checked avail, but if it does (e.g. concurrent cancel), treat as Rejected and rollback? We have already emitted trades — violates atomicity.
            // For this single-threaded engine, this path is unreachable. Mark as Rejected.
            result.status = ExecStatus::Rejected;
            result.reason = "fok_partial_unexpected";
            result.remaining = remaining;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = result.filled;
            rep.remaining = remaining;
            rep.status = ExecStatus::Rejected;
            rep.seq = takerSeq;
            rep.reason = "fok_partial_unexpected";
            sink_->onOrderUpdate(rep);
        }
    } else { // Limit (GTC)
        if (remaining == 0) {
            result.status = ExecStatus::Filled;
            result.remaining = 0;
            ExecutionReport rep;
            rep.orderId = taker.id;
            rep.side = taker.side;
            rep.type = taker.type;
            rep.price = taker.price;
            rep.qty = taker.qty;
            rep.filled = taker.qty;
            rep.remaining = 0;
            rep.status = ExecStatus::Filled;
            rep.seq = takerSeq;
            sink_->onOrderUpdate(rep);
        } else if (result.trades.empty()) {
            Order resting = taker;
            resting.remaining = remaining;
            resting.seq = takerSeq;
            resting.status = OrderStatus::New;
            const bool added = book_.addOrder(resting);
            if (!added) {
                result.status = ExecStatus::Rejected;
                result.reason = "price_out_of_domain";
                result.remaining = remaining;
                ExecutionReport rej;
                rej.orderId = taker.id;
                rej.side = taker.side;
                rej.type = taker.type;
                rej.price = taker.price;
                rej.qty = taker.qty;
                rej.filled = 0;
                rej.remaining = remaining;
                rej.status = ExecStatus::Rejected;
                rej.seq = takerSeq;
                rej.reason = "price_out_of_domain";
                sink_->onOrderUpdate(rej);
            } else {
                result.status = ExecStatus::Resting;
                result.remaining = remaining;
                ExecutionReport rep;
                rep.orderId = taker.id;
                rep.side = taker.side;
                rep.type = taker.type;
                rep.price = taker.price;
                rep.qty = taker.qty;
                rep.filled = taker.qty - remaining;
                rep.remaining = remaining;
                rep.status = ExecStatus::Resting;
                rep.seq = takerSeq;
                sink_->onOrderUpdate(rep);
                const Quantity after = book_.totalQuantity(taker.side, taker.price);
                const auto best = book_.bestPrice(taker.side);
                BookTick tick;
                tick.side = taker.side;
                tick.price = taker.price;
                tick.totalQuantity = after;
                tick.removed = false;
                tick.isBest = best.has_value() && *best == taker.price;
                sink_->onBookTick(tick);
            }
        } else {
            Order resting = taker;
            resting.remaining = remaining;
            resting.seq = takerSeq;
            resting.status = OrderStatus::PartiallyFilled;
            const bool added = book_.addOrder(resting);
            if (!added) {
                result.status = ExecStatus::PartiallyFilled;
                result.remaining = remaining;
                result.reason = "price_out_of_domain_on_rest";
                ExecutionReport rep;
                rep.orderId = taker.id;
                rep.side = taker.side;
                rep.type = taker.type;
                rep.price = taker.price;
                rep.qty = taker.qty;
                rep.filled = result.filled;
                rep.remaining = remaining;
                rep.status = ExecStatus::PartiallyFilled;
                rep.seq = takerSeq;
                rep.reason = "price_out_of_domain_on_rest";
                sink_->onOrderUpdate(rep);
            } else {
                result.status = ExecStatus::PartiallyFilled;
                result.remaining = remaining;
                ExecutionReport rep;
                rep.orderId = taker.id;
                rep.side = taker.side;
                rep.type = taker.type;
                rep.price = taker.price;
                rep.qty = taker.qty;
                rep.filled = result.filled;
                rep.remaining = remaining;
                rep.status = ExecStatus::PartiallyFilled;
                rep.seq = takerSeq;
                sink_->onOrderUpdate(rep);
                const Quantity after = book_.totalQuantity(taker.side, taker.price);
                const auto best = book_.bestPrice(taker.side);
                BookTick tick;
                tick.side = taker.side;
                tick.price = taker.price;
                tick.totalQuantity = after;
                tick.removed = false;
                tick.isBest = best.has_value() && *best == taker.price;
                sink_->onBookTick(tick);
            }
        }
    }

    result.bestBidAfter = book_.bestBid();
    result.bestAskAfter = book_.bestAsk();
    if (result.bestBidAfter.has_value() && result.bestAskAfter.has_value()) {
        assert(*result.bestBidAfter < *result.bestAskAfter && "book left crossed");
    }
    return result;
}

}  // namespace lob
