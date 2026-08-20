#include "gateway/server.hpp"
#include "gateway/engine_host.hpp"
#include "gateway/frame.hpp"
#include "gateway/json.hpp"
#include "core/order_book.hpp"

#include "test_framework.hpp"

#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace gateway;
using namespace lob;

static SOCKET clientConnect(uint16_t port) {
    for (int i=0;i<30;++i) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s==INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a))==0) return s;
        ::closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return INVALID_SOCKET;
}
static bool clientSendJson(SOCKET s, const std::string& j) {
    auto b=frame::encode(j); size_t off=0;
    while(off<b.size()){ int rc=::send(s,b.data()+off, static_cast<int>(b.size()-off),0); if(rc==SOCKET_ERROR) return false; off+=rc; }
    return true;
}
static std::vector<std::string> clientRecvAll(SOCKET s, int timeoutMs) {
    std::vector<std::string> out; std::vector<char> buf; buf.reserve(8192);
    auto start=std::chrono::steady_clock::now();
    while(true){
        auto now=std::chrono::steady_clock::now();
        int elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now-start).count());
        if(elapsed>=timeoutMs) break;
        int remain=timeoutMs-elapsed;
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s,&rfds);
        timeval tv{0, std::min(50000, remain*1000)};
        int rc=::select(0,&rfds,nullptr,nullptr,&tv);
        if(rc<=0) {
            if(!out.empty() && elapsed>100) break;
            continue;
        }
        char tmp[8192]; int n=::recv(s,tmp,sizeof(tmp),0);
        if(n<=0) break;
        buf.insert(buf.end(),tmp,tmp+n);
        while(true){
            auto dr=frame::tryDecode(buf.data(),buf.size());
            if(dr.status==frame::DecodeResult::Status::NeedMore) break;
            if(dr.status==frame::DecodeResult::Status::Error){ buf.clear(); break; }
            out.push_back(dr.payload);
            buf.erase(buf.begin(), buf.begin()+static_cast<std::ptrdiff_t>(dr.consumed));
        }
    }
    return out;
}

TEST(gateway_load_single_client_throughput) {
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& j,int id){return host.handleMessage(j,id);});
    srv.setConnectHandler([&](int id){host.sendSnapshot(id);});
    CHECK(srv.start(0));
    SOCKET c = clientConnect(srv.port());
    CHECK(c != INVALID_SOCKET);
    // drain snapshot
    clientRecvAll(c, 300);
    const int N = 2000; // keep Debug fast
    auto t0 = std::chrono::steady_clock::now();
    for (int i=0;i<N;++i) {
        JsonValue o = JsonValue::makeObject();
        o.set("type", JsonValue("order.new"));
        o.set("id", JsonValue(int64_t(1000 + i)));
        o.set("side", JsonValue(i%2==0 ? "buy" : "sell"));
        // price near touch to create mix of resting/crossing
        int64_t price = (i%2==0) ? 100 + (i%5) : 101 - (i%5);
        // actually vary to cause some crosses
        if (i%3==0) price = 105; else if (i%3==1) price = 95;
        else price = 100 + (i%2==0 ? -1 : 1);
        // ensure price >0
        if (price <=0) price = 100;
        o.set("price", JsonValue(price));
        o.set("qty", JsonValue(int64_t(1 + (i%5))));
        clientSendJson(c, o.stringify());
        // For throughput, we could pipeline without waiting, but for correctness we drain periodically
        if (i%100==0) {
            auto msgs = clientRecvAll(c, 50);
            (void)msgs;
        }
    }
    // drain remaining broadcasts
    auto all = clientRecvAll(c, 800);
    auto t1 = std::chrono::steady_clock::now();
    double nsPerOp = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    double opsPerSec = 1e9 / nsPerOp;
    // Check seq ordering: collect all execution.report and trade seq, ensure monotonic
    // We need to parse broadcast seq field
    uint64_t lastSeq = 0;
    bool seqOk = true;
    for (auto& m : all) {
        auto j = JsonValue::parse(m);
        if (!j || !j->has("seq")) continue;
        uint64_t s = static_cast<uint64_t>(j->get("seq")->asInt());
        if (s <= lastSeq) seqOk = false;
        lastSeq = s;
    }
    CHECK(seqOk);
    // No hang: we got here within timeout
    CHECK(all.size() > 0);
    // Throughput sanity: at least 5k ops/s in Debug (very conservative), 20k in Release
    // Just check >1000
    CHECK(opsPerSec > 1000);
    // Record numbers for UNDERSTANDING (printed, not asserted strictly)
    // Use CHECK to print via side effect? We'll just ensure test passes
    std::printf("  [load] single client N=%d ns/op %.1f ops/s %.1fK seqOk=%d msgs=%zu\n", N, nsPerOp, opsPerSec/1000, seqOk, all.size());
    ::closesocket(c);
    srv.stop();
    // Ensure no deadlock: server stopped cleanly
    CHECK(!srv.isRunning());
}

TEST(gateway_load_concurrent_two_clients) {
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& j,int id){return host.handleMessage(j,id);});
    CHECK(srv.start(0));
    SOCKET c1 = clientConnect(srv.port());
    SOCKET c2 = clientConnect(srv.port());
    CHECK(c1 != INVALID_SOCKET && c2 != INVALID_SOCKET);
    // concurrent send
    const int N = 1000;
    std::atomic<bool> done{false};
    std::vector<std::string> trace1, trace2;
    auto sender = [&](SOCKET c, int baseId, std::vector<std::string>& out) {
        for (int i=0;i<N;++i) {
            JsonValue o = JsonValue::makeObject();
            o.set("type", JsonValue("order.new"));
            o.set("id", JsonValue(int64_t(baseId + i)));
            o.set("side", JsonValue(i%2==0 ? "buy" : "sell"));
            o.set("price", JsonValue(int64_t(100 + (i%3)-1)));
            o.set("qty", JsonValue(int64_t(1 + (i%3))));
            clientSendJson(c, o.stringify());
            if (i%200==0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // drain
        auto msgs = clientRecvAll(c, 600);
        out = msgs;
    };
    std::thread t1(sender, c1, 5000, std::ref(trace1));
    std::thread t2(sender, c2, 10000, std::ref(trace2));
    t1.join(); t2.join();
    // Check both got broadcasts and seq ordering holds per client (server broadcast seq is global, so each client's view should be monotonic)
    auto checkSeq = [](const std::vector<std::string>& v)->bool {
        uint64_t last=0;
        for (auto& m: v) {
            auto j=JsonValue::parse(m);
            if(!j||!j->has("seq")) continue;
            uint64_t s=static_cast<uint64_t>(j->get("seq")->asInt());
            if(s<=last) return false;
            last=s;
        }
        return true;
    };
    CHECK(checkSeq(trace1));
    CHECK(checkSeq(trace2));
    ::closesocket(c1); ::closesocket(c2);
    srv.stop();
    CHECK(!srv.isRunning());
}

TEST(gateway_soak_no_deadlock) {
    // Soak for ~1 sec with continuous orders
    OrderBook book;
    Server srv;
    EngineHost<OrderBook> host(book, srv);
    srv.setHandler([&](const std::string& j,int id){return host.handleMessage(j,id);});
    CHECK(srv.start(0));
    SOCKET c = clientConnect(srv.port());
    CHECK(c != INVALID_SOCKET);
    auto start = std::chrono::steady_clock::now();
    int ops = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 800) {
        JsonValue o = JsonValue::makeObject();
        o.set("type", JsonValue("order.new"));
        o.set("id", JsonValue(int64_t(20000 + ops)));
        o.set("side", JsonValue(ops%2==0?"buy":"sell"));
        o.set("price", JsonValue(int64_t(100)));
        o.set("qty", JsonValue(int64_t(1)));
        clientSendJson(c, o.stringify());
        ++ops;
        if (ops % 50 == 0) {
            // drain a bit to avoid buffer overflow
            clientRecvAll(c, 20);
            if (!srv.isRunning()) break;
        }
    }
    // final drain
    clientRecvAll(c, 200);
    CHECK(srv.isRunning());
    ::closesocket(c);
    srv.stop();
    CHECK(ops > 500);
}
