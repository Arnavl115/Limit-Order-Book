#pragma once

// Phase 3B — Match result types + event contract.
//
// The engine's outputs. The API layer (Phase 4) consumes these without
// touching the book directly, so they are the gateway contract.
//
// Design
//   Trade             — one maker/taker match at the maker's price.
//   ExecutionReport   — per-order lifecycle event (New → PartiallyFilled →
//                       Filled / Cancelled / Rejected / Resting).
//   MatchResult       — summary of one `processOrder` call (all trades +
//                       residual state + best bid/ask before/after).
//   BookTick          — level emptied / best moved; the gateway's
//                       market-data tick.
//
// All prices are integer ticks, seq is the authoritative time-priority
// tie-breaker, and every qty is integer.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Trade — one fill between a taker and a maker at the maker's price
// ---------------------------------------------------------------------------

struct Trade {
    std::uint64_t tradeId = 0;   // monotonic trade identifier
    SeqNo seq = 0;               // engine seq at trade time (strictly increasing)
    OrderId takerId = 0;
    OrderId makerId = 0;
    Side takerSide = Side::Buy;  // side of the aggressor
    Price price = 0;             // execution price (maker's resting price)
    Quantity qty = 0;            // matched quantity (>0)
    Timestamp ts = 0;            // wall-clock ns (informational)
};

[[nodiscard]] inline std::string toString(const Trade& t) {
    // Cheap, allocation-friendly formatter for logs / JSON. Not locale-sensitive.
    return std::string("Trade{tradeId=") + std::to_string(t.tradeId) +
           " seq=" + std::to_string(t.seq) +
           " taker=" + std::to_string(t.takerId) +
           " maker=" + std::to_string(t.makerId) +
           " side=" + std::string(toString(t.takerSide)) +
           " price=" + std::to_string(t.price) +
           " qty=" + std::to_string(t.qty) + "}";
}

// ---------------------------------------------------------------------------
// ExecutionReport — per-order callback payload for EventSink::onOrderUpdate
// ---------------------------------------------------------------------------

enum class ExecStatus : std::uint8_t {
    New,               // accepted, not yet matched
    PartiallyFilled,   // partially matched, residual remains
    Filled,            // fully matched (or reduced to zero)
    Cancelled,         // resting residual cancelled (IOC etc.)
    Rejected,          // never entered the book (validation failure)
    Resting,           // residual now resting on the book at a new price level
};

constexpr const char* toString(ExecStatus s) noexcept {
    switch (s) {
        case ExecStatus::New:             return "new";
        case ExecStatus::PartiallyFilled: return "partially_filled";
        case ExecStatus::Filled:           return "filled";
        case ExecStatus::Cancelled:        return "cancelled";
        case ExecStatus::Rejected:         return "rejected";
        case ExecStatus::Resting:          return "resting";
    }
    return "unknown";
}

struct ExecutionReport {
    OrderId orderId = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;
    Quantity qty = 0;          // original qty
    Quantity filled = 0;       // qty - remaining at report time
    Quantity remaining = 0;    // qty still open
    ExecStatus status = ExecStatus::New;
    SeqNo seq = 0;             // assigned seq (0 when Rejected before assignment)
    std::string reason;        // non-empty when Rejected (e.g. "duplicate_id")
};

[[nodiscard]] inline std::string toString(const ExecutionReport& r) {
    std::string s = std::string("Exec{order=") + std::to_string(r.orderId) +
                    " side=" + toString(r.side) +
                    " price=" + std::to_string(r.price) +
                    " qty=" + std::to_string(r.qty) +
                    " filled=" + std::to_string(r.filled) +
                    " remaining=" + std::to_string(r.remaining) +
                    " status=" + toString(r.status) +
                    " seq=" + std::to_string(r.seq);
    if (!r.reason.empty()) {
        s += " reason=" + r.reason;
    }
    s += "}";
    return s;
}

// ---------------------------------------------------------------------------
// BookTick — market-data tick emitted when a level changes
// ---------------------------------------------------------------------------

struct BookTick {
    Side side = Side::Buy;
    Price price = 0;           // price level that changed
    Quantity totalQuantity = 0; // new total at that level (0 when removed)
    bool removed = false;      // true when the level was pruned
    bool isBest = false;       // true when this level was/is the best
};

[[nodiscard]] inline std::string toString(const BookTick& tk) {
    return std::string("Tick{side=") + toString(tk.side) +
           " price=" + std::to_string(tk.price) +
           " qty=" + std::to_string(tk.totalQuantity) +
           (tk.removed ? " removed" : "") +
           (tk.isBest ? " best" : "") + "}";
}

// ---------------------------------------------------------------------------
// MatchResult — summary of one MatchingEngine::processOrder call
// ---------------------------------------------------------------------------

struct MatchResult {
    OrderId takerId = 0;
    Side takerSide = Side::Buy;
    ExecStatus status = ExecStatus::Rejected;  // final taker status
    Quantity filled = 0;       // sum of trade qtys
    Quantity remaining = 0;    // residual resting qty (0 when Filled/Rejected)
    SeqNo takerSeq = 0;        // seq assigned to the taker (0 when Rejected)
    std::vector<Trade> trades; // in execution order (seq ascending)
    std::string reason;        // non-empty when Rejected

    // Best bid/ask snapshot before and after the call (nullopt when side empty)
    std::optional<Price> bestBidBefore;
    std::optional<Price> bestAskBefore;
    std::optional<Price> bestBidAfter;
    std::optional<Price> bestAskAfter;

    [[nodiscard]] bool isRejected() const noexcept {
        return status == ExecStatus::Rejected;
    }
    [[nodiscard]] bool isFilled() const noexcept {
        return status == ExecStatus::Filled;
    }
};

[[nodiscard]] inline std::string toString(const MatchResult& r) {
    std::string s = std::string("Match{order=") + std::to_string(r.takerId) +
                    " side=" + toString(r.takerSide) +
                    " status=" + toString(r.status) +
                    " filled=" + std::to_string(r.filled) +
                    " remaining=" + std::to_string(r.remaining) +
                    " trades=" + std::to_string(r.trades.size());
    if (!r.reason.empty()) {
        s += " reason=" + r.reason;
    }
    s += "}";
    return s;
}

}  // namespace lob
