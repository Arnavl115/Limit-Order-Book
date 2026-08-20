#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gateway {
namespace ws {

// Compute Sec-WebSocket-Accept: base64(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
[[nodiscard]] std::string computeAcceptKey(const std::string& clientKey);

// SHA1 20-byte digest
[[nodiscard]] std::array<uint8_t,20> sha1(const std::string& data);
[[nodiscard]] std::array<uint8_t,20> sha1(const uint8_t* data, size_t len);

// Base64 encode (no line breaks, pad with =)
[[nodiscard]] std::string base64Encode(const uint8_t* data, size_t len);
[[nodiscard]] std::string base64Encode(const std::array<uint8_t,20>& digest);

// WebSocket frame helpers (server side)
// Encode text frame server->client (no mask)
[[nodiscard]] std::vector<char> encodeTextFrame(const std::string& payload);
// Try decode one client->server frame (masked). Returns NeedMore if incomplete, Ok with payload, Error on protocol violation.
struct WsDecodeResult {
    enum class Status { NeedMore, Ok, Error } status = Status::NeedMore;
    size_t consumed = 0;
    std::string payload; // for text frame
    bool isClose = false;
    bool isPing = false;
    std::string pingPayload;
    std::string error;
};
[[nodiscard]] WsDecodeResult tryDecodeFrame(const char* data, size_t len) noexcept;

} // namespace ws
} // namespace gateway
