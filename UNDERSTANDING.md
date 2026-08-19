# Project Understanding

Live working notes for the high-performance Limit Order Book (LOB) + Matching Engine project.
This file documents what has been built, which file does what, and the rationale behind each change.

---

## 1. Where we are

We are building the system in **C++20** (backend/engine) in iterative phases:

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Core data structures: `Order`, `PriceLevel`, shared types | ✅ Done |
| 2 | `OrderBook` class: add, cancel, best bid/ask | ✅ Done (Step A baseline) |
| 2B | OrderBook performance pass: arena pool + hash levels + benchmarks | ✅ Done |
| 3 | Matching Engine: crossing the spread, partial fills | ⏭ Next |
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

**Phase 2B relationship**: these mandated structures are the canonical baseline
(`OrderBook`) and stay untouched. `FastOrderBook` is the performance variant —
it keeps the *same* public API and invariants but swaps the containers for
arena-pooled intrusive lists, an open-addressing id map, and a bounded price
array (see Section 4). The benchmark in Section 5 measures the trade-off
between the two.

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
│       ├── price_level.cpp    # TU placeholder (all methods are inline in the header)
│       ├── order_book.hpp     # OrderBook: price tree + O(1) order map (Phase 2)
│       ├── order_book.cpp     # TU placeholder (all methods are inline in the header)
│       ├── order_arena.hpp    # Phase 2B: chunked arena pool for order nodes
│       ├── order_id_map.hpp   # Phase 2B: open-addressing OrderId -> node map
│       ├── fast_price_level.hpp  # Phase 2B: intrusive FIFO price level
│       ├── fast_order_book.hpp   # Phase 2B: arena + bounded price array book
│       └── fast_order_book.cpp   # TU placeholder (all methods are inline)
└── tests/
    ├── test_framework.hpp     # minimal dependency-free test harness
    ├── test_main.cpp          # RUN() entry point
    ├── test_order.cpp         # tests for Order
    ├── test_price_level.cpp   # tests for PriceLevel
    ├── test_order_book.cpp    # tests for OrderBook
    ├── test_order_arena.cpp   # tests for the arena pool (Phase 2B)
    ├── test_order_id_map.cpp  # tests for the open-addressing map (Phase 2B)
    ├── test_fast_order_book.cpp  # tests for FastOrderBook (Phase 2B)
    └── bench_order_book.cpp   # OrderBook vs FastOrderBook benchmark (own main)
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

### `src/core/order_book.hpp` — the `OrderBook` (Phase 2)

The central book: resting bids and asks organised by price, with **O(1)
cancellation** and O(log N) best-quote queries.

- **Representation** (mandated):
  - `bids_` / `asks_`: `std::map<Price, PriceLevel>` (red-black tree). Bids are
    queried from `rbegin()` (descending -> highest price first); asks from
    `begin()` (ascending -> lowest price first).
  - `orders_`: `std::unordered_map<OrderId, PriceLevel::iterator>` — the key to
    O(1) cancel. It maps an order id to the *exact list node* holding that
    order, so cancellation never scans a level.
- **Why iterators, not raw pointers**: orders live inside `std::list` nodes.
  `std::list` iterators remain valid across insertions and erasures of *other*
  elements, so the order map can never dangle through unrelated activity. When
  cancelling, the order's `price`/`side` are read **off the Order node itself**,
  so the map stores no duplicated key data that could drift.
- **Key methods**: `addOrder` (O(log N) price lookup via `try_emplace` +
  O(1) FIFO append; assigns the next engine `seq`), `cancelOrder` (O(1);
  prunes the price level from the tree when it empties), `bestBid`/`bestAsk`
  (O(1) via `rbegin()`/`begin()`), `bestBidLevel`/`bestAskLevel` (level
  handles for the Phase 3 matching engine), `findOrder`, `totalQuantity`,
  `levelCount`, `orderCount`.
- **Invariants** (maintained by every mutator, documented in the header):
  1. one `orders_` entry per resting order, each in exactly one level on the
     correct side;
  2. level price key == stored `Order.price`;
  3. no empty level is ever stored (last erase prunes the key);
  4. `PriceLevel::total_qty_` always == sum of remaining qty;
  5. `seq` strictly increasing in arrival order (authoritative time priority).
- **Rejection policy**: `addOrder` rejects duplicate ids (book-wide), zero
  quantity, and non-positive prices. These are cheap defensive checks; the
  gateway is expected to do the heavy validation in later phases.
- All methods are inline in the header (hot path must be inlinable), matching
  the `PriceLevel` convention.

### `src/core/order_book.cpp` — placeholder TU

`OrderBook` is fully header-inline; this file is the stable home for future
non-inline code.

### `src/core/order_arena.hpp` — Phase 2B order-node arena

`OrderNode` + `OrderArena`, the first Phase 2B structure. Replaces the
`std::list` per-order heap allocation with a chunked arena pool:

- `OrderNode` wraps an `Order` plus `prev`/`next` pointers (intrusive list
  links) and a back-pointer to its owning `FastPriceLevel`. Because arena
  memory never relocates, external references can be raw `OrderNode*` instead
  of `std::list` iterators.
- `OrderArena` allocates nodes in contiguous chunks (geometrically growing,
  default first chunk 65,536 nodes, capped at 1,048,576) and hands them out
  from an intrusive free list. `allocate`/`deallocate` are O(1) and never call
  the heap allocator once a chunk exists; a freed node's `next` pointer doubles
  as the free-list link.

### `src/core/order_id_map.hpp` — Phase 2B order-id table

`OrderIdMap`: an open-addressing (linear probing) `OrderId -> OrderNode*` map
with a splitmix64 finalizer and tombstone deletions. It replaces
`std::unordered_map`, which defeats the "no per-order allocation" goal because
MSVC's implementation heap-allocates one node per element. The slot buffer is
pre-sizable (`reserve`) and grows by rehash at ~70% occupancy, so the per-op
cost is amortized to zero after warm-up. Tombstones are compacted by rehash
once live+deleted occupancy crosses the threshold, so heavy add/cancel churn
cannot degrade probe chains.

### `src/core/fast_price_level.hpp` — Phase 2B price level

The `FastPriceLevel`: an intrusive doubly-linked list of `OrderNode*` in FIFO
order (head = oldest = next to execute), the Phase 2B analogue of `PriceLevel`.
`insert` is an O(1) tail append, `erase` is an O(1) unlink through the node's
own pointers, `reduce` keeps `total_qty_` consistent for partial fills. It
stores `price` and `side` on the level so cancellation can find the price-array
slot without re-reading the node.

### `src/core/fast_order_book.hpp` — the `FastOrderBook` (Phase 2B)

The performance book. Same public API as `OrderBook`
(add/cancel/best/find/total), but with Phase 2B internals:

- **Order storage** — `OrderArena` + `FastPriceLevel`: no per-order heap
  allocation, and intrusive links avoid `std::list`'s per-node indirection.
- **Order lookup** — `OrderIdMap`: open addressing over a contiguous buffer
  instead of `std::unordered_map`'s per-node heap nodes.
- **Price lookup** — bounded array indexed by `price - min_price` (domain fixed
  at construction): O(1) lookup instead of `std::map`'s O(log N) tree walk.
- **Best bid/ask** — a cached index per side, set on insert when the price
  improves the best, and refreshed by a walk toward the interior (down for
  bids, up for asks) when the best level empties. The walk is **amortized
  O(1)** per operation (each emptied slot is crossed at most once per time it
  is at the frontier); the worst case is O(price range) for one op that empties
  a tall frontier. This is the standard high-frequency-LOB trade-off: the
  bounded price array buys O(1) lookup and cheap best maintenance at the cost
  of a fixed price domain.

The design keeps every Phase 2 invariant (id uniqueness book-wide, price key ==
level key, no stored empty level, `total_qty_` consistency, strict seq
ordering) and the Phase 3 engine hook (`bestBidLevel`/`bestAskLevel` +
`reduce`). Price-level objects are pooled (created on first use of a price,
recycled when the level empties), so they never allocate on the hot path
either.

### `src/core/fast_order_book.cpp` — placeholder TU

`FastOrderBook` is fully header-inline; this file is the stable home for future
non-inline code.

### `tests/test_order_arena.cpp` — arena tests

Verifies allocation/recycling (LIFO free list), geometric chunk growth without
moving existing nodes, capacity accounting, and that deallocated nodes are
never handed out while still in use.

### `tests/test_order_id_map.cpp` — map tests

Verifies insert/find, duplicate rejection, erase, growth/rehash preserving all
entries, tombstone paths after churn, and `reserve`.

### `tests/test_fast_order_book.cpp` — FastOrderBook tests

The full OrderBook contract re-run against `FastOrderBook`: empty book, best
bid/ask maintenance on both sides (including the upward ask walk), FIFO within
a level, cancels revealing the next best, level pruning, duplicate/invalid/out-
of-domain rejection, seq assignment, side isolation, per-price totals, the
engine-hook reduce path, arena/level reuse after a full drain, and
`forEachLevel` best-first iteration.

### `tests/bench_order_book.cpp` — Phase 2B benchmark

A standalone executable (`lob_bench.exe`) that runs the **same** workloads
against `OrderBook` and `FastOrderBook`:

- **add-only**: a pure stream of limit orders at random prices near the touch.
- **add/cancel mix**: a steady-state population with adds and cancels
  interleaved (exercises cancellation and best-maintenance under churn).
- **best-quote loop**: tight `bestBid()` + `bestAsk()` reads on a populated
  book.

It measures ns/op, ops/s, and **heap allocation counts** via global
`operator new`/`delete` overrides (safe: the bench is its own one-TU
executable). Results are in Section 5 below.

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

### `tests/test_order_book.cpp`

Verifies the book's contracts and invariants:
- Empty book: no best bid/ask, null level handles, zero counts.
- Best-bid = highest resting price; best-ask = lowest resting price, including
  after cancels (the next-best quote is revealed).
- FIFO time priority within a level (`front()` = oldest = next to execute).
- O(1) cancel: level totals, list shape, and `orders_` map stay consistent.
- Last-order-at-a-price cancels prune the level from the tree.
- Duplicate ids are rejected book-wide; unknown ids fail cancel gracefully.
- Invalid orders (zero qty, non-positive price) are rejected.
- `seq` is assigned strictly increasing in arrival order.
- Bids and asks are fully isolated.
- The engine hook: `bestBidLevel()` + `PriceLevel::reduce()` + `cancelOrder()`
  model the Phase 3 fill path and keep every invariant intact.

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
7. **Phase 2 `OrderBook` (Step A baseline)** — `order_book.hpp` with the
   mandated `std::map` + `std::list` + `std::unordered_map` structure,
   documented invariants, rejection policy, and `bestBidLevel`/`bestAskLevel`
   handles as the Phase 3 engine hook. Test suite now at **24 tests**, all
   passing in Debug and Release under `/W4`.
8. **Phase 2B performance pass** — the canonical `OrderBook` is kept intact as
   the mandated-structure baseline; a new `FastOrderBook` implements the same
   public API with the Phase 2B structures:
   - `OrderArena` — chunked arena pool for order nodes (no per-order heap
     allocation; O(1) allocate/free from a free list).
   - `OrderIdMap` — open-addressing `OrderId -> node` table (replaces
     `std::unordered_map`, which heap-allocates a node per element).
   - `FastPriceLevel` — intrusive doubly-linked FIFO level.
   - bounded price array — O(1) price-level lookup; cached best bid/ask with
     amortized O(1) refresh (walk toward the interior when the best empties).
   - `bench_order_book.cpp` — head-to-head benchmark with allocation tracking.

   **Measured results** (Release, x64, MSVC 19.44, price window 49,600–50,400;
   medians of 3 runs, ns/op):

   | workload        | OrderBook    | FastOrderBook | speedup |
   |-----------------|--------------|---------------|---------|
   | add-only        | ~746 ns/op   | ~458 ns/op    | ~1.6x   |
   | add/cancel mix  | ~471 ns/op   | ~244 ns/op    | ~1.9x   |
   | best-quote loop | ~4.7 ns/op   | ~3.8 ns/op    | ~1.25x  |

   **Heap allocations** (the arena's whole point):

   | workload        | OrderBook   | FastOrderBook |
   |-----------------|-------------|---------------|
   | add-only (2M)   | 4,000,003   | **7**         |
   | add/cancel mix  | 4,062,890   | **45**        |
   | best-quote      | 0           | 0             |

   OrderBook allocates ~2 nodes per add (one `std::list` node + one
   `std::unordered_map` node); FastOrderBook does essentially **zero** per-op
   allocation once chunks and the map buffer are warm. The ~1.6–1.9x latency
   win comes from cache-friendly contiguous storage and O(1) array lookup; the
   remaining `FastOrderBook` allocations are one-time growth events (arena
   chunk, map buffer, level pool) amortized over the whole run.
9. **Bug found & fixed during 2B** — `FastOrderBook::acquireLevel()` created a
   level but forgot to push it onto the free list, and the best-bid/ask
   refresh walked only downward (correct for bids, wrong for asks, which walk
   up). Both were caught by the new tests and are covered by regression tests.
10. **Test suite now at 56 tests** (24 baseline + 32 Phase 2B), all passing in
    Debug and Release under `/W4`.

---

## 6. How to verify

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # Debug
& .\build\Debug\lob_tests.exe        # expect: Passed 56/56 tests, 0 failures

powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
& .\build\Release\lob_tests.exe      # expect: Passed 56/56 tests, 0 failures
& .\build\Release\lob_bench.exe      # OrderBook vs FastOrderBook benchmark
```

---

## 7. Next steps

- **Phase 3** — Matching engine: crossing the spread, partial fills, order
  states, using the `bestBidLevel`/`bestAskLevel` handles and `reduce()`. The
  engine will be written once against a small "book backend" interface so both
  `OrderBook` and `FastOrderBook` can drive it.
- Then the gateway, bots, and frontend.