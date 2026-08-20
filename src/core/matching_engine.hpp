#pragma once

// Phase 3C — Matching engine (limit-order core).
//
// One template `MatchingEngine<Book>` drives both OrderBook and
// FastOrderBook via the BookBackend concept + LevelTrait shim.  No virtual
// dispatch on the hot path; all trait helpers are inlined.
//
// Matching semantics (price-time priority, FIFO via level head):
//   - Validate/reject (duplicate id, zero qty, non-positive price)
//   - Assign seq (book.allocateSeq) and emit New/Rejected
//   - Taker buys sweep asks ascending (begin()), sells sweep bids descending
//     (rbegin() via bestBidLevel). Cross when taker buy price >= best ask
//     (and sell <= best bid); equality crosses.
//   - Loop over opposite best levels: front maker is oldest at that price.
//     Match min(taker.remaining, maker.remaining) at maker's price (never
//     price-improve). Produce a Trade, reduce() the maker, emit Trade +
//     maker OrderUpdate + BookTick, pop fully-filled makers via cancelOrder.
//   - Terminate when taker filled or no crossing; residual rests via addOrder
//     (FIFO tail, seq already assigned) or Filled.
//   - Invariants: book never left crossed; sum(trade qty) == taker moved;
//     every maker reduction exact; seq strictly increasing.
//
// Latency: steady-state zero allocations (trades vector in MatchResult is the
// only allocation on the outcome path; engine tick path itself allocates
// nothing). EventSink calls are through a single pointer; Null sink inlines
// to nothing.

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

    // Process one limit order through the book. The input `taker` is taken by
    // value so the engine can mutate remaining without affecting the caller.
    MatchResult processOrder(Order taker);

    [[nodiscard]] Book& book() noexcept { return book_; }
    [[nodiscard]] const Book& book() const noexcept { return book_; }

    // For tests/bench: inspect counters.
    [[nodiscard]] std::uint64_t nextTradeId() const noexcept { return next_trade_id_; }

private:
    [[nodiscard]] static bool crosses(Price takerPrice, Price makerPrice,
                                      Side takerSide) noexcept {
        if (takerSide == Side::Buy) {
            return takerPrice >= makerPrice;  // buy sweeps asks
        }
        return takerPrice <= makerPrice;      // sell sweeps bids
    }

    Book& book_;
    IEventSink* sink_;
    NullEventSink null_sink_;
    std::uint64_t next_trade_id_ = 1;
    SeqNo next_trade_seq_ = 1;
};

// ---------------------------------------------------------------------------
// Implementation — header-inline so the hot path inlines per Book
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
    } else if (taker.price <= 0) {
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

    // ---------- seq assignment (valid orders only) ----------
    const SeqNo takerSeq = book_.allocateSeq();
    taker.seq = takerSeq;
    result.takerSeq = takerSeq;
    if (taker.remaining == 0) {
        taker.remaining = taker.qty;
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

    // ---------- match loop ----------
    while (remaining > 0) {
        auto* level = oppositeBestLevel(book_, taker.side);
        if (LevelTrait<Book>::empty(level)) {
            break;
        }
        const Price makerPrice = LevelTrait<Book>::price(level);
        if (!crosses(taker.price, makerPrice, taker.side)) {
            break;
        }

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
        tr.price = makerPriceCopy;  // maker's price (no price improvement beyond maker)
        tr.qty = tradeQty;
        tr.ts = taker.ts;
        result.trades.push_back(tr);
        result.filled += tradeQty;
        sink_->onTrade(tr);

        // Capture totals before mutation for tick logic
        // (for OrderBook the level object stays, for Fast we will recompute via book_.totalQuantity)
        if (tradeQty == makerRemainingBefore) {
            // Fully filled maker — reduce to zero then remove via book
            LevelTrait<Book>::reduce(level, handle, tradeQty);

            ExecutionReport makerRep;
            makerRep.orderId = makerId;
            makerRep.side = makerSide;
            makerRep.type = OrderType::Limit;
            makerRep.price = makerPriceCopy;
            makerRep.qty = makerQty;
            makerRep.filled = makerQty;  // fully filled (original qty)
            makerRep.remaining = 0;
            makerRep.status = ExecStatus::Filled;
            makerRep.seq = makerSeq;
            sink_->onOrderUpdate(makerRep);

            // Remove the maker from the book (prunes level if empty)
            const bool ok = book_.cancelOrder(makerId);
            (void)ok;
            assert(ok && "maker must exist");

            const Quantity afterTotal = book_.totalQuantity(makerSide, makerPriceCopy);
            const bool removed = (afterTotal == 0);
            BookTick tick;
            tick.side = makerSide;
            tick.price = makerPriceCopy;
            tick.totalQuantity = afterTotal;
            tick.removed = removed;
            tick.isBest = true;  // we always consumed the best level
            sink_->onBookTick(tick);
        } else {
            // Partially filled maker — stays in book
            LevelTrait<Book>::reduce(level, handle, tradeQty);

            const Quantity makerRemainingAfter = LevelTrait<Book>::getOrder(handle).remaining;
            ExecutionReport makerRep;
            makerRep.orderId = makerId;
            makerRep.side = makerSide;
            makerRep.type = OrderType::Limit;
            makerRep.price = makerPriceCopy;
            makerRep.qty = makerQty;
            makerRep.filled = makerQty - makerRemainingAfter;
            makerRep.remaining = makerRemainingAfter;
            makerRep.status = ExecStatus::PartiallyFilled;
            makerRep.seq = makerSeq;
            sink_->onOrderUpdate(makerRep);

            const Quantity afterTotal = LevelTrait<Book>::totalQuantity(level);
            BookTick tick;
            tick.side = makerSide;
            tick.price = makerPriceCopy;
            tick.totalQuantity = afterTotal;
            tick.removed = false;
            tick.isBest = true;
            sink_->onBookTick(tick);
        }

        remaining -= tradeQty;
    }

    // ---------- residual handling ----------
    if (remaining == 0) {
        // Fully filled — no resting order
        result.status = ExecStatus::Filled;
        result.remaining = 0;
        ExecutionReport takerRep;
        takerRep.orderId = taker.id;
        takerRep.side = taker.side;
        takerRep.type = taker.type;
        takerRep.price = taker.price;
        takerRep.qty = taker.qty;
        takerRep.filled = taker.qty;
        takerRep.remaining = 0;
        takerRep.status = ExecStatus::Filled;
        takerRep.seq = takerSeq;
        sink_->onOrderUpdate(takerRep);
    } else if (result.trades.empty()) {
        // No cross — entire order rests
        Order resting = taker;
        resting.remaining = remaining;
        resting.seq = takerSeq;  // preserve engine-assigned seq
        resting.status = OrderStatus::New;
        const bool added = book_.addOrder(resting);
        // If domain rejected (Fast), treat as Rejected and roll back? For now assert.
        // In production we would not have taken trades (none), so rejection is clean.
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
            ExecutionReport restRep;
            restRep.orderId = taker.id;
            restRep.side = taker.side;
            restRep.type = taker.type;
            restRep.price = taker.price;
            restRep.qty = taker.qty;
            restRep.filled = taker.qty - remaining;
            restRep.remaining = remaining;
            restRep.status = ExecStatus::Resting;
            restRep.seq = takerSeq;
            sink_->onOrderUpdate(restRep);

            const Quantity afterTotal = book_.totalQuantity(taker.side, taker.price);
            const auto bestForSide = book_.bestPrice(taker.side);
            BookTick tick;
            tick.side = taker.side;
            tick.price = taker.price;
            tick.totalQuantity = afterTotal;
            tick.removed = false;
            tick.isBest = bestForSide.has_value() && *bestForSide == taker.price;
            sink_->onBookTick(tick);
        }
    } else {
        // Partially filled, residual rests
        Order resting = taker;
        resting.remaining = remaining;
        resting.seq = takerSeq;
        resting.status = OrderStatus::PartiallyFilled;
        const bool added = book_.addOrder(resting);
        if (!added) {
            // Should not happen for in-domain prices; if it does, we have already
            // emitted trades — the atomicity contract says FOK would prevent this,
            // but for plain Limit we surface the error and leave trades as-is.
            result.status = ExecStatus::PartiallyFilled;
            result.remaining = remaining;
            result.reason = "price_out_of_domain_on_rest";
            ExecutionReport pf;
            pf.orderId = taker.id;
            pf.side = taker.side;
            pf.type = taker.type;
            pf.price = taker.price;
            pf.qty = taker.qty;
            pf.filled = result.filled;
            pf.remaining = remaining;
            pf.status = ExecStatus::PartiallyFilled;
            pf.seq = takerSeq;
            pf.reason = "price_out_of_domain_on_rest";
            sink_->onOrderUpdate(pf);
        } else {
            result.status = ExecStatus::PartiallyFilled;
            result.remaining = remaining;
            ExecutionReport pf;
            pf.orderId = taker.id;
            pf.side = taker.side;
            pf.type = taker.type;
            pf.price = taker.price;
            pf.qty = taker.qty;
            pf.filled = result.filled;
            pf.remaining = remaining;
            pf.status = ExecStatus::PartiallyFilled;
            pf.seq = takerSeq;
            sink_->onOrderUpdate(pf);

            const Quantity afterTotal = book_.totalQuantity(taker.side, taker.price);
            const auto bestForSide = book_.bestPrice(taker.side);
            BookTick tick;
            tick.side = taker.side;
            tick.price = taker.price;
            tick.totalQuantity = afterTotal;
            tick.removed = false;
            tick.isBest = bestForSide.has_value() && *bestForSide == taker.price;
            sink_->onBookTick(tick);
        }
    }

    result.bestBidAfter = book_.bestBid();
    result.bestAskAfter = book_.bestAsk();

    // Invariant: book never left crossed
    if (result.bestBidAfter.has_value() && result.bestAskAfter.has_value()) {
        assert(*result.bestBidAfter < *result.bestAskAfter && "book left crossed after match");
    }

    return result;
}

}  // namespace lob
