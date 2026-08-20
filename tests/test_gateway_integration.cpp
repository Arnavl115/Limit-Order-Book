#include "gateway/server.hpp"
#include "gateway/engine_host.hpp"
#include "gateway/frame.hpp"
#include "gateway/json.hpp"
#include "core/order_book.hpp"
#include "core/fast_order_book.hpp"

#include "test_framework.hpp"

#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace gateway;
using namespace lob;

// Client helpers (same as test_gateway.cpp)

static SOCKET clientConnect(uint16_t port, int retries = 30) {
    for (int i = 0; i < retries; ++i) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return s;
        ::closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return INVALID_SOCKET;
}
static bool clientSendJson(SOCKET s, const std::string& json) {
    auto b = frame::encode(json);
    size_t off = 0;
    while (off < b.size()) {
        int rc = ::send(s, b.data() + off, static_cast<int>(b.size() - off), 0);
        if (rc == SOCKET_ERROR) return false;
        off += static_cast<size_t>(rc);
    }
    return true;
}
static std::vector<std::string> clientRecvAll(SOCKET s, int totalTimeoutMs = 800) {
    std::vector<std::string> out;
    std::vector<char> buf;
    buf.reserve(8192);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        int remain = totalTimeoutMs - elapsed;
        if (remain <= 0) break;
        // use select with short timeout to allow checking for more data
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        timeval tv{0, 50000}; // 50ms poll
        // but respect remain
        if (remain < 50) { tv.tv_sec = 0; tv.tv_usec = remain * 1000; }
        int rc = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (rc <= 0) {
            // no data within poll, check if we have already got something and now idle -> break
            if (!out.empty()) {
                // if we have received at least one message and then 50ms idle, assume done
                // Actually we should continue until totalTimeoutMs idle
                // For now break if no data and we have something
                // Wait a bit more to ensure no more
                // We'll break if we have had 100ms of idle after last message
                // Simpler: if no data and elapsed > 200, break
                if (elapsed > 200 && !buf.empty()) {
                    // try decode any pending
                }
                // continue to check total timeout
                continue;
            } else {
                // no messages yet, keep waiting until totalTimeoutMs
                continue;
            }
        }
        char tmp[4096];
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.insert(buf.end(), tmp, tmp + n);
        // try decode all complete frames
        while (true) {
            auto dr = frame::tryDecode(buf.data(), buf.size());
            if (dr.status == frame::DecodeResult::Status::NeedMore) break;
            if (dr.status == frame::DecodeResult::Status::Error) {
                out.push_back("{\"type\":\"error\",\"reason\":\"" + dr.error + "\"}");
                buf.clear();
                break;
            }
            out.push_back(dr.payload);
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(dr.consumed));
        }
        // reset start if we got something? Not necessary, we want to collect all within totalTimeoutMs
        // Continue to collect more until timeout
    }
    return out;
}

// Helper to find message of type
static bool containsType(const std::vector<std::string>& msgs, const std::string& type) {
    for (auto& m : msgs) {
        auto j = JsonValue::parse(m);
        if (j && j->has("type") && j->get("type")->asString() == type) return true;
    }
    return false;
}
static std::vector<JsonValue> filterType(const std::vector<std::string>& msgs, const std::string& type) {
    std::vector<JsonValue> out;
    for (auto& m : msgs) {
        auto j = JsonValue::parse(m);
        if (j && j->has("type") && j->get("type")->asString() == type) out.push_back(*j);
    }
    return out;
}

TEST(gateway_integration_order_new_rests_and_snapshot) {
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& json, int sid){ return host.handleMessage(json, sid); });
    srv.setConnectHandler([&](int sid){ host.sendSnapshot(sid); });
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    SOCKET c = clientConnect(port);
    CHECK(c != INVALID_SOCKET);
    // On connect we should get snapshot (via connectHandler). Wait for it.
    auto snapMsgs = clientRecvAll(c, 500);
    CHECK(containsType(snapMsgs, "marketdata.snapshot"));
    // Place limit buy 100 qty10
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("order.new"));
    o.set("id", JsonValue(int64_t(1)));
    o.set("side", JsonValue("buy"));
    o.set("price", JsonValue(int64_t(100)));
    o.set("qty", JsonValue(int64_t(10)));
    o.set("orderType", JsonValue("limit"));
    CHECK(clientSendJson(c, o.stringify()));
    auto msgs = clientRecvAll(c, 500);
    // Should have execution.report new + resting + tick
    auto reports = filterType(msgs, "execution.report");
    CHECK(reports.size() >= 2);
    bool hasResting = false;
    for (auto& r : reports) if (r.get("status")->asString() == "resting") hasResting = true;
    CHECK(hasResting);
    CHECK(containsType(msgs, "marketdata.tick"));
    CHECK(book.contains(1));
    CHECK(book.bestBid().value() == 100);
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_integration_trade_broadcast) {
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& j, int id){ return host.handleMessage(j,id); });
    CHECK(srv.start(0));
    SOCKET c = clientConnect(srv.port());
    CHECK(c != INVALID_SOCKET);
    // Place resting sell 105 qty10
    JsonValue s1 = JsonValue::makeObject();
    s1.set("type", JsonValue("order.new"));
    s1.set("id", JsonValue(int64_t(1)));
    s1.set("side", JsonValue("sell"));
    s1.set("price", JsonValue(int64_t(105)));
    s1.set("qty", JsonValue(int64_t(10)));
    clientSendJson(c, s1.stringify());
    auto m1 = clientRecvAll(c, 400);
    (void)m1;
    // Place crossing buy 110 qty10 -> should trade
    JsonValue b1 = JsonValue::makeObject();
    b1.set("type", JsonValue("order.new"));
    b1.set("id", JsonValue(int64_t(2)));
    b1.set("side", JsonValue("buy"));
    b1.set("price", JsonValue(int64_t(110)));
    b1.set("qty", JsonValue(int64_t(10)));
    CHECK(clientSendJson(c, b1.stringify()));
    auto msgs = clientRecvAll(c, 500);
    CHECK(containsType(msgs, "trade"));
    auto trades = filterType(msgs, "trade");
    CHECK(trades.size() >= 1);
    CHECK(trades[0].get("price")->asInt() == 105); // maker price
    CHECK(trades[0].get("qty")->asInt() == 10);
    CHECK(trades[0].get("takerId")->asInt() == 2);
    CHECK(trades[0].get("makerId")->asInt() == 1);
    // execution reports: maker Filled, taker Filled
    auto reports = filterType(msgs, "execution.report");
    bool makerFilled = false, takerFilled = false;
    for (auto& r : reports) {
        if (r.get("orderId")->asInt() == 1 && r.get("status")->asString() == "filled") makerFilled = true;
        if (r.get("orderId")->asInt() == 2 && r.get("status")->asString() == "filled") takerFilled = true;
    }
    CHECK(makerFilled);
    CHECK(takerFilled);
    CHECK(book.empty());
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_integration_cancel) {
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& j,int id){return host.handleMessage(j,id);});
    CHECK(srv.start(0));
    SOCKET c = clientConnect(srv.port());
    CHECK(c != INVALID_SOCKET);
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("order.new"));
    o.set("id", JsonValue(int64_t(5)));
    o.set("side", JsonValue("buy"));
    o.set("price", JsonValue(int64_t(100)));
    o.set("qty", JsonValue(int64_t(10)));
    clientSendJson(c, o.stringify());
    clientRecvAll(c, 300);
    CHECK(book.contains(5));
    // cancel
    JsonValue canc = JsonValue::makeObject();
    canc.set("type", JsonValue("order.cancel"));
    canc.set("id", JsonValue(int64_t(5)));
    CHECK(clientSendJson(c, canc.stringify()));
    auto msgs = clientRecvAll(c, 400);
    auto reports = filterType(msgs, "execution.report");
    bool cancelled = false;
    for (auto& r : reports) if (r.get("orderId")->asInt()==5 && r.get("status")->asString()=="cancelled") cancelled = true;
    CHECK(cancelled);
    CHECK(!book.contains(5));
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_integration_cross_book_parity_via_gateway) {
    std::vector<std::string> traceOb;
    {
        OrderBook ob;
        Server srv1;
        EngineHost<OrderBook> host1(ob, srv1);
        srv1.setHandler([&](const std::string& j,int id){return host1.handleMessage(j,id);});
        CHECK(srv1.start(0));
        SOCKET c = clientConnect(srv1.port());
        CHECK(c != INVALID_SOCKET);
        std::vector<JsonValue> seq;
        JsonValue a = JsonValue::makeObject(); a.set("type", JsonValue("order.new")); a.set("id", JsonValue(int64_t(1))); a.set("side", JsonValue("sell")); a.set("price", JsonValue(int64_t(100))); a.set("qty", JsonValue(int64_t(5))); seq.push_back(a);
        JsonValue b = JsonValue::makeObject(); b.set("type", JsonValue("order.new")); b.set("id", JsonValue(int64_t(2))); b.set("side", JsonValue("sell")); b.set("price", JsonValue(int64_t(101))); b.set("qty", JsonValue(int64_t(5))); seq.push_back(b);
        JsonValue c1 = JsonValue::makeObject(); c1.set("type", JsonValue("order.new")); c1.set("id", JsonValue(int64_t(10))); c1.set("side", JsonValue("buy")); c1.set("price", JsonValue(int64_t(101))); c1.set("qty", JsonValue(int64_t(8))); seq.push_back(c1);
        for (auto& j : seq) {
            clientSendJson(c, j.stringify());
            auto msgs = clientRecvAll(c, 400);
            for (auto& m : msgs) traceOb.push_back(m);
        }
        ::closesocket(c);
        srv1.stop();
    }
    std::vector<std::string> traceFb;
    {
        FastOrderBook fb(1,1000);
        Server srv2;
        EngineHost<FastOrderBook> host2(fb, srv2);
        srv2.setHandler([&](const std::string& j,int id){return host2.handleMessage(j,id);});
        CHECK(srv2.start(0));
        SOCKET c = clientConnect(srv2.port());
        CHECK(c != INVALID_SOCKET);
        std::vector<JsonValue> seq;
        JsonValue a = JsonValue::makeObject(); a.set("type", JsonValue("order.new")); a.set("id", JsonValue(int64_t(1))); a.set("side", JsonValue("sell")); a.set("price", JsonValue(int64_t(100))); a.set("qty", JsonValue(int64_t(5))); seq.push_back(a);
        JsonValue b = JsonValue::makeObject(); b.set("type", JsonValue("order.new")); b.set("id", JsonValue(int64_t(2))); b.set("side", JsonValue("sell")); b.set("price", JsonValue(int64_t(101))); b.set("qty", JsonValue(int64_t(5))); seq.push_back(b);
        JsonValue c1 = JsonValue::makeObject(); c1.set("type", JsonValue("order.new")); c1.set("id", JsonValue(int64_t(10))); c1.set("side", JsonValue("buy")); c1.set("price", JsonValue(int64_t(101))); c1.set("qty", JsonValue(int64_t(8))); seq.push_back(c1);
        for (auto& j : seq) {
            clientSendJson(c, j.stringify());
            auto msgs = clientRecvAll(c, 400);
            for (auto& m : msgs) traceFb.push_back(m);
        }
        ::closesocket(c);
        srv2.stop();
    }

    // Compare ignoring seq numbers? But seq should be identical because same ops and both start at 1.
    // For this test, we compare type + key fields ignoring seq/broadcast seq
    CHECK(traceOb.size() == traceFb.size());
    for (size_t i=0;i<std::min(traceOb.size(), traceFb.size());++i) {
        auto jo = JsonValue::parse(traceOb[i]);
        auto jf = JsonValue::parse(traceFb[i]);
        CHECK(jo.has_value() && jf.has_value());
        if (!jo || !jf) continue;
        CHECK(jo->get("type")->asString() == jf->get("type")->asString());
        // For trade, compare price/qty/taker/maker/side
        if (jo->get("type")->asString() == "trade") {
            CHECK(jo->get("price")->asInt() == jf->get("price")->asInt());
            CHECK(jo->get("qty")->asInt() == jf->get("qty")->asInt());
            CHECK(jo->get("takerId")->asInt() == jf->get("takerId")->asInt());
            CHECK(jo->get("makerId")->asInt() == jf->get("makerId")->asInt());
        }
        // For reports, compare orderId/status
        if (jo->get("type")->asString() == "execution.report") {
            CHECK(jo->get("orderId")->asInt() == jf->get("orderId")->asInt());
            CHECK(jo->get("status")->asString() == jf->get("status")->asString());
        }
    }
}
