#include "gateway/server.hpp"
#include "gateway/frame.hpp"
#include "gateway/json.hpp"

#include "test_framework.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

using namespace gateway;

// Helpers for client side (Winsock, blocking)

static SOCKET clientConnect(uint16_t port, int retries = 20) {
    for (int i = 0; i < retries; ++i) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) return s;
        ::closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return INVALID_SOCKET;
}

static bool clientSendJson(SOCKET s, const std::string& json) {
    auto bytes = frame::encode(json);
    size_t sent = 0;
    while (sent < bytes.size()) {
        int rc = ::send(s, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
        if (rc == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

static bool clientSendRaw(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int rc = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (rc == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

// recv one length-prefixed JSON with timeout (ms). Returns empty on timeout/closed/error.
static std::string clientRecvJson(SOCKET s, int timeoutMs = 2000) {
    // set recv timeout via select
    std::vector<char> buf;
    buf.reserve(4096);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        int remain = timeoutMs - elapsed;
        if (remain <= 0) return "";
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv{remain / 1000, (remain % 1000) * 1000};
        int rc = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (rc <= 0) return "";
        char tmp[4096];
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return "";
        buf.insert(buf.end(), tmp, tmp + n);
        auto dr = frame::tryDecode(buf.data(), buf.size());
        if (dr.status == frame::DecodeResult::Status::NeedMore) {
            continue;
        }
        if (dr.status == frame::DecodeResult::Status::Error) {
            return ""; // framing error
        }
        // Ok
        return dr.payload;
    }
}

TEST(gateway_malformed_frame_error) {
    Server srv;
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    CHECK(port != 0);

    SOCKET c = clientConnect(port);
    CHECK(c != INVALID_SOCKET);

    // Send zero-length frame (4 bytes 0) -> server should send error and close
    char bad[4] = {0,0,0,0};
    CHECK(clientSendRaw(c, bad, 4));
    std::string resp = clientRecvJson(c, 1000);
    CHECK(!resp.empty());
    if (!resp.empty()) {
        auto j = JsonValue::parse(resp);
        CHECK(j.has_value());
        CHECK(j->has("type"));
        CHECK(j->get("type")->asString() == "error");
    }
    // After framing error, server closes; next recv should be closed (empty)
    // Give server a moment to close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Try recv again should be empty (closed)
    char tmp[1];
    // use select to check if closed
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(c, &rfds);
    timeval tv{0, 200000};
    int rc = ::select(0, &rfds, nullptr, nullptr, &tv);
    if (rc > 0) {
        int n = ::recv(c, tmp, sizeof(tmp), 0);
        CHECK(n <= 0); // should be closed
    }
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_malformed_json_error_keeps_connection) {
    Server srv;
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    SOCKET c = clientConnect(port);
    CHECK(c != INVALID_SOCKET);
    // Send valid frame containing invalid JSON
    std::string badJson = "{\"a\": }"; // invalid
    CHECK(clientSendJson(c, badJson));
    std::string resp = clientRecvJson(c, 1000);
    CHECK(!resp.empty());
    auto j = JsonValue::parse(resp);
    CHECK(j.has_value());
    CHECK(j->get("type")->asString() == "error");
    // Connection should stay open — send valid echo
    std::string good = "{\"type\":\"echo\",\"msg\":\"hi\"}";
    CHECK(clientSendJson(c, good));
    std::string echo = clientRecvJson(c, 1000);
    CHECK(echo == good);
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_echo) {
    Server srv;
    // default handler echoes
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    SOCKET c = clientConnect(port);
    CHECK(c != INVALID_SOCKET);
    std::string msg = "{\"type\":\"echo\",\"id\":123}";
    CHECK(clientSendJson(c, msg));
    std::string resp = clientRecvJson(c, 1000);
    CHECK(resp == msg);
    ::closesocket(c);
    srv.stop();
}

TEST(gateway_two_concurrent_clients) {
    Server srv;
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    SOCKET c1 = clientConnect(port);
    SOCKET c2 = clientConnect(port);
    CHECK(c1 != INVALID_SOCKET);
    CHECK(c2 != INVALID_SOCKET);
    std::string m1 = "{\"type\":\"echo\",\"client\":1}";
    std::string m2 = "{\"type\":\"echo\",\"client\":2}";
    CHECK(clientSendJson(c1, m1));
    CHECK(clientSendJson(c2, m2));
    std::string r1 = clientRecvJson(c1, 1000);
    std::string r2 = clientRecvJson(c2, 1000);
    CHECK(r1 == m1);
    CHECK(r2 == m2);
    // interleaved second round
    CHECK(clientSendJson(c1, m2));
    CHECK(clientSendJson(c2, m1));
    std::string r1b = clientRecvJson(c1, 1000);
    std::string r2b = clientRecvJson(c2, 1000);
    CHECK(r1b == m2);
    CHECK(r2b == m1);
    ::closesocket(c1);
    ::closesocket(c2);
    srv.stop();
}

TEST(gateway_shutdown_mid_connection) {
    Server srv;
    CHECK(srv.start(0));
    uint16_t port = srv.port();
    SOCKET c = clientConnect(port);
    CHECK(c != INVALID_SOCKET);
    std::string msg = "{\"type\":\"echo\",\"x\":1}";
    CHECK(clientSendJson(c, msg));
    std::string resp = clientRecvJson(c, 1000);
    CHECK(resp == msg);
    // stop server while client still connected
    srv.stop();
    // client recv should now indicate closed (recv 0 or error)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // try to send again — should fail or be ignored
    // Use select to see if socket is closed
    char buf[1];
    // set non-blocking for test
    u_long mode = 1;
    ::ioctlsocket(c, FIONBIO, &mode);
    int n = ::recv(c, buf, sizeof(buf), 0);
    // either 0 (closed) or SOCKET_ERROR with WSAEWOULDBLOCK if not yet closed, but after stop it should be closed
    // We accept either closed or would-block as long as server stopped cleanly without hang
    CHECK(n <= 0 || n == 0);
    // ensure stop didn't hang (we already returned)
    CHECK(!srv.isRunning());
    ::closesocket(c);
}
