// Phase 2B benchmark: OrderBook (canonical std:: containers) vs FastOrderBook
// (arena pool + bounded price array) on the SAME public API and workloads.
//
// Measures per-operation latency/throughput and, crucially, heap allocations
// per op — the whole point of the arena is "no per-order allocation".
//
// Run in Release:  powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
//                   .\build\Release\lob_bench.exe

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "core/fast_order_book.hpp"
#include "core/order_book.hpp"

using namespace lob;

// ---------------------------------------------------------------------------
// Global new/delete overrides to count heap allocations. This translation
// unit is the entire lob_bench.exe, so the override is confined to it.
// ---------------------------------------------------------------------------
static std::size_t g_allocs = 0;
static std::size_t g_frees = 0;

void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    ++g_frees;
    std::free(p);
}

void operator delete[](void* p) noexcept {
    ++g_frees;
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    ++g_frees;
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    ++g_frees;
    std::free(p);
}

// ---------------------------------------------------------------------------
// Deterministic PRNG (xorshift64star) so runs are reproducible.
// ---------------------------------------------------------------------------
static std::uint64_t g_seed = 0x9E3779B97F4A7C15ull;

static std::uint64_t rand64() {
    std::uint64_t x = g_seed;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_seed = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static std::uint64_t randBelow(std::uint64_t n) { return rand64() % n; }

// ---------------------------------------------------------------------------
// Shared workload parameters (prices in a realistic tick window).
// ---------------------------------------------------------------------------
static constexpr Price kCenter = 50000;
static constexpr Price kSpread = 200;
static constexpr Price kMinPrice = kCenter - kSpread - 200;  // 49600
static constexpr Price kMaxPrice = kCenter + kSpread + 200;  // 50400

static Side randSide() { return (rand64() & 1u) ? Side::Sell : Side::Buy; }

static Price randPrice() { return kCenter - kSpread + static_cast<Price>(randBelow(2 * kSpread + 1)); }

static Order makeOrder(OrderId id, Side side, Price price) {
    Order o;
    o.id = id;
    o.side = side;
    o.type = OrderType::Limit;
    o.price = price;
    o.qty = 1 + randBelow(100);
    o.remaining = o.qty;
    return o;
}

// ---------------------------------------------------------------------------
// Results + reporting.
// ---------------------------------------------------------------------------
struct Result {
    std::size_t ops = 0;
    double ns_per_op = 0.0;
    std::size_t allocs = 0;
    std::size_t frees = 0;
};

static double elapsedNs(std::chrono::steady_clock::time_point t0,
                        std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
// Workload 1: pure add stream (measures insert throughput + per-add allocs).
// ---------------------------------------------------------------------------
template <typename Book>
Result benchAddOnly(Book& book, std::size_t warmup, std::size_t timed) {
    OrderId id = 1;
    auto run = [&](std::size_t steps) {
        for (std::size_t i = 0; i < steps; ++i) {
            book.addOrder(makeOrder(id++, randSide(), randPrice()));
        }
    };

    run(warmup);
    g_allocs = 0;
    g_frees = 0;
    auto t0 = std::chrono::steady_clock::now();
    run(timed);
    auto t1 = std::chrono::steady_clock::now();

    Result r;
    r.ops = timed;
    r.ns_per_op = elapsedNs(t0, t1) / static_cast<double>(timed);
    r.allocs = g_allocs;
    r.frees = g_frees;
    return r;
}

// ---------------------------------------------------------------------------
// Workload 2: steady-state add/cancel mix around a bounded live population
// (measures the cancel path + best-quote maintenance under churn).
// ---------------------------------------------------------------------------
template <typename Book>
Result benchMix(Book& book, std::size_t live_fill, std::size_t max_live,
                std::size_t warmup, std::size_t timed) {
    std::vector<OrderId> live;
    live.reserve(max_live + 1);

    OrderId id = 1;
    for (std::size_t i = 0; i < live_fill; ++i) {
        if (book.addOrder(makeOrder(id, randSide(), randPrice()))) {
            live.push_back(id);
            ++id;
        }
    }

    std::size_t ops_done = 0;
    auto run = [&](std::size_t steps) {
        for (std::size_t s = 0; s < steps; ++s) {
            if (live.size() >= max_live) {
                std::size_t pick = randBelow(live.size());
                OrderId cancel_id = live[pick];
                live[pick] = live.back();
                live.pop_back();
                book.cancelOrder(cancel_id);
            } else if ((rand64() & 1u) == 0) {
                Order o = makeOrder(id, randSide(), randPrice());
                if (book.addOrder(o)) {
                    live.push_back(id);
                    ++id;
                }
            } else if (!live.empty()) {
                std::size_t pick = randBelow(live.size());
                OrderId cancel_id = live[pick];
                live[pick] = live.back();
                live.pop_back();
                book.cancelOrder(cancel_id);
            }
            ++ops_done;
        }
    };

    run(warmup);
    g_allocs = 0;
    g_frees = 0;
    std::size_t ops_before = ops_done;
    auto t0 = std::chrono::steady_clock::now();
    run(timed);
    auto t1 = std::chrono::steady_clock::now();

    Result r;
    r.ops = ops_done - ops_before;
    r.ns_per_op = elapsedNs(t0, t1) / static_cast<double>(r.ops);
    r.allocs = g_allocs;
    r.frees = g_frees;
    return r;
}

// ---------------------------------------------------------------------------
// Workload 3: best-quote hot loop on a populated book.
// ---------------------------------------------------------------------------
template <typename Book>
Result benchBestQuote(Book& book, std::size_t live_fill, std::size_t timed) {
    OrderId id = 1;
    for (std::size_t i = 0; i < live_fill; ++i) {
        book.addOrder(makeOrder(id++, randSide(), randPrice()));
    }

    // Warm the structure with a few quote reads.
    volatile Price sink = 0;
    for (std::size_t i = 0; i < 10000; ++i) {
        if (auto b = book.bestBid()) {
            sink += *b;
        }
        if (auto a = book.bestAsk()) {
            sink += *a;
        }
    }
    (void)sink;

    g_allocs = 0;
    g_frees = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < timed; ++i) {
        sink = 0;
        if (auto b = book.bestBid()) {
            sink += *b;
        }
        if (auto a = book.bestAsk()) {
            sink += *a;
        }
        (void)sink;
    }
    auto t1 = std::chrono::steady_clock::now();

    Result r;
    r.ops = timed;  // one "op" = one bestBid + one bestAsk
    r.ns_per_op = elapsedNs(t0, t1) / static_cast<double>(timed);
    r.allocs = g_allocs;
    r.frees = g_frees;
    return r;
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------
template <typename Fn>
static void report(Fn&& makeBook) {
    constexpr std::size_t kAddOnlyTimed = 2'000'000;
    constexpr std::size_t kAddOnlyWarmup = 500'000;
    constexpr std::size_t kMixTimed = 4'000'000;
    constexpr std::size_t kMixWarmup = 1'000'000;
    constexpr std::size_t kBestQuoteTimed = 10'000'000;
    constexpr std::size_t kLiveFill = 2'000;
    constexpr std::size_t kMaxLive = 4'000;

    {
        auto book = makeBook();  // factory returns std::unique_ptr<Book>
        Result r = benchAddOnly(*book, kAddOnlyWarmup, kAddOnlyTimed);
        std::printf(
            "  add-only        : %8.1f ns/op  %7.1fM ops/s  allocs=%zu  frees=%zu\n",
            r.ns_per_op, 1000.0 / r.ns_per_op, r.allocs, r.frees);
    }
    {
        auto book = makeBook();
        Result r = benchMix(*book, kLiveFill, kMaxLive, kMixWarmup, kMixTimed);
        std::printf(
            "  add/cancel mix  : %8.1f ns/op  %7.1fM ops/s  allocs=%zu  frees=%zu\n",
            r.ns_per_op, 1000.0 / r.ns_per_op, r.allocs, r.frees);
    }
    {
        auto book = makeBook();
        Result r = benchBestQuote(*book, kLiveFill, kBestQuoteTimed);
        std::printf(
            "  best-quote loop : %8.1f ns/op  %7.1fM ops/s  allocs=%zu  frees=%zu\n",
            r.ns_per_op, 1000.0 / r.ns_per_op, r.allocs, r.frees);
    }
}

static std::unique_ptr<OrderBook> makeCanonical() {
    return std::make_unique<OrderBook>();
}
static std::unique_ptr<FastOrderBook> makeFast() {
    auto book = std::make_unique<FastOrderBook>(kMinPrice, kMaxPrice);
    book->reserveOrders(16'384);
    return book;
}

int main() {
    std::printf("Phase 2B benchmark (price window [%lld, %lld])\n",
                static_cast<long long>(kMinPrice), static_cast<long long>(kMaxPrice));

    std::printf("OrderBook     (std::map + std::list + unordered_map):\n");
    report(makeCanonical);

    std::printf("FastOrderBook (arena pool + bounded price array):\n");
    report(makeFast);

    return 0;
}
