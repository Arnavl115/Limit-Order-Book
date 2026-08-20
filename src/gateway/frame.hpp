#pragma once

// Length-prefixed framing for TCP transport (bot, tests).
// Wire format: [4 bytes big-endian uint32 N][N bytes UTF-8 JSON]
// Max N = 1 MiB (1_048_576). Zero-length is invalid.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gateway {
namespace frame {

constexpr std::size_t kMaxFrameBytes = 1u << 20; // 1 MiB

// Encode JSON string into length-prefixed bytes (BE)
[[nodiscard]] std::vector<char> encode(std::string_view json);

// Decode result for streaming buffer
struct DecodeResult {
    enum class Status { NeedMore, Ok, Error } status = Status::NeedMore;
    std::size_t consumed = 0; // bytes consumed from input (4+N) when Ok
    std::string payload;      // JSON string when Ok
    std::string error;        // when Error
};

// Try to decode one frame from [data, data+len). Does not modify input.
// NeedMore: not enough bytes yet (need at least 4, then 4+N)
// Ok: payload filled, consumed = 4+N
// Error: framing error (zero length, >kMax, etc.) — caller should close connection
[[nodiscard]] DecodeResult tryDecode(const char* data, std::size_t len) noexcept;

// Helper: decode all complete frames from a buffer, removing consumed prefix.
// Returns vector of payloads; on Error, returns error string and leaves buffer as-is? Instead we return error via outError.
// This is a convenience for tests — it loops tryDecode.
[[nodiscard]] std::vector<std::string> decodeAll(std::vector<char>& buf, std::string* outError = nullptr);

} // namespace frame
} // namespace gateway
