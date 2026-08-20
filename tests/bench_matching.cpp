// Phase 3F benchmark — MatchingEngine over OrderBook vs FastOrderBook
// Measures engine cost (ns/op, trades/op, allocs/op) under a synthetic
// taker stream that exercises crossing, resting, and multi-level sweeps.
// Like bench_order_book.cpp, this is a standalone executable with global
// operator new counting so steady-state allocations are visible.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "core/fast_order_book.hpp"
#include "core/matching_engine.hpp"
#include "core/order_book.hpp"

using namespace lob;

// ---------------------------------------------------------------------------
// Global new/delete counting (confined to this TU)
// ---------------------------------------------------------------------------
static std::size_t g_allocs = 0;
static std::size_t g_frees = 0;

void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { ++g_frees; std::free(p); }
void operator delete[](void* p) noexcept { ++g_frees; std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ++g_frees; std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { ++g_frees; std::free(p); }

// ---------------------------------------------------------------------------
// PRNG
// ---------------------------------------------------------------------------
static std::uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static std::uint64_t rand64() {
    std::uint64_t x = g_seed;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_seed = x;
    return x * 0x2545F4914F6CDD1Dull;
}
static std::uint64_t randBelow(std::uint64_t n) { return rand64() % n; }

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static constexpr Price kCenter = 50000;
static constexpr Price kSpread = 200;
static constexpr Price kMinP = kCenter - kSpread - 200; // 49600
static constexpr Price kMaxP = kCenter + kSpread + 200; // 50400

static Order makeLimit(OrderId id, Side side, Price price, Quantity qty) {
    Order o;
    o.id = id; o.side = side; o.type = OrderType::Limit; o.price = price; o.qty = qty; o.remaining = qty;
    o.ts = id * 1000;
    return o;
}
static Order makeMarket(OrderId id, Side side, Quantity qty) {
    Order o;
    o.id = id; o.side = side; o.type = OrderType::Market; o.price = 0; o.qty = qty; o.remaining = qty;
    o.ts = id * 1000;
    return o;
}

struct Result {
    std::size_t ops = 0;
    double ns_per_op = 0;
    double trades_per_op = 0;
    std::size_t allocs = 0;
    std::size_t frees = 0;
};

static double elapsedNs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

// ---------------------------------------------------------------------------
// Engine bench: mixed crossing / resting / market / ioc / fok / cancel
// For bench we drive MatchingEngine, not raw book.addOrder, so the measured
// cost is engine + book. A pure book bench is in bench_order_book.cpp.
// ---------------------------------------------------------------------------

template<typename Book>
Result benchEngine(Book& book, MatchingEngine<Book>& eng, std::size_t warmup, std::size_t timed) {
    // Pre-fill book to create depth (so crossing has something to match)
    OrderId id = 1;
    for (int i = 0; i < 1000; ++i) {
        Side s = (rand64() & 1) ? Side::Sell : Side::Buy;
        Price p = (s == Side::Buy) ? (kCenter - 10 - (Price)randBelow(50)) : (kCenter + 10 + (Price)randBelow(50));
        eng.processOrder(makeLimit(id++, s, p, 1 + randBelow(20)));
    }

    std::size_t totalTrades = 0;
    auto run = [&](std::size_t steps) {
        for (size_t i = 0; i < steps; ++i) {
            int roll = (int)randBelow(100);
            Order o;
            if (roll < 40) {
                // limit taker near touch (50% cross, 50% rest)
                Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                Price px;
                if (side == Side::Buy) px = kCenter - (Price)randBelow(30) + (Price)randBelow(40); //  around mid
                else px = kCenter + (Price)randBelow(30) - (Price)randBelow(40);
                // occasionally force cross by widening price
                if ((rand64() & 1) == 0) {
                    px = (side == Side::Buy) ? kCenter + 50 : kCenter - 50;
                }
                o = makeLimit(id++, side, px, 1 + randBelow(10));
            } else if (roll < 50) {
                // market
                Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                o = makeMarket(id++, side, 1 + randBelow(10));
            } else if (roll < 60) {
                // IOC
                Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                Price px = (side == Side::Buy) ? kCenter + (Price)randBelow(30) : kCenter - (Price)randBelow(30);
                o = makeLimit(id++, side, px, 1 + randBelow(10));
                o.type = OrderType::IOC;
            } else if (roll < 65) {
                // FOK (less frequent)
                Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                Price px = (side == Side::Buy) ? kCenter + 40 : kCenter - 40;
                o = makeLimit(id++, side, px, 1 + randBelow(5));
                o.type = OrderType::FOK;
            } else if (roll < 80 && book.orderCount() > 200) {
                // cancel random live order via engine (emits events)
                // pick a random id that may or may not exist; we find one via best level front
                auto* lvl = (rand64() & 1) ? book.bestBidLevel() : book.bestAskLevel();
                if (lvl && !LevelTrait<Book>::empty(lvl)) {
                    auto h = LevelTrait<Book>::front(lvl);
                    OrderId cid = LevelTrait<Book>::getOrder(h).id;
                    eng.cancelOrder(cid);
                    continue; // cancel counts as op
                } else {
                    // fallback to limit
                    Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                    Price px = kCenter + (side==Side::Buy ? 10 : -10);
                    o = makeLimit(id++, side, px, 1 + randBelow(10));
                }
            } else {
                // resting limit far from touch
                Side side = (rand64() & 1) ? Side::Buy : Side::Sell;
                Price px = (side == Side::Buy) ? (kCenter - 80 - (Price)randBelow(30)) : (kCenter + 80 + (Price)randBelow(30));
                o = makeLimit(id++, side, px, 1 + randBelow(10));
            }
            auto r = eng.processOrder(o);
            totalTrades += r.trades.size();
        }
    };

    run(warmup);
    g_allocs = 0; g_frees = 0;
    std::size_t tradesBefore = totalTrades;
    auto t0 = std::chrono::steady_clock::now();
    run(timed);
    auto t1 = std::chrono::steady_clock::now();

    Result r;
    r.ops = timed;
    r.ns_per_op = elapsedNs(t0, t1) / double(timed);
    r.trades_per_op = double(totalTrades - tradesBefore) / double(timed);
    r.allocs = g_allocs;
    r.frees = g_frees;
    return r;
}

template<typename Fn>
static void report(Fn&& make) {
    constexpr std::size_t kWarm = 20000;
    constexpr std::size_t kTimed = 200000;

    {
        auto book = make();
        MatchingEngine<typename std::remove_reference<decltype(*book)>::type> eng(*book);
        // For Fast, reserve
        if constexpr (std::is_same_v<typename std::remove_reference<decltype(*book)>::type, FastOrderBook>) {
            book->reserveOrders(8192);
        }
        Result r = benchEngine(*book, eng, kWarm, kTimed);
        std::printf("  engine mix (limit/market/ioc/fok/cancel): %7.1f ns/op  %5.1fM ops/s  trades/op %.2f  allocs=%zu frees=%zu\n",
            r.ns_per_op, 1000.0 / r.ns_per_op, r.trades_per_op, r.allocs, r.frees);
    }
}

static std::unique_ptr<OrderBook> makeCanon() { return std::make_unique<OrderBook>(); }
static std::unique_ptr<FastOrderBook> makeFast() {
    auto b = std::make_unique<FastOrderBook>(kMinP, kMaxP);
    b->reserveOrders(16384);
    return b;
}

int main() {
    std::printf("Phase 3F — MatchingEngine benchmark (price window [%lld,%lld])\n",
        (long long)kMinP, (long long)kMaxP);
    std::printf("OrderBook engine:\n");
    report(makeCanon);
    std::printf("FastOrderBook engine:\n");
    report(makeFast);
    return 0;
}
