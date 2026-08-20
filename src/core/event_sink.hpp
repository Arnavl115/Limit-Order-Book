#pragma once

// Phase 3B — Event sink interface for the MatchingEngine.
//
// The engine emits typed callbacks without knowing who is listening. The
// gateway (Phase 4) will implement a sink that serialises them to JSON and
// broadcasts them; benchmarks and tests use a counting sink.
//
// Latency requirement: the hot path (MatchingEngine::processOrder) takes an
// `IEventSink&` and calls through it once per interesting event. A null sink
// (NullEventSink) inlines to zero cost when no listener is registered.

#include "match_types.hpp"

namespace lob {

struct IEventSink {
    virtual ~IEventSink() = default;

    // One trade produced by crossing a taker with a maker. Called for every
    // individual maker fill, in seq order.
    virtual void onTrade(const Trade& /*trade*/) {}

    // Per-order lifecycle update. Called at least once per `processOrder`
    // (New/Rejected immediately, then PartiallyFilled/Filled/Resting as the
    // engine steps through the book). Multiple calls per order are possible
    // (e.g. New -> PartiallyFilled -> Filled).
    virtual void onOrderUpdate(const ExecutionReport& /*report*/) {}

    // Market-data tick: a price level's total changed or was pruned.
    // The gateway maps these to `marketdata.tick` messages.
    virtual void onBookTick(const BookTick& /*tick*/) {}
};

// Zero-overhead default. All methods inline to empty bodies; the call
// site can be devirtualized when the concrete type is known.
struct NullEventSink final : IEventSink {
    void onTrade(const Trade&) final {}
    void onOrderUpdate(const ExecutionReport&) final {}
    void onBookTick(const BookTick&) final {}
};

// Testing helper: counts invocations and stores the last payloads.
struct CountingEventSink final : IEventSink {
    std::size_t tradeCount = 0;
    std::size_t orderUpdateCount = 0;
    std::size_t bookTickCount = 0;

    Trade lastTrade{};
    ExecutionReport lastReport{};
    BookTick lastTick{};

    std::vector<Trade> trades;
    std::vector<ExecutionReport> reports;
    std::vector<BookTick> ticks;

    void onTrade(const Trade& t) final {
        ++tradeCount;
        lastTrade = t;
        trades.push_back(t);
    }
    void onOrderUpdate(const ExecutionReport& r) final {
        ++orderUpdateCount;
        lastReport = r;
        reports.push_back(r);
    }
    void onBookTick(const BookTick& tk) final {
        ++bookTickCount;
        lastTick = tk;
        ticks.push_back(tk);
    }

    void clear() noexcept {
        tradeCount = orderUpdateCount = bookTickCount = 0;
        trades.clear();
        reports.clear();
        ticks.clear();
        lastTrade = Trade{};
        lastReport = ExecutionReport{};
        lastTick = BookTick{};
    }
};

}  // namespace lob
