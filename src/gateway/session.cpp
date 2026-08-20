#include "session.hpp"
#include "server.hpp"
#include "frame.hpp"
#include "json.hpp"
#include "ws_util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace gateway {

Session::Session(SOCKET sock, int id, Server* server)
    : sock_(sock), id_(id), server_(server) {}

Session::~Session() {
    stop();
    if (sock_ != INVALID_SOCKET) {
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void Session::start() {
    running_.store(true);
    th_ = std::thread(&Session::run, this);
}

void Session::stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        if (sock_ != INVALID_SOCKET) {
            ::shutdown(sock_, SD_BOTH);
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }
    if (th_.joinable()) {
        if (std::this_thread::get_id() != th_.get_id())
            th_.join();
        else
            th_.detach();
    }
}

bool Session::sendFrame(const std::string& json) {
    if (sock_ == INVALID_SOCKET) return false;
    std::vector<char> bytes;
    if (isWebSocket_) {
        bytes = ws::encodeTextFrame(json);
    } else {
        bytes = frame::encode(json);
    }
    size_t sent = 0;
    while (sent < bytes.size()) {
        int rc = ::send(sock_, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
        if (rc == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

static bool sendAllRaw(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int rc = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (rc == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

static std::string toLowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string getHeaderValue(const std::string& headers, const std::string& name) {
    std::string lname = toLowerCopy(name);
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string hname = toLowerCopy(line.substr(0, colon));
        // trim spaces
        size_t start = hname.find_first_not_of(" \t");
        size_t end = hname.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        hname = hname.substr(start, end - start + 1);
        if (hname == lname) {
            std::string val = line.substr(colon + 1);
            size_t vs = val.find_first_not_of(" \t");
            size_t ve = val.find_last_not_of(" \t\r");
            if (vs == std::string::npos) return "";
            return val.substr(vs, ve - vs + 1);
        }
    }
    return "";
}

bool Session::handleHttp(const std::vector<char>& buf, size_t& consumed) {
    // buf starts with "GET", contains "\r\n\r\n"
    std::string req(buf.data(), buf.size());
    size_t hdrEnd = req.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false; // need more
    std::string headerPart = req.substr(0, hdrEnd);
    consumed = hdrEnd + 4;
    // parse request line
    std::istringstream firstLineStream(headerPart);
    std::string requestLine;
    std::getline(firstLineStream, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
    std::istringstream rl(requestLine);
    std::string method, path, version;
    rl >> method >> path >> version;
    // check for WebSocket upgrade
    std::string upgrade = getHeaderValue(headerPart, "Upgrade");
    std::string connection = getHeaderValue(headerPart, "Connection");
    std::string wsKey = getHeaderValue(headerPart, "Sec-WebSocket-Key");
    std::string upgradeLower = toLowerCopy(upgrade);
    std::string connLower = toLowerCopy(connection);
    bool isWs = (upgradeLower == "websocket" && connLower.find("upgrade") != std::string::npos && !wsKey.empty() && path == "/ws");
    if (isWs) {
        std::string accept = ws::computeAcceptKey(wsKey);
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        sendAllRaw(sock_, resp.data(), resp.size());
        isWebSocket_ = true;
        return true; // keep open, now WS mode
    }
    // Not WebSocket — serve static
    // Normalize path
    if (path == "/") path = "/index.html";
    // Map to filesystem
    std::string fsPath;
    if (path == "/index.html" || path == "/app.js" || path == "/style.css") {
        // try frontend dir relative to cwd and absolute fallback
        std::vector<std::string> candidates = {
            "frontend" + path,
            "C:/Users/arnav/project2/frontend" + path
        };
        for (auto& c : candidates) {
            std::ifstream f(c, std::ios::binary);
            if (f) { fsPath = c; break; }
        }
    }
    std::string body;
    std::string contentType = "text/plain";
    int status = 200;
    std::string statusText = "OK";
    if (!fsPath.empty()) {
        std::ifstream f(fsPath, std::ios::binary);
        body.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (path.size() >= 5 && path.substr(path.size()-5)==".html") contentType = "text/html";
        else if (path.size() >= 3 && path.substr(path.size()-3)==".js") contentType = "application/javascript";
        else if (path.size() >= 4 && path.substr(path.size()-4)==".css") contentType = "text/css";
    } else {
        if (path == "/index.html") {
            // placeholder if not found
            body = "<!doctype html><html><head><title>LOB</title></head><body><h1>LOB Gateway</h1><p>No frontend built. Use WebSocket at /ws</p></body></html>";
            contentType = "text/html";
        } else {
            status = 404; statusText = "Not Found";
            body = "Not Found";
            contentType = "text/plain";
        }
    }
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
                       "Content-Type: " + contentType + "\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + body;
    sendAllRaw(sock_, resp.data(), resp.size());
    return false; // close after HTTP
}

bool Session::handleWebSocketLoop(std::vector<char>& buf) {
    while (true) {
        auto dr = ws::tryDecodeFrame(buf.data(), buf.size());
        if (dr.status == ws::WsDecodeResult::Status::NeedMore) return true; // need more data, keep connection
        if (dr.status == ws::WsDecodeResult::Status::Error) {
            // protocol error -> close
            return false;
        }
        // consume
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(dr.consumed));
        if (dr.isClose) {
            return false;
        }
        if (dr.isPing) {
            // reply pong
            std::vector<char> pong;
            pong.reserve(2 + dr.pingPayload.size());
            pong.push_back(static_cast<char>(0x8A)); // FIN + pong
            size_t len = dr.pingPayload.size();
            if (len < 126) pong.push_back(static_cast<char>(len));
            else if (len <= 0xFFFF) { pong.push_back(126); pong.push_back((len>>8)&0xFF); pong.push_back(len&0xFF); }
            else { pong.push_back(127); for(int i=7;i>=0;--i) pong.push_back((len>>(i*8))&0xFF); }
            pong.insert(pong.end(), dr.pingPayload.begin(), dr.pingPayload.end());
            sendAllRaw(sock_, pong.data(), pong.size());
            continue;
        }
        // text frame payload is JSON
        std::string payload = dr.payload;
        std::string errStr;
        auto jv = JsonValue::parse(payload, &errStr);
        std::string response;
        if (!jv) {
            JsonValue err = JsonValue::makeObject();
            err.set("type", JsonValue("error"));
            err.set("reason", JsonValue(errStr.empty() ? "invalid_json" : errStr));
            response = err.stringify();
        } else {
            if (server_) response = server_->handleRequest(payload, id_);
            else response = payload;
        }
        if (!response.empty()) {
            auto out = ws::encodeTextFrame(response);
            if (!sendAllRaw(sock_, out.data(), out.size())) return false;
        }
        // continue to next frame if buffered
    }
}

void Session::run() {
    std::vector<char> buf;
    buf.reserve(8192);
    char tmp[4096];
    bool httpChecked = false;

    while (running_.load()) {
        // First, try to process any buffered data without blocking
        bool progressed = false;
        if (!httpChecked && !isWebSocket_ && buf.size() >= 3 && std::memcmp(buf.data(), "GET", 3) == 0) {
            std::string s(buf.data(), buf.size());
            size_t pos = s.find("\r\n\r\n");
            if (pos != std::string::npos) {
                size_t consumed = 0;
                bool keep = handleHttp(buf, consumed);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
                httpChecked = true;
                if (!keep) break; // HTTP static done
                progressed = true;
                // isWebSocket_ may now be true, fall through to WS handling
            } else if (buf.size() > 8192) {
                break;
            }
        } else if (!httpChecked && !isWebSocket_ && !buf.empty()) {
            // Not HTTP, mark as checked so future data treated as frames
            // Only mark if we have enough to decide: if first bytes are not GET, it's length-prefix
            // We wait until we have at least 1 byte and it's not 'G'
            if (buf[0] != 'G') httpChecked = true;
        }

        if (isWebSocket_ && !buf.empty()) {
            // try to handle WS frames
            size_t before = buf.size();
            bool keep = handleWebSocketLoop(buf);
            if (!keep) break;
            if (buf.size() != before) progressed = true;
            if (progressed) continue; // more buffered frames may be available, loop again without recv
        }

        if (!isWebSocket_ && httpChecked) {
            bool any = false;
            while (true) {
                auto dr = frame::tryDecode(buf.data(), buf.size());
                if (dr.status == frame::DecodeResult::Status::NeedMore) break;
                if (dr.status == frame::DecodeResult::Status::Error) {
                    JsonValue err = JsonValue::makeObject();
                    err.set("type", JsonValue("error"));
                    err.set("reason", JsonValue(dr.error));
                    std::string errJson = err.stringify();
                    auto errBytes = frame::encode(errJson);
                    sendAllRaw(sock_, errBytes.data(), errBytes.size());
                    running_.store(false);
                    any = true;
                    break;
                }
                std::string payload = std::move(dr.payload);
                buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(dr.consumed));
                std::string errStr;
                auto jv = JsonValue::parse(payload, &errStr);
                std::string response;
                if (!jv) {
                    JsonValue err = JsonValue::makeObject();
                    err.set("type", JsonValue("error"));
                    err.set("reason", JsonValue(errStr.empty() ? "invalid_json" : errStr));
                    response = err.stringify();
                } else {
                    if (server_) response = server_->handleRequest(payload, id_);
                    else response = payload;
                }
                if (!response.empty()) {
                    auto outBytes = frame::encode(response);
                    if (!sendAllRaw(sock_, outBytes.data(), outBytes.size())) {
                        running_.store(false);
                        break;
                    }
                }
                any = true;
            }
            if (any) {
                progressed = true;
                continue;
            }
        }

        if (progressed) continue;

        // Need more data -> block on recv
        int rc = ::recv(sock_, tmp, sizeof(tmp), 0);
        if (rc <= 0) break;
        buf.insert(buf.end(), tmp, tmp + rc);
    }
    running_.store(false);
    if (server_) server_->removeSession(id_);
    if (sock_ != INVALID_SOCKET) {
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

} // namespace gateway
