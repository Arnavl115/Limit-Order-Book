# Gateway Protocol v1 (Phase 4A)

Date: 2026-08-20
Status: spec — implementation in 4B-4E

## 1. Decision — transport

Environment probe (2026-08-20, Windows, offline, `winget` blocked for CMake):
- `python --version` → **Python 3.14.6** (`C:\Users\arnav\AppData\Local\Programs\Python\Python314`), `pip 26.1.2` with only `certifi/cffi/charset-normalizer/cryptography/idna/pycparser/requests/urllib3` cached — no `websockets`/`aiohttp`. Bot **must be stdlib-only** (`socket`, `json`, `hashlib`, `base64`) — consistent with spec 5A.
- `node --version` → **v26.5.0**, `npm 11.17.0`, globals `cline/codecall/kanban/opencode-ai` only — no `react`/`vite` cached. Frontend **must be no-build static** (`frontend/index.html`+`app.js`+`style.css`) served by gateway — consistent with spec 6A.
- No CMake/ninja/g++ on PATH — MSVC Build Tools only, `build.ps1` drives `cl.exe` directly.
- Network blocked — nothing downloadable; all libs must be stdlib/OS built-in.

Decision per spec cross-cutting rule "Nothing external is downloadable — all libs must be stdlib/OS-built-in" and spec 4 constraint "offline. No external libs — use Winsock2 (Windows built-in) and a hand-rolled minimal JSON writer/parser":

- **Transport: TCP length-prefixed JSON for bot + minimal HTTP/WebSocket for browser on the same port (multiplexed).**
  - Bot (Python) connects via raw TCP and uses `4-byte big-endian length` framing — simplest stdlib implementation, no `websockets` pip needed.
  - Browser cannot speak raw TCP, so the same listener also speaks HTTP/WebSocket (RFC 6455). `GET /` serves static files from `frontend/`; `GET /ws` with `Upgrade: websocket` performs handshake and then carries the same JSON messages as WebSocket text frames. Python bot may optionally use the same WebSocket handshake (implemented via `hashlib.sha1`+`base64` stdlib) but the length-prefixed path is kept for `tests/test_gateway` loopback simplicity.
- **Why not WebSocket-only for bot**: Python stdlib has no WebSocket client; hand-rolling the client frame masking is doable but length-prefix is one `struct.pack(">I", len)` and no masking, so the harness keeps both paths tested: `gateway` accepts raw length-prefix when the first bytes are not `GET `, otherwise parses HTTP.

## 2. Framing

### 2a. TCP length-prefix (bot, tests)

```
[4 bytes big-endian uint32 N][N bytes UTF-8 JSON object]
```

- `0 < N <= 1_048_576` (1 MiB). Larger → server sends `{type:"error",reason:"frame_too_large"}` and closes.
- Receiver must buffer until `4+N` bytes available; multiple messages may be coalesced in one `recv`.
- One JSON object per frame (no NDJSON, no delimiter). Whitespace outside object is not allowed.
- Server and client both use this framing on raw TCP. Little-endian is rejected.

### 2b. WebSocket (browser, also usable by bot)

- Standard RFC 6455 handshake: client sends `GET /ws HTTP/1.1` + `Sec-WebSocket-Key`, server replies `101 Switching Protocols` with `Sec-WebSocket-Accept: base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`.
- After handshake, messages are **text frames** (`opcode 0x1`, FIN=1, payload = UTF-8 JSON object). No fragmentation. Client-to-server frames are masked (spec requires); server-to-client are not masked. `opcode 0x8` close, `0x9` ping (server replies pong `0xA`), `0x2` binary is rejected.
- Max text payload 1 MiB, same as TCP.

### 2c. HTTP static

- `GET /` or `GET /index.html` → `Content-Type: text/html`
- `GET /app.js` → `application/javascript`, `GET /style.css` → `text/css`
- `404` for unknown paths, `426 Upgrade Required` for `GET /ws` without WebSocket headers.

## 3. JSON — hand-rolled minimal codec

Both sides use `src/gateway/json.hpp` (no `nlohmann/json`). Supported:

- Types: `null`, `bool`, `number` (int64 or double — prices/qty are integers, but codec accepts both), `string` (UTF-8, escapes `\" \\ \/ \b \f \n \r \t \uXXXX`), `array`, `object` (unordered, string keys).
- Writer: `JsonValue::stringify()` produces canonical JSON (no pretty-print). Numbers encoded without trailing `.0` when integer.
- Parser: `JsonValue::parse(string_view)` returns `std::optional<JsonValue>` + error string; rejects duplicate keys, trailing garbage, unterminated strings, invalid escapes, numbers outside int64/double, depth > 64.

## 4. Message types (all JSON objects, `type` discriminator)

Common envelope optional: `{ "seq": uint64, "ts": uint64 }` — `seq` is server broadcast seq (global, monotonic, per connection ordered), `ts` is server wall clock ns. Client messages may omit `seq`/`ts`.

### 4.1. Client → Server

| `type` | fields | description |
|---|---|---|
| `order.new` | `id:uint64`, `side:"buy"|"sell"`, `price:int64` (ticks, 0 allowed only for `market`), `qty:uint64`, `orderType:"limit"|"market"|"ioc"|"fok"` (default `"limit"`), `ts?:uint64` | New order. Server validates (`zero_qty`/`invalid_price`/`duplicate_id`/domain) → `execution.report` `Rejected` or `New`+matching. Market `price` ignored. |
| `order.cancel` | `id:uint64` | Cancel resting order. → `execution.report` `Cancelled` or `Rejected(unknown_id)`. |
| `order.replace` | `id:uint64`, `price:int64`, `qty:uint64` | Alias for `modifyOrder`: `cancel` + `new` with same id, re-queued tail. Equivalent to `order.cancel` + `order.new` atomically. |
| `subscribe` | `channel:"book"|"trades"|"reports"` | Optional; by default server subscribes to all on connect. Kept for future filtering. |
| `ping` | `id?:any` | Keepalive → `pong`. |

### 4.2. Server → Client (broadcast, seq-ordered)

| `type` | fields | description |
|---|---|---|
| `marketdata.snapshot` | `seq:uint64`, `bids:[{price,qty}], asks:[{price,qty}]` (best-first: bids desc, asks asc), `bestBid?:int64`, `bestAsk?:int64` | Sent on connect (and on `subscribe`) — full book snapshot. |
| `marketdata.tick` | `seq:uint64`, `side:"buy"|"sell"`, `price:int64`, `qty:uint64` (0 when removed), `removed:bool`, `isBest:bool` | Level total changed / pruned. Mirrors `BookTick` `src/core/event_sink.hpp:1`. |
| `trade` | `seq:uint64`, `tradeId:uint64`, `takerId:uint64`, `makerId:uint64`, `side:"buy"|"sell"` (taker side), `price:int64`, `qty:uint64`, `ts:uint64` | One maker/taker match at maker price. `seq` is trade seq (`next_trade_seq_`). |
| `execution.report` | `seq:uint64`, `orderId:uint64`, `side`, `price`, `qty`, `filled`, `remaining`, `status:"new"|"partially_filled"|"filled"|"cancelled"|"rejected"|"resting"`, `orderType`, `reason?:string` | Per-order lifecycle. `status` maps `ExecStatus` `src/core/match_types.hpp:1`. `reason` for `rejected` (`duplicate_id`/`zero_qty`/`invalid_price`/`price_out_of_domain`/`fok_insufficient_liquidity`/`no_liquidity`/`ioc_no_fill`/`unknown_id`). |
| `error` | `reason:string` | Malformed frame/JSON, unknown type, frame too large. Connection stays open for JSON errors, closed for framing errors. |
| `pong` | `id?:any` | Reply to `ping`. |

Seq discipline: server holds `std::atomic<uint64_t> broadcastSeq` (starts 1). Every `marketdata.*`/`trade`/`execution.report` broadcast increments it and sets `seq`. Clients can detect gap (`seq != prev+1`) and re-request `marketdata.snapshot`.

Idempotent id handling: `id` is client-supplied `OrderId`. Server rejects duplicate live ids (`book.contains(id)`) with `Rejected(duplicate_id)`. After fill/cancel, the id may be reused (book no longer contains it) — safe to retry.

## 5. Concurrency & backpressure

- Engine lives behind `std::mutex` in `src/gateway/engine_host.hpp` (single-threaded matching; gateway threads acquire lock for `processOrder`/`cancelOrder`/`modifyOrder`).
- Gateway: one `accept` thread + per-connection worker thread (blocking `recv` loop; simple, fits `select` on Windows). `send` is `std::mutex` per connection.
- Backpressure: per-connection send buffer is TCP's kernel buffer; if `send` would block (slow consumer), worker checks `select` writability with 100ms timeout; ticks/trades that would overflow are coalesced (only latest `marketdata.tick` per price kept); `execution.report`/`trade` are never dropped — slow consumer is disconnected with `{type:"error",reason:"slow_consumer"}` after 1s stall. Documented policy.

## 6. Ports & executables

- `gateway.exe` (built by `build.ps1` Release) listens on `127.0.0.1:9000` (raw length-prefix) and `127.0.0.1:8080` (HTTP/WebSocket + static). Ports configurable via CLI `--port`/`--http-port`.
- `build.ps1` builds `lob_tests.exe` (includes loopback integration tests `tests/test_gateway.cpp`), `lob_bench.exe`, `lob_match_bench.exe`, `gateway.exe`.

## 7. Verification plan (matches Phase 4 steps)

- 4B: `tests/test_json.cpp` (round-trip, escapes, malformed rejection) + `tests/test_frame.cpp` (length-prefix encode/decode, partial buffers, too-large, little-endian rejection) — both `/W4` clean.
- 4C: loopback `tests/test_gateway.cpp` — connect raw TCP, send malformed frame → `error`, echo, two concurrent clients, shutdown mid-connection (server `stop()` joins threads).
- 4D: `tests/test_gateway_integration.cpp` — `order.new` limit over socket → `trade` broadcast + `execution.report` echoes, `order.cancel` → `Cancelled`, snapshot on connect, cross-book parity flag (gateway constructed with `OrderBook` vs `FastOrderBook` yields identical JSON traces for same input sequence).
- 4E: `tests/load_gateway.cpp` (or `bench_gateway.cpp`) — steady-state 200k orders/s through socket, seq ordering, no deadlock over 60s soak; numbers recorded in `UNDERSTANDING.md` §5.

## 8. Appendix — example exchange

```
C→S {"type":"order.new","id":1,"side":"buy","price":100,"qty":10,"orderType":"limit"}
S→* {"type":"execution.report","seq":1,"orderId":1,"status":"new", ...}
S→* {"type":"execution.report","seq":2,"orderId":1,"status":"resting", ...}
S→* {"type":"marketdata.tick","seq":3,"side":"buy","price":100,"qty":10,"removed":false,"isBest":true}

C→S {"type":"order.new","id":2,"side":"sell","price":100,"qty":6,"orderType":"limit"}
S→* {"type":"trade","seq":4,"tradeId":1,"takerId":2,"makerId":1,"price":100,"qty":6,"side":"sell"}
S→* {"type":"execution.report","seq":5,"orderId":1,"status":"partially_filled", ...}
S→* {"type":"execution.report","seq":6,"orderId":2,"status":"filled", ...}
S→* {"type":"marketdata.tick","seq":7,"side":"buy","price":100,"qty":4,"removed":false,"isBest":true}
```
