#include "gateway/json.hpp"
#include "test_framework.hpp"

using namespace gateway;

TEST(json_null_bool) {
    auto v = JsonValue::parse("null");
    CHECK(v.has_value() && v->isNull());
    CHECK(v->stringify() == "null");
    auto t = JsonValue::parse("true");
    CHECK(t.has_value() && t->isBool() && t->asBool());
    auto f = JsonValue::parse("false");
    CHECK(f.has_value() && f->isBool() && !f->asBool());
}

TEST(json_numbers) {
    auto i = JsonValue::parse("42");
    CHECK(i.has_value() && i->isInt() && i->asInt()==42);
    CHECK(i->stringify()=="42");
    auto neg = JsonValue::parse("-7");
    CHECK(neg.has_value() && neg->asInt()==-7);
    auto dbl = JsonValue::parse("3.14");
    CHECK(dbl.has_value() && dbl->isDouble());
    CHECK(dbl->stringify().find("3.14") != std::string::npos);
    auto exp = JsonValue::parse("1e6");
    CHECK(exp.has_value() && exp->isDouble());
    // trailing garbage rejected
    std::string err;
    auto bad = JsonValue::parse("42 extra", &err);
    CHECK(!bad.has_value());
    CHECK(!err.empty());
}

TEST(json_strings_and_escapes) {
    auto s = JsonValue::parse("\"hello\"");
    CHECK(s.has_value() && s->isString() && s->asString()=="hello");
    CHECK(s->stringify()=="\"hello\"");
    auto esc = JsonValue::parse("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"");
    CHECK(esc.has_value());
    CHECK(esc->asString() == std::string("a\"b\\c/d\b\f\n\r\t"));
    // round-trip preserves escapes
    std::string rt = esc->stringify();
    auto reparsed = JsonValue::parse(rt);
    CHECK(reparsed.has_value() && reparsed->asString()==esc->asString());
    // \u escape
    auto uni = JsonValue::parse("\"\\u0041\"");
    CHECK(uni.has_value() && uni->asString()=="A");
    auto uni2 = JsonValue::parse("\"\\u00e9\"");
    CHECK(uni2.has_value());
    // UTF-8 bytes for e9 should be C3 A9
    CHECK(uni2->asString().size()==2);
    // control char unescaped rejected
    std::string err;
    auto bad = JsonValue::parse("\"hello\nworld\"", &err);
    CHECK(!bad.has_value());
}

TEST(json_arrays) {
    auto a = JsonValue::parse("[1,2,3]");
    CHECK(a.has_value() && a->isArray() && a->asArray().size()==3);
    CHECK(a->asArray()[0].asInt()==1);
    CHECK(a->stringify()=="[1,2,3]");
    auto empty = JsonValue::parse("[]");
    CHECK(empty.has_value() && empty->asArray().empty());
    // nested
    auto nested = JsonValue::parse("[[1],[2,3]]");
    CHECK(nested.has_value() && nested->asArray().size()==2);
}

TEST(json_objects) {
    auto o = JsonValue::parse("{\"a\":1,\"b\":true}");
    CHECK(o.has_value() && o->isObject());
    CHECK(o->has("a") && o->has("b"));
    CHECK(o->get("a")->asInt()==1);
    CHECK(o->get("b")->asBool());
    // stringify sorts keys via std::map — check contains
    std::string s = o->stringify();
    CHECK(s.find("\"a\":1") != std::string::npos);
    CHECK(s.find("\"b\":true") != std::string::npos);
    // building via API
    JsonValue obj = JsonValue::makeObject();
    obj.set("x", JsonValue(int64_t(10)));
    obj.set("y", JsonValue("hi"));
    CHECK(obj.has("x") && obj.has("y"));
    CHECK(obj["x"].asInt()==10);
    auto parsed = JsonValue::parse(obj.stringify());
    CHECK(parsed.has_value() && *parsed == obj);
}

TEST(json_duplicate_key_rejected) {
    std::string err;
    auto v = JsonValue::parse("{\"a\":1,\"a\":2}", &err);
    CHECK(!v.has_value());
    CHECK(err.find("duplicate") != std::string::npos);
}

TEST(json_malformed_rejected) {
    std::string err;
    CHECK(!JsonValue::parse("{", &err).has_value());
    err.clear();
    CHECK(!JsonValue::parse("[1,]", &err).has_value());
    err.clear();
    CHECK(!JsonValue::parse("\"unterminated", &err).has_value());
    err.clear();
    CHECK(!JsonValue::parse("{\"a\":}", &err).has_value());
    err.clear();
    CHECK(!JsonValue::parse("invalid", &err).has_value());
    err.clear();
    CHECK(!JsonValue::parse("", &err).has_value());
    err.clear();
    // depth too deep
    std::string deep;
    for (int i=0;i<70;++i) deep.push_back('[');
    for (int i=0;i<70;++i) deep.push_back(']');
    CHECK(!JsonValue::parse(deep, &err).has_value());
    CHECK(err.find("depth") != std::string::npos);
}

TEST(json_roundtrip_complex) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("type", JsonValue("order.new"));
    obj.set("id", JsonValue(int64_t(123)));
    obj.set("side", JsonValue("buy"));
    obj.set("price", JsonValue(int64_t(100)));
    obj.set("qty", JsonValue(int64_t(10)));
    JsonValue arr = JsonValue::makeArray();
    arr.push_back(JsonValue(int64_t(1)));
    arr.push_back(JsonValue(int64_t(2)));
    obj.set("arr", arr);
    std::string s = obj.stringify();
    auto parsed = JsonValue::parse(s);
    CHECK(parsed.has_value());
    CHECK(*parsed == obj);
    CHECK(parsed->get("type")->asString()=="order.new");
    CHECK(parsed->get("id")->asInt()==123);
}

TEST(json_equality_int_double) {
    auto a = JsonValue(int64_t(1));
    auto b = JsonValue(1.0);
    CHECK(a == b); // numeric equality across types
}
