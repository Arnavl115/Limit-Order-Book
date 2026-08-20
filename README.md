# High-Performance LOB + Matching Engine

C++20 limit order book (LOB) with price-time priority, templated matching engine, Winsock2 gateway, Python market-maker, and no-build vanilla JS frontend. Offline, MSVC-only, stdlib/OS built-in libs only.

## Architecture

```
                +-------------------+      4B BE JSON / WS text JSON
   Browser  <--->| Gateway (Winsock) |<----> Python Bot (stdlib socket)
   (frontend/    |  EngineHost<Book> |      (bot/mm_client.py)
    app.js)      |  MatchingEngine   |<----> Fake counterparty / mm_client --test
                 |  OrderBook /      |
                 |  FastOrderBook    |
                 +-------------------+
                          |
                     Book state
                 (bids: std::map / bounded array, levels: std::list / intrusive)
```

- **OrderBook** `src/core/order_book.hpp:1` — mandated `std::map<Price,PriceLevel>` (bids `rbegin`, asks `begin`), `PriceLevel` `std::list<Order>` FIFO `src/core/price_level.hpp:1`, `std::unordered_map<OrderId, PriceLevel::iterator>` O(1) cancel. Invariants: one map entry per resting order, price key == Order.price, no empty level, `total_qty` == sum remaining, `seq` strictly increasing.
- **FastOrderBook** `src/core/fast_order_book.hpp:1` — same API, arena `src/core/order_arena.hpp:1` (chunked 65k→1M, free-list O(1)), `OrderIdMap` `src/core/order_id_map.hpp:1` (open-addressing, 70% load, 16-way SSE `ctrlGroup`), `FastPriceLevel` `src/core/fast_price_level.hpp:1` intrusive doubly-linked FIFO, bounded price array `price-min` O(1) lookup, cached `best_idx` amortized O(1) walk (down for bids, up for asks), level pool.
- **MatchingEngine** `src/core/matching_engine.hpp:1` — template `MatchingEngine<Book>` via `BookBackend` concept `src/core/book_backend.hpp:1` (`LevelTrait<Book>` unifies `PriceLevel` vs `FastPriceLevel` `price`/`front`/`reduce`). Handles Limit/Market/IOC/FOK `src/core/types.hpp:32` (`OrderType{Limit,Market,IOC,FOK}`), Market sweeps until empty (never rests, `no_liquidity` if no fill), IOC fill-then-cancel (`Cancelled`), FOK atomic `totalCrossableQty` pre-check (`fok_insufficient_liquidity` no trades), `modifyOrder`/`replaceOrder` cancel+re-add with new seq (tail). Preserves incoming non-zero `seq` (`next_seq = max(next_seq, seq+1)`) via `allocateSeq`/`nextSeq` `src/core/order_book.hpp:111`.

## Repository Layout

```
project2/
├── build.ps1                 # MSVC driver (VsDevCmd.bat -arch=x64, cl /std:c++20 /EHsc /W4 /DNOMINMAX, per-TU obj, no CMake)
├── docs/protocol.md          # Gateway protocol v1 spec
├── src/core/
│   ├── types.hpp             # OrderId/Quantity(int64 ticks)/Timestamp/SeqNo, Side, OrderType, OrderStatus, toString
│   ├── order.hpp             # Order{id,side,type,price,qty,remaining,ts,seq,status,filled()}
│   ├── price_level.hpp       # PriceLevel FIFO std::list, total_qty, insert/erase/reduce O(1)
│   ├── order_book.hpp        # OrderBook (mandated)
│   ├── order_arena.hpp       # OrderNode + OrderArena chunked pool
│   ├── order_id_map.hpp      # OrderIdMap (direct 4M + Swiss SIMD hash)
│   ├── fast_price_level.hpp  # FastPriceLevel intrusive FIFO
│   ├── fast_order_book.hpp   # FastOrderBook bounded array
│   ├── book_backend.hpp      # BookBackend concept + LevelTrait
│   ├── match_types.hpp       # Trade, ExecStatus, ExecutionReport, BookTick, MatchResult
│   ├── event_sink.hpp        # IEventSink / Null / Counting
│   └── matching_engine.hpp   # MatchingEngine<Book>
├── src/gateway/
│   ├── json.hpp/.cpp         # Minimal JSON (null/bool/int/double/string escapes \uXXXX/array/object depth 64, duplicate reject)
│   ├── frame.hpp/.cpp        # 4B BE len + JSON (1 MiB max, tryDecode NeedMore/Ok/Error)
│   ├── ws_util.hpp/.cpp      # SHA1 (RFC3174) + base64 + WS accept/frame (0x81 text, masked client)
│   ├── session.hpp/.cpp      # Per-connection Session (thread, isWebSocket, handleHttp static WS handshake)
│   ├── server.hpp/.cpp       # Winsock Server (listen 127.0.0.1, acceptThread, per-client workers, broadcast, Ws2_32)
│   ├── engine_host.hpp       # EngineHost<Book> : IEventSink (mutex, nextBcastSeq, snapshot/tick/trade/report JSON)
│   └── main.cpp              # gateway.exe --port 9000 --book fast|canon
├── bot/
│   ├── mm_client.py          # MMClient (socket 4B BE, reconnect 1.5x backoff, ping/subscribe)
│   ├── config.py             # HALF_SPREAD 2, ORDER_QTY 5, INVENTORY_SKEW 0.5, MAX_INVENTORY 50, REFERENCE_PRICE 100, REFRESH 200ms, HEARTBEAT 5s
│   ├── strategy.py           # compute_mid, compute_quotes (mid-half+skew, self-trade would_cross_self), inventory skew
│   └── market_maker.py       # MarketMaker (book maps, own_bids/asks, bid_id/ask_id, inventory via trade, quote via order.new/replace, safety kill/flatten, dry-run)
├── frontend/
│   ├── index.html            # 12-col grid: top bar (symbol|last px|bid/ask|spread|conn), chart canvas, book asks/bids + spread, trades, orders, entry, status bar
│   ├── style.css             # Dark #0B0E11 panels #131722 borders #2A2E39 buy #0ECB81 sell #F6465D tabular monospace heat depth best outline flash
│   └── app.js                # Vanilla JS IIFE: WS ws://host/ws auto-reconnect, snapshot/tick/trade/report, rAF dirty, canvas candlestick, heat, 1/2/Enter/Esc, latency
└── tests/
    ├── test_*.cpp            # 162 tests (see below)
    ├── bench_order_book.cpp  # lob_bench.exe
    └── bench_matching.cpp    # lob_match_bench.exe
```

## Mandatory Structures

| Constraint | Required | Implementation |
|---|---|---|
| Order lookup O(1) | Hash map `OrderId -> list ref` | `OrderBook` `std::unordered_map<OrderId,PriceLevel::iterator>` `src/core/order_book.hpp:1` |
| Price level | `std::list` doubly-linked FIFO | `PriceLevel::orders_ std::list<Order>` head oldest |
| Book sides | `std::map` RB-tree `Price->Level` | `OrderBook` `std::map<Price,PriceLevel>` bids `rbegin` asks `begin` |

Fast variant swaps to `OrderArena` + `OrderIdMap` + `FastPriceLevel` + bounded array while keeping same `BookBackend` interface for one engine.

## Build

No CMake, no `g++`, offline. Requires VS 2022 Build Tools (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat`).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # Debug /Od /Zi
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release  # Release /O2 /GL
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean   # wipe build/
```

Outputs `build\<Config>\lob_tests.exe`, `lob_bench.exe`, `lob_match_bench.exe`, `gateway.exe`. Flags `/std:c++20 /EHsc /W4 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I src`, `/permissive-` `/Zc:__cplusplus`.

## Tests

Dependency-free harness `tests/test_framework.hpp:1` (`TEST`/`CHECK`/`RUN`).

```
162/162 passed Debug and Release /W4 zero warnings
  24 Phase 1/2 baseline (Order, PriceLevel, OrderBook)
  32 Phase 2B (OrderArena, OrderIdMap, FastOrderBook, bench)
   4 Phase 3A (BookBackend concept)
  11 Phase 3B (Trade/Report/Tick, CountingEventSink)
  27 Phase 3C (MatchingEngine limit core both books)
   6 Phase 3D (Parity harness std::minstd_rand 500..5000 steps, top-churn, one-price flood, gaps, adversarial)
  29 Phase 3E (Market/IOC/FOK/modify both books + cross parity)
  17 Phase 4B (JSON escapes/uXXXX depth 64 duplicate, frame 4B BE coalesced/LE reject)
   5 Phase 4C (Server loopback malformed/echo/concurrent/shutdown)
   4 Phase 4D (EngineHost order.new→resting/tick, trade at maker price, cancel, cross-book parity via gateway)
   3 Phase 4E (throughput, concurrent 2×1000, soak 800ms no deadlock, seq monotonic)
```

Run: `.\build\Release\lob_tests.exe`

## Benchmarks

Measured Release x64 MSVC 19.44, price window 49,600–50,400, medians of runs, `operator new` counting via global override (bench is its own TU).

**Phase 2B — Book**

| workload | OrderBook | FastOrderBook | speedup |
|---|---|---|---|
| add-only (2M) | ~560 ns/op 1.8M ops/s allocs 4,000,003 | ~55 ns/op 18.0M ops/s allocs 7 | ~10x |
| add/cancel mix | ~170 ns/op 5.9M allocs 4,062,890 | ~35 ns/op 28.6M allocs 4 | ~4.9x |
| best-quote loop | ~1.1 ns/op 937M | ~1.0 ns/op 1013M allocs 0 | ~1x |

OrderBook 2 allocs/op (list+unordered_map node), Fast 0 after warm (arena chunk, map buffer, level pool amortized).

**Phase 3F — MatchingEngine (200k ops mixed 40% limit near-touch 50% crossing, 10% market, 10% IOC, 5% FOK, 15% cancel)**

| | OrderBook | FastOrderBook |
|---|---|---|
| ns/op | ~232–270 ns | ~145–153 ns (1.8x) |
| M ops/s | 3.7 | 6.5 |
| trades/op | 0.48 | 0.49 |
| allocs | 433,731 | 108,996 (0.54/op vs 2.17/op) |

Engine overhead < one book add-only, steady-state Fast book allocation-free (allocs are `MatchResult::trades` vector).

**Phase 4E — Gateway (via `tests/test_load_gateway.cpp:1`)**

Single client 2000 orders (pipeline 100) through `Server`+`EngineHost` JSON `4B BE` loopback: ~1.4K ops/s (includes WS/JSON/TCP) vs engine-only 3.7M ops/s, seq monotonic, no deadlock, concurrent 2×1000 both seq monotonic, soak 800ms continuous.

Run: `.\build\Release\lob_bench.exe`, `.\build\Release\lob_match_bench.exe` (both allocation-tracked)

## Gateway Protocol v1

See `docs/protocol.md:1`.

- **Transport**: Winsock2 `WSAStartup`/`socket`/`bind(127.0.0.1)`/`listen`/`accept`/`select`/`send`/`recv` (`src/gateway/server.hpp:1` `#pragma Ws2_32`). Multiplexed single port: `GET ` → `handleHttp` (static `frontend/*` via `ifstream` `200`/`404`, `GET /ws` → `101` `computeAcceptKey` SHA1+base64 `src/gateway/ws_util.hpp:1` RFC3174, `0x81` text, masked client, 1 MiB limit) else `4B BE len + JSON` (`src/gateway/frame.hpp:1` `kMax 1 MiB` `tryDecode` NeedMore/Ok/Error). Browser uses `ws://host/ws` text frames, bot uses TCP length-prefix (both JSON same).
- **JSON** `src/gateway/json.hpp:1` hand-rolled `JsonValue` (`Null`/`Bool`/`Int`/`Double`/`String` escapes `\" \\ \/ \b \f \n \r \t \uXXXX` → UTF-8, `Array`/`Object` depth 64, duplicate-key/trailing-garbage rejected, `%.17g` doubles), `parse`→`optional<JsonValue>`, `stringify`.
- **Messages** `order.new` `{id,side:"buy"|"sell",price, qty, orderType:"limit"|"market"|"ioc"|"fok", ts?}` → `order.cancel` `{id}` → `order.replace` `{id,price,qty}` (cancel+re-add tail) → `subscribe`/`ping`; server → `marketdata.snapshot` `{seq,bids:[{price,qty}], asks, bestBid, bestAsk}` (bids `rbegin` / `forEachLevel` best-first), `marketdata.tick` `{side,price,qty,removed,isBest}`, `trade` `{tradeId,seq,takerId,makerId,side,price,qty,ts}` at maker price, `execution.report` `{orderId,side,price,qty,filled,remaining,status:"new"|"partially_filled"|"filled"|"cancelled"|"rejected"|"resting", orderType, seqOrder, reason}`, `error`/`pong`. Server `seq` atomic `nextBcastSeq` monotonic, gap → resubscribe snapshot, idempotent `OrderId` (`duplicate_id` → `Rejected`).
- **Concurrency**: `EngineHost<Book> : IEventSink` `src/gateway/engine_host.hpp:1` holds `Book&`+`Server&`+`MatchingEngine<Book>`+`mutex`+`atomic nextBcastSeq`, `handleMessage` `lock`+`engine.processOrder` → broadcast via `onTrade`/`onOrderUpdate`/`onBookTick` (`make*Json` `seq` → `server.broadcast`), `subscribe`→`sendSnapshot`. `Server` `acceptThread` + per-`Session` worker `Session{thread,isWebSocket,run}` `src/gateway/session.hpp:1`, `broadcast`/`sendTo` per session framing, clean `stop` (`exchange(false)`+`closesocket`+`join`). Backpressure: coalesce ticks, never drop `trade`/`report` → `slow_consumer` disconnect after 1s.
- **Ports**: `gateway.exe --port 9000 --book fast|canon` (default `FastOrderBook(1,100000)` reserve 16k) listens `127.0.0.1:9000` (or ephemeral `0` → `getsockname`). Also serves `frontend/` static.

## Python Bot

`bot/mm_client.py:1` `MMClient` (`socket`, `struct`, `json`) `MAX_FRAME 1MiB`, `connect(retries, backoff 1.5x)`, `send(obj)` `json.dumps(separators)` `>I`, `recv(timeout)`/`recv_all` `frame.tryDecode` loop, `ping`/`subscribe`, `--test` does `ping→pong` `subscribe→snapshot` `order.new→reports`.

`bot/config.py:1` `HALF_SPREAD 2`, `ORDER_QTY 5`, `INVENTORY_SKEW 0.5`, `MAX_INVENTORY 50`, `REFERENCE_PRICE 100` (mid fallback when book empty), `REFRESH_INTERVAL_MS 200`, `HEARTBEAT_TIMEOUT_S 5.0`, `DRY_RUN`.

`bot/strategy.py:1` `compute_mid(bids,asks,last)` → `(bid+ask)//2` else `REFERENCE_PRICE`, `compute_quotes(mid,inv,best,own)` → `bid=mid-half+skew` `ask=mid+half+skew` `skew=-inv*0.5`, `MIN_SPREAD`, clamp `>0`, `would_cross_self` (skip if `bid>=bestAsk` or `>=min(own_ask)` etc.), `qty` capped `MAX_ORDER_SIZE`.

`bot/market_maker.py:1` `MarketMaker{host,port,dry_run}` tracks `book_bids/asks`, `best`, `own_bids/asks` `bid_id/ask_id`, `inventory` via `trade` maker/taker, `quote` via `order.new` or `order.replace` (same id tail), `last_quote_mid`. `_check_safety` `abs(inv)>MAX`→`kill`→`_flatten` cancel all + `market` to 0, `heartbeat` `now-last_msg>HEARTBEAT`→`reconnect`, `--dry-run` logs `would bid/ask`, `--max-inv`/`--duration`.

Verified `dry-run` `bid 98/ask 102` around `REFERENCE_PRICE` when empty, live `sold 3 @102 inv=-3` → `replace 98→100`.

## Frontend

`frontend/index.html:1` 12-col grid: top bar `symbol|last px ±chg%|bid/ask|spread|conn ●`, `6fr 3fr 3fr` main (`canvas#chart` 800×300 + `book` `asks top ↓ bids` + `trades time|px|qty`), `8fr 4fr` bottom (`orders time|id|side|px|qty|status` + entry `side toggle/price/qty/total/type`), status bar `WS|latency|seq|depth`. No external deps, `gateway.exe` serves `frontend/*` via `Session::handleHttp` (`text/html`/`javascript`/`css`).

`frontend/style.css:1` TradingView dark `bg #0B0E11` `panels #131722` `borders #2A2E39` `buy #0ECB81` `sell #F6465D` tabular `ui-monospace` condensed, depth `div.depth` `opacity .12` width `qty/maxQty`, best `outline #2962FF`, spread `#1E222D #FFB800`, flash `rgba` 400ms, `rAF` batching, scrollbars `2A2E39`.

`frontend/app.js:1` vanilla IIFE: `WebSocket` `ws://host/ws` auto-reconnect `delay*1.5`, `subscribe` on open, `marketdata.snapshot` (`Map` + `best` + `lastSeq`) vs `tick` (`Map` + flash), `trade` → `trades` 200 + `lastPrice/chg` + `candleMap` per sec `open/high/low/close/vol` 120, book ladder `maxQty` heat, trades 80, orders `Map` 80 + `status` pills + cancel, `posSummary` `Inv`, `sendOrder` `Date.now()+rand` `orderType` `sendTimes` + `pending`, `latencyMs` via `pong`, `updateTotal` `market` disables price, `1`/`2` side `Enter` submit `Esc` cancelAll, click book row → `inPrice`, chart `canvas` `dpr` `candleW` `gap` `chartScale` wheel `0.5–3` drag, tooltip `O/H/L/C/V`, `setInterval` ping `5s`, `scheduleRender` `requestAnimationFrame` dirty. Exposes `window._lob`.

## How to Verify

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # Debug
.\build\Debug\lob_tests.exe        # 162/162

powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
.\build\Release\lob_tests.exe      # 162/162
.\build\Release\lob_bench.exe      # add-only / mix / best-quote
.\build\Release\lob_match_bench.exe # engine mix
.\build\Release\gateway.exe --port 9000 --book fast   # then Enter to stop
# Bot
python -m bot.mm_client --port 9000 --test
python -m bot.market_maker --port 9000 --dry-run --duration 3
python -m bot.market_maker --port 9000 --duration 5
# Frontend
curl http://127.0.0.1:9000/                # 200 text/html
curl http://127.0.0.1:9000/app.js          # 200 application/javascript
# Browser
# open http://127.0.0.1:9000/ → book heat, trades, chart canvas, order entry 1/2/Enter/Esc, status latency/seq
```

All configs `/W4` zero warnings (`/DNOMINMAX`).

## End-to-End Runbook

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
.\build\Release\gateway.exe --port 9000 --book fast   # terminal 1
python -m bot.market_maker --port 9000 --duration 0   # terminal 2 (0=infinite)
# terminal 3: open http://127.0.0.1:9000/ → place Buy/Sell limit via UI (1/2, price, qty, Enter) → see trade + book flash + candle, inventory
```

Also `start-all.bat:1` one-click `build` → `start gateway` → `start bot` → `start http://127.0.0.1:9000/` (see `start-all.bat:1`).

## Technical Notes

- Prices integer ticks, `seq` authoritative tie-breaker, no floats.
- No external libs downloadable — stdlib/OS only (Winsock2, hand-rolled JSON/SHA1/base64, Python stdlib `socket`/`json`, vanilla JS).
- Invariants: no crossed book ever, `sum(trade qty)==filled`, exact maker reductions, `seq` strictly increasing, FOK atomic, Market/IOC never rest, gateway `seq` monotonic, clean shutdown, `Ws2_32` via `#pragma`.
