#pragma once

// Phase 4C — TCP server (Winsock) with accept thread + per-connection workers.
// For 4C it speaks length-prefixed JSON (4B BE len + JSON). In 4D it also
// multiplexes HTTP/WebSocket on the same port (detects "GET "). Clean shutdown,
// connection registry, backpressure policy (see docs/protocol.md).

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#include "session.hpp"

namespace gateway {

class Server {
public:
    using Handler = std::function<std::string(const std::string& json, int sessionId)>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start listening on 127.0.0.1:port (port 0 = ephemeral). Returns false on bind/listen failure.
    bool start(uint16_t port = 0);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] size_t clientCount() const;

    void setHandler(Handler h) { handler_ = std::move(h); }
    std::string handleRequest(const std::string& json, int sessionId) {
        if (handler_) return handler_(json, sessionId);
        return json; // echo
    }
    using ConnectHandler = std::function<void(int sessionId)>;
    void setConnectHandler(ConnectHandler h) { connectHandler_ = std::move(h); }

    // Broadcast JSON to all connected sessions (length-prefixed). Used in 4D.
    void broadcast(const std::string& json);

    // For tests: send to specific session
    bool sendTo(int sessionId, const std::string& json);
    void removeSession(int sessionId);

private:
    void acceptLoop();
    void handleClient(SOCKET clientSock, int sessionId);

    bool wsaInited_ = false;
    SOCKET listenSock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    mutable std::mutex clientsMutex_;
    std::map<int, std::shared_ptr<Session>> sessions_; // id -> session
    Handler handler_;
    ConnectHandler connectHandler_;
    int nextId_ = 1;
};

} // namespace gateway
