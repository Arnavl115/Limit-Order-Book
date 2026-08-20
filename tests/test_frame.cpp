#include "gateway/frame.hpp"
#include "test_framework.hpp"

using namespace gateway::frame;

TEST(frame_encode_decode_roundtrip) {
    std::string json = "{\"type\":\"order.new\",\"id\":1}";
    auto bytes = encode(json);
    CHECK(bytes.size() == 4 + json.size());
    // big-endian check: length 27 = 0x00 0x00 0x00 0x1B
    auto res = tryDecode(bytes.data(), bytes.size());
    CHECK(res.status == DecodeResult::Status::Ok);
    CHECK(res.payload == json);
    CHECK(res.consumed == bytes.size());
}

TEST(frame_partial_buffers_need_more) {
    std::string json = "{\"a\":1}";
    auto bytes = encode(json);
    // only 2 bytes -> NeedMore
    auto r1 = tryDecode(bytes.data(), 2);
    CHECK(r1.status == DecodeResult::Status::NeedMore);
    // 4 bytes exactly (header) -> NeedMore (need payload)
    auto r2 = tryDecode(bytes.data(), 4);
    CHECK(r2.status == DecodeResult::Status::NeedMore);
    // 4 + len-1 -> NeedMore
    auto r3 = tryDecode(bytes.data(), bytes.size()-1);
    CHECK(r3.status == DecodeResult::Status::NeedMore);
}

TEST(frame_too_large_rejected) {
    std::string big(1<<20, 'x'); // 1 MiB
    // encode will produce 1 MiB payload + 4; tryDecode should Error because >kMax (1 MiB exactly is allowed? Max is 1<<20, so 1 MiB is allowed, 1MiB+1 is not)
    auto bytes = encode(big);
    auto res = tryDecode(bytes.data(), bytes.size());
    CHECK(res.status == DecodeResult::Status::Ok); // exactly 1 MiB is okay
    std::string bigger( (1<<20)+1, 'x');
    auto bytes2 = encode(bigger);
    auto res2 = tryDecode(bytes2.data(), bytes2.size());
    CHECK(res2.status == DecodeResult::Status::Error);
    CHECK(res2.error.find("too large") != std::string::npos);
}

TEST(frame_zero_length_rejected) {
    std::vector<char> buf;
    buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(0);
    auto res = tryDecode(buf.data(), buf.size());
    CHECK(res.status == DecodeResult::Status::Error);
    CHECK(res.error.find("zero") != std::string::npos);
}

TEST(frame_multiple_messages_coalesced) {
    std::string j1 = "{\"type\":\"a\"}";
    std::string j2 = "{\"type\":\"b\"}";
    auto b1 = encode(j1);
    auto b2 = encode(j2);
    std::vector<char> buf;
    buf.insert(buf.end(), b1.begin(), b1.end());
    buf.insert(buf.end(), b2.begin(), b2.end());
    // decode first
    auto r1 = tryDecode(buf.data(), buf.size());
    CHECK(r1.status == DecodeResult::Status::Ok);
    CHECK(r1.payload == j1);
    // remaining after consuming first
    auto r2 = tryDecode(buf.data()+r1.consumed, buf.size()-r1.consumed);
    CHECK(r2.status == DecodeResult::Status::Ok);
    CHECK(r2.payload == j2);
}

TEST(frame_decodeAll_helper) {
    std::string j1 = "{\"x\":1}";
    std::string j2 = "{\"y\":2}";
    auto b1 = encode(j1);
    auto b2 = encode(j2);
    std::vector<char> buf;
    buf.insert(buf.end(), b1.begin(), b1.end());
    buf.insert(buf.end(), b2.begin(), b2.end());
    // partial second
    buf.pop_back();
    std::string err;
    auto payloads = decodeAll(buf, &err);
    // should have decoded j1 only, left partial j2 in buf
    CHECK(payloads.size()==1);
    CHECK(payloads[0]==j1);
    CHECK(buf.size() == b2.size()-1); // remaining partial
    // add missing byte back and decode again
    buf.push_back(j2.back());
    auto payloads2 = decodeAll(buf, &err);
    CHECK(payloads2.size()==1);
    CHECK(payloads2[0]==j2);
    CHECK(buf.empty());
}

TEST(frame_little_endian_rejected_as_too_large) {
    // Encode manually little-endian length 27 = 0x1B 0x00 0x00 0x00
    // Our decoder expects BE, so it will read 0x1B000000 = ~453M > 1MiB -> Error
    std::string json = "{\"a\":1}";
    std::vector<char> le;
    uint32_t n = static_cast<uint32_t>(json.size());
    le.push_back(static_cast<char>(n & 0xFF));
    le.push_back(static_cast<char>((n>>8)&0xFF));
    le.push_back(static_cast<char>((n>>16)&0xFF));
    le.push_back(static_cast<char>((n>>24)&0xFF));
    le.insert(le.end(), json.begin(), json.end());
    auto res = tryDecode(le.data(), le.size());
    CHECK(res.status == DecodeResult::Status::Error);
}

TEST(frame_empty_input_need_more) {
    auto res = tryDecode(nullptr, 0);
    CHECK(res.status == DecodeResult::Status::NeedMore);
    std::vector<char> empty;
    std::string err;
    auto payloads = decodeAll(empty, &err);
    CHECK(payloads.empty());
}
