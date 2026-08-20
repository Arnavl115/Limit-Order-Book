#pragma once

// Phase 4C — Session handle (per-connection state).
// Kept minimal for 4C; extended in 4D for WebSocket/HTTP.
// Server owns Sessions; each Session runs its own worker thread.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace gateway {

class Server; // forward

class Session {
public:
    Session(SOCKET sock, int id, Server* server);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start();
    void stop(); // close socket, join thread
    bool sendFrame(const std::string& json); // framed (WS or length-prefix) + send

    int id() const noexcept { return id_; }
    SOCKET socket() const noexcept { return sock_; }
    bool isWebSocket() const noexcept { return isWebSocket_; }

private:
    void run();
    bool handleHttp(const std::vector<char>& buf, size_t& consumed);
    bool handleWebSocketLoop(std::vector<char>& buf);

    SOCKET sock_ = INVALID_SOCKET;
    int id_ = 0;
    Server* server_ = nullptr;
    std::thread th_;
    std::atomic<bool> running_{false};
    bool isWebSocket_ = false;
};

} // namespace gateway
