# Project Understanding

Live working notes for the high-performance Limit Order Book (LOB) + Matching Engine project.
This file documents what has been built, which file does what, and the rationale behind each change.

---

## 1. Where we are

We are building the system in **C++20** (backend/engine) in iterative phases:

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Core data structures: `Order`, `PriceLevel`, shared types | ✅ Done |
| 2 | `OrderBook` class: add, cancel, best bid/ask | ⏭ Next |
| 3 | Matching Engine: crossing the spread, partial fills | pending |
| 4 | API & WebSocket gateway | pending |
| 5 | Python market-maker bot | pending |
| 6 | React frontend | pending |

The LOB strictly follows **price-time priority**: at any price, orders execute in
FIFO order (oldest first). The constraints below are mandatory.

---

## 2. Mandatory data-structure constraints (and how we satisfy them)

| Constraint | Required structure | Our implementation |
|-----------|--------------------|--------------------|
| Order lookup + O(1) cancel | Hash map `OrderID -> reference into list` | `std::unordered_map` lives in the `OrderBook` (Phase 2) and stores a `PriceLevel::iterator`, not a raw pointer |
| Price level | `std::list` (doubly linked) of orders, FIFO | `PriceLevel::orders_` is a `std::list<Order>`; head = oldest order |
| Book sides | `std::map` / red-black tree, `Price -> PriceLevel` | `OrderBook` uses `std::map`; bids traversed with `rbegin()` (descending), asks with `begin()` (ascending) |

Complexity guarantees already in place (Phase 1):
- **Insert** into a price level: O(1) — push to the tail of the `std::list`.
- **Erase** from a price level: O(1) — `std::list::erase(iterator)`; the iterator
  came from the order map, so no scan is ever needed.
- **Best bid / ask**: O(log N) via `std::map` (red-black tree); bids use the
  highest key, asks the lowest.

Note on pointers: orders are stored *inside* `std::list` nodes. External
references are kept as **iterators**, never raw pointers. `std::list` iterators
stay valid after insertions and after erasing *other* elements, which is exactly
the guarantee the order map relies on.

---

## 3. Repository layout

```
project2/
├── build.ps1                 # PowerShell build driver (MSVC, no CMake needed)
├── src/
│   └── core/
│       ├── types.hpp          # shared aliases + enums
│       ├── order.hpp          # the Order struct
│       ├── price_level.hpp    # PriceLevel: std::list FIFO level, O(1) ops
│       └── price_level.cpp    # TU placeholder (all methods are inline in the header)
└── tests/
    ├── test_framework.hpp     # minimal dependency-free test harness
    ├── test_main.cpp          # RUN() entry point
    ├── test_order.cpp         # tests for Order
    └── test_price_level.cpp   # tests for PriceLevel
```

---

## 4. File-by-file explanation

### `build.ps1` — build driver

- The machine has **no CMake, no ninja, no g++** on PATH, and the network is
  blocked (winget can't download CMake).
- It locates the installed **MSVC 2022 Build Tools** at
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\...` and
  invokes `VsDevCmd.bat -arch=x64` to set up the x64 environment, then calls
  `cl.exe` directly.
- Build strategy: **compile each translation unit to an `.obj`** in
  `build\obj-<Config>`, then link everything into `build\<Config>\lob_tests.exe`.
  Per-TU objects keep the build incremental.
- Flags: `/std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus` plus `/I src`
  (so tests can `#include "core/..."`).
- Two configurations: `Debug` (`/Od /Zi`) and `Release` (`/O2 /GL`).

Usage:
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1            # Debug
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean    # wipe build/
```

### `src/core/types.hpp` — shared types

- `OrderId` (`uint64_t`) — unique order id.
- `Quantity` (`uint64_t`) — integer shares/lots.
- `Price` (`int64_t`) — price in **integer ticks** (fixed-point, e.g. micro-dollars).
  Doubles are banned for prices; no float rounding anywhere in the engine.
- `Timestamp` (`uint64_t`) — arrival wall clock in ns (informational only).
- `SeqNo` (`uint64_t`) — engine sequence number; the *authoritative* time-priority
  tie-breaker (wall-clock timestamps can collide; seq numbers cannot).
- Enums: `Side { Buy, Sell }`, `OrderType { Limit }` (Market/IOC/FOK come later),
  `OrderStatus { New, PartiallyFilled, Filled, Cancelled, Rejected }`.
- `toString(...)` helpers for logging/serialization.

### `src/core/order.hpp` — the `Order` struct

Fields: `id, side, type, price, qty, remaining, ts, seq, status`.

- `qty` is the quantity at submission; `remaining` is what's left to fill.
- Helpers: `filled()` = `qty - remaining`, `isFilled()`, `isResting()`.
- All fields are public value types so orders can be copied/moved into list nodes.

### `src/core/price_level.hpp` — the price level

A single level of the book (one distinct price).

- `std::list<Order> orders_` — FIFO container. Head of list = oldest order = next
  to execute under time priority.
- `Quantity total_qty_` — running sum of remaining quantity, kept consistent so
  the book can report level depth in O(1).
- `insert(Order)` → O(1) tail append; returns `iterator` for the order map.
- `erase(iterator)` → O(1); decrements `total_qty_`; returns the following iterator.
- `reduce(iterator, qty)` → O(1) partial-fill helper: subtracts from
  `remaining`, flips status to `Filled`/`PartiallyFilled`, and keeps `total_qty_`
  correct. This is the hook the Matching Engine (Phase 3) will use.
- Accessors: `empty()`, `size()` (O(1) for `std::list` since C++11), `totalQuantity()`,
  `price()`, `front()`, and `begin()/end()` for iteration.
- Everything is inline in the header to maximise inlining (low-latency goal).

### `src/core/price_level.cpp` — placeholder TU

`PriceLevel` is fully header-inline; this file just exists as a stable home for
future non-inline code. It currently only `#include`s the header.

### `tests/test_framework.hpp` — mini test harness

- Deliberately **dependency-free** (no Catch2/GoogleTest) so we can build offline.
- `TEST(name)` registers a test; `CHECK(expr)` records failures with file/line.
- `RUN()` expands to `main()` that runs every registered test and returns
  non-zero on failure.

### `tests/test_main.cpp`

Contains `RUN()` (the `main`). Nothing else.

### `tests/test_order.cpp`

Verifies `Order` defaults, fill math (`filled()`, `isFilled()`, `isResting()`
across partial-fill transitions), and the `toString` helpers.

### `tests/test_price_level.cpp`

Verifies the critical invariants:
- FIFO order: insertion order is preserved; `front()` is the oldest order.
- `totalQuantity()` accumulates correctly and stays consistent through erases.
- Erasing the middle/head/last order leaves a valid FIFO level with correct totals.
- Iterators returned by `insert` remain valid after further inserts.
- `reduce()` handles partial fills (status transitions + quantity accounting).

---

## 5. What changed / was newly introduced (delta since the empty repo)

1. **Toolchain discovery & build system** — no `cmake`/`ninja`/`g++` exist, so a
   CMake-free MSVC build driver (`build.ps1`) was created. Verified with
   `cl.exe` from VS 2022 Build Tools, `/std:c++20`.
2. **New directory structure** — `src/core/` for engine headers, `tests/` for tests.
3. **`types.hpp`, `order.hpp`, `price_level.hpp`** — Phase 1 core data structures.
4. **`price_level.hpp` design choice** — orders stored inside `std::list`; API
   returns *iterators*, not raw pointers, so the order map (Phase 2) gets O(1)
   cancellation with no pointer-validity hazards.
5. **`reduce()` API** — added early so `PriceLevel` owns its `total_qty_`
   invariant; Phase 3 matching will call it for partial fills.
6. **Test infrastructure** — dependency-free harness + 7 passing tests
   (Debug and Release both build clean under `/W4`).

---

## 6. How to verify

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
& .\build\Debug\lob_tests.exe        # expect: Passed 7/7 tests, 0 failures
```

---

## 7. Next steps

- **Phase 2** — `OrderBook`: `std::map<Price, PriceLevel>` for asks (ascending)
  and bids (descending), `OrderMap` (`std::unordered_map<OrderId, PriceLevel::iterator>`),
  `AddOrder`, `CancelOrder`, `GetBestBid()`, `GetBestAsk()`, and tests.
- **Phase 3** — Matching engine: crossing the spread, partial fills, order states.
- Then the gateway, bots, and frontend.