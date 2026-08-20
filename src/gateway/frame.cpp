#include "frame.hpp"

#include <cassert>
#include <cstring>

namespace gateway {
namespace frame {

std::vector<char> encode(std::string_view json) {
    std::vector<char> out;
    out.reserve(4 + json.size());
    std::uint32_t n = static_cast<std::uint32_t>(json.size());
    // big-endian
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

DecodeResult tryDecode(const char* data, std::size_t len) noexcept {
    DecodeResult r;
    if (len < 4) {
        r.status = DecodeResult::Status::NeedMore;
        return r;
    }
    std::uint32_t n = (static_cast<unsigned char>(data[0]) << 24) |
                      (static_cast<unsigned char>(data[1]) << 16) |
                      (static_cast<unsigned char>(data[2]) << 8) |
                      static_cast<unsigned char>(data[3]);
    if (n == 0) {
        r.status = DecodeResult::Status::Error;
        r.error = "zero-length frame";
        return r;
    }
    if (n > kMaxFrameBytes) {
        r.status = DecodeResult::Status::Error;
        r.error = "frame too large";
        return r;
    }
    if (len < 4 + static_cast<std::size_t>(n)) {
        r.status = DecodeResult::Status::NeedMore;
        return r;
    }
    r.status = DecodeResult::Status::Ok;
    r.consumed = 4 + n;
    r.payload.assign(data + 4, data + 4 + n);
    return r;
}

std::vector<std::string> decodeAll(std::vector<char>& buf, std::string* outError) {
    std::vector<std::string> payloads;
    size_t offset = 0;
    while (true) {
        if (buf.size() - offset < 4) break;
        DecodeResult dr = tryDecode(buf.data() + offset, buf.size() - offset);
        if (dr.status == DecodeResult::Status::NeedMore) break;
        if (dr.status == DecodeResult::Status::Error) {
            if (outError) *outError = dr.error;
            // do not consume on error — caller should handle
            return payloads;
        }
        payloads.push_back(std::move(dr.payload));
        offset += dr.consumed;
    }
    if (offset > 0) {
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return payloads;
}

} // namespace frame
} // namespace gateway
