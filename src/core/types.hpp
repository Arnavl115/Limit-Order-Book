#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

// Order identifier. Assigned by the exchange; unique per engine lifetime.
using OrderId = std::uint64_t;

// Quantity is always an integer number of shares/lots.
using Quantity = std::uint64_t;

// Price is stored as integer ticks (fixed-point, e.g. micro-dollars) to avoid
// floating point. Never use doubles for prices.
using Price = std::int64_t;

// Wall-clock arrival time in nanoseconds since epoch. Used only as a
// convenience field; engine-assigned sequence numbers are the authoritative
// time-priority tie-breaker.
using Timestamp = std::uint64_t;

// Monotonically increasing engine sequence number. A higher seq means the
// order arrived later, so lower seq wins under time priority at a price level.
using SeqNo = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,   // resting bid
    Sell,  // resting ask
};

enum class OrderType : std::uint8_t {
    Limit,   // resting limit, GTC by default (price must be >0)
    Market,  // no price, sweeps until filled or book empty
    IOC,     // Immediate-Or-Cancel: fill what crosses, cancel residual
    FOK,     // Fill-Or-Kill: all-or-nothing, no partial trades
};

enum class OrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
};

constexpr const char* toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:  return "limit";
        case OrderType::Market: return "market";
        case OrderType::IOC:    return "ioc";
        case OrderType::FOK:    return "fok";
    }
    return "unknown";
}

constexpr const char* toString(Side s) noexcept {
    return s == Side::Buy ? "buy" : "sell";
}

constexpr const char* toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::New:             return "new";
        case OrderStatus::PartiallyFilled: return "partially_filled";
        case OrderStatus::Filled:           return "filled";
        case OrderStatus::Cancelled:        return "cancelled";
        case OrderStatus::Rejected:         return "rejected";
    }
    return "unknown";
}

}  // namespace lob