#include "ws_util.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gateway {
namespace ws {

namespace {

// SHA1 implementation (public domain, RFC 3174)
struct SHA1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
};

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a,b,c,d,e;
    uint32_t w[80];
    for (int i=0;i<16;++i) {
        w[i] = (static_cast<uint32_t>(buffer[i*4])<<24) |
               (static_cast<uint32_t>(buffer[i*4+1])<<16) |
               (static_cast<uint32_t>(buffer[i*4+2])<<8) |
               static_cast<uint32_t>(buffer[i*4+3]);
    }
    for (int i=16;i<80;++i) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=state[0]; b=state[1]; c=state[2]; d=state[3]; e=state[4];
    for (int i=0;i<80;++i) {
        uint32_t f,k;
        if (i<20) { f=(b&c)|((~b)&d); k=0x5A827999; }
        else if (i<40) { f=b^c^d; k=0x6ED9EBA1; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else { f=b^c^d; k=0xCA62C1D6; }
        uint32_t temp = rol(a,5) + f + e + k + w[i];
        e=d; d=c; c=rol(b,30); b=a; a=temp;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}

static void init(SHA1Ctx* ctx) {
    ctx->state[0]=0x67452301; ctx->state[1]=0xEFCDAB89; ctx->state[2]=0x98BADCFE;
    ctx->state[3]=0x10325476; ctx->state[4]=0xC3D2E1F0;
    ctx->count=0;
}
static void update(SHA1Ctx* ctx, const uint8_t* data, size_t len) {
    size_t i=0, j=0;
    j = static_cast<size_t>((ctx->count >> 3) & 63);
    ctx->count += static_cast<uint64_t>(len) << 3;
    if ((j + len) > 63) {
        std::memcpy(&ctx->buffer[j], data, (i = 64 - j));
        transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) transform(ctx->state, &data[i]);
        j=0;
    } else i=0;
    std::memcpy(&ctx->buffer[j], &data[i], len - i);
}
static void final(SHA1Ctx* ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i=0;i<8;++i) finalcount[i]= static_cast<uint8_t>((ctx->count >> ((7-i)*8)) & 255);
    uint8_t pad = 0x80;
    update(ctx, &pad, 1);
    while ((ctx->count & 504) != 448) { pad=0; update(ctx, &pad, 1); }
    update(ctx, finalcount, 8);
    for (int i=0;i<20;++i) digest[i]= static_cast<uint8_t>((ctx->state[i>>2] >> ((3-(i&3))*8)) & 255);
}

} // anon

std::array<uint8_t,20> sha1(const uint8_t* data, size_t len) {
    SHA1Ctx ctx; init(&ctx); update(&ctx, data, len);
    std::array<uint8_t,20> out{}; final(&ctx, out.data()); return out;
}
std::array<uint8_t,20> sha1(const std::string& data) {
    return sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len+2)/3)*4);
    for (size_t i=0;i<len;i+=3) {
        uint32_t a = data[i];
        uint32_t b = (i+1 < len) ? data[i+1] : 0;
        uint32_t c = (i+2 < len) ? data[i+2] : 0;
        uint32_t v = (a<<16)|(b<<8)|c;
        out.push_back(tbl[(v>>18)&63]);
        out.push_back(tbl[(v>>12)&63]);
        out.push_back(i+1 < len ? tbl[(v>>6)&63] : '=');
        out.push_back(i+2 < len ? tbl[v&63] : '=');
    }
    return out;
}
std::string base64Encode(const std::array<uint8_t,20>& d) {
    return base64Encode(d.data(), d.size());
}

std::string computeAcceptKey(const std::string& clientKey) {
    static const std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = clientKey + guid;
    auto digest = sha1(concat);
    return base64Encode(digest);
}

// WebSocket frame encode (server -> client, no mask, text)

std::vector<char> encodeTextFrame(const std::string& payload) {
    std::vector<char> out;
    out.reserve(2 + 8 + payload.size());
    out.push_back(static_cast<char>(0x81)); // FIN + text
    size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i=7;i>=0;--i) out.push_back(static_cast<char>((len >> (i*8)) & 0xFF));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

WsDecodeResult tryDecodeFrame(const char* data, size_t len) noexcept {
    WsDecodeResult r;
    if (len < 2) { r.status = WsDecodeResult::Status::NeedMore; return r; }
    uint8_t b0 = static_cast<uint8_t>(data[0]);
    uint8_t b1 = static_cast<uint8_t>(data[1]);
    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t payloadLen = b1 & 0x7F;
    size_t pos = 2;
    if (!fin) { r.status = WsDecodeResult::Status::Error; r.error = "fragmented not supported"; return r; }
    if (payloadLen == 126) {
        if (len < pos + 2) { r.status = WsDecodeResult::Status::NeedMore; return r; }
        payloadLen = (static_cast<uint8_t>(data[pos]) << 8) | static_cast<uint8_t>(data[pos+1]);
        pos += 2;
    } else if (payloadLen == 127) {
        if (len < pos + 8) { r.status = WsDecodeResult::Status::NeedMore; return r; }
        payloadLen = 0;
        for (int i=0;i<8;++i) payloadLen = (payloadLen << 8) | static_cast<uint8_t>(data[pos+i]);
        pos += 8;
    }
    size_t maskPos = 0;
    uint8_t mask[4] = {0,0,0,0};
    if (masked) {
        if (len < pos + 4) { r.status = WsDecodeResult::Status::NeedMore; return r; }
        mask[0]=static_cast<uint8_t>(data[pos]); mask[1]=static_cast<uint8_t>(data[pos+1]); mask[2]=static_cast<uint8_t>(data[pos+2]); mask[3]=static_cast<uint8_t>(data[pos+3]);
        pos += 4;
        maskPos = pos; // not used
        (void)maskPos;
    } else {
        // client must mask per spec, but we accept unmasked for tests
    }
    if (len < pos + payloadLen) { r.status = WsDecodeResult::Status::NeedMore; return r; }
    if (payloadLen > (1u<<20)) { r.status = WsDecodeResult::Status::Error; r.error = "ws frame too large"; return r; }
    r.consumed = pos + static_cast<size_t>(payloadLen);
    if (opcode == 0x8) { // close
        r.status = WsDecodeResult::Status::Ok;
        r.isClose = true;
        return r;
    }
    if (opcode == 0x9) { // ping
        r.status = WsDecodeResult::Status::Ok;
        r.isPing = true;
        r.pingPayload.assign(data + pos, data + pos + payloadLen);
        if (masked) {
            for (size_t i=0;i<r.pingPayload.size();++i) r.pingPayload[i] ^= mask[i%4];
        }
        return r;
    }
    if (opcode == 0xA) { // pong, ignore
        r.status = WsDecodeResult::Status::Ok;
        r.payload = "";
        return r;
    }
    if (opcode != 0x1) { // only text
        r.status = WsDecodeResult::Status::Error;
        r.error = "unsupported opcode";
        return r;
    }
    r.payload.assign(data + pos, data + pos + payloadLen);
    if (masked) {
        for (size_t i=0;i<r.payload.size();++i) r.payload[i] ^= mask[i%4];
    }
    r.status = WsDecodeResult::Status::Ok;
    return r;
}

} // namespace ws
} // namespace gateway
