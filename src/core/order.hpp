#pragma once

#include "types.hpp"

namespace lob {

// A single order. Instances live inside a std::list owned by a PriceLevel;
// external references are held as std::list iterators, never raw pointers,
// so the underlying node address is irrelevant to users of the API.
struct Order {
    OrderId     id = 0;
    Side        side = Side::Buy;
    OrderType   type = OrderType::Limit;
    Price       price = 0;
    Quantity    qty = 0;          // quantity at the moment of submission
    Quantity    remaining = 0;    // qty - filled so far
    Timestamp   ts = 0;           // arrival wall clock (informational)
    SeqNo       seq = 0;          // engine sequence; authoritative for time priority
    OrderStatus status = OrderStatus::New;

    constexpr Quantity filled() const noexcept { return qty - remaining; }
    constexpr bool isFilled() const noexcept { return remaining == 0; }
    constexpr bool isResting() const noexcept { return remaining > 0; }
};

}  // namespace lob