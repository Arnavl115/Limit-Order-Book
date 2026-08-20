#include "json.hpp"

#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace gateway {

JsonValue JsonValue::makeArray() {
    JsonValue v;
    v.type_ = Type::Array;
    v.array_ = std::make_shared<std::vector<JsonValue>>();
    return v;
}
JsonValue JsonValue::makeObject() {
    JsonValue v;
    v.type_ = Type::Object;
    v.object_ = std::make_shared<std::map<std::string, JsonValue>>();
    return v;
}

bool JsonValue::has(const std::string& key) const noexcept {
    if (type_ != Type::Object || !object_) return false;
    return object_->find(key) != object_->end();
}
const JsonValue* JsonValue::get(const std::string& key) const noexcept {
    if (type_ != Type::Object || !object_) return nullptr;
    auto it = object_->find(key);
    return it == object_->end() ? nullptr : &it->second;
}
JsonValue* JsonValue::get(const std::string& key) noexcept {
    if (type_ != Type::Object || !object_) return nullptr;
    auto it = object_->find(key);
    return it == object_->end() ? nullptr : &it->second;
}
void JsonValue::set(const std::string& key, JsonValue val) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_ = std::make_shared<std::map<std::string, JsonValue>>();
        string_.clear();
        array_.reset();
    }
    if (!object_) object_ = std::make_shared<std::map<std::string, JsonValue>>();
    (*object_)[key] = std::move(val);
}
void JsonValue::push_back(JsonValue val) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_ = std::make_shared<std::vector<JsonValue>>();
        string_.clear();
        object_.reset();
    }
    if (!array_) array_ = std::make_shared<std::vector<JsonValue>>();
    array_->push_back(std::move(val));
}
JsonValue& JsonValue::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_ = std::make_shared<std::map<std::string, JsonValue>>();
        string_.clear();
        array_.reset();
    }
    if (!object_) object_ = std::make_shared<std::map<std::string, JsonValue>>();
    return (*object_)[key];
}
const JsonValue& JsonValue::operator[](const std::string& key) const {
    static const JsonValue nullVal;
    if (type_ != Type::Object || !object_) return nullVal;
    auto it = object_->find(key);
    return it == object_->end() ? nullVal : it->second;
}

bool JsonValue::operator==(const JsonValue& other) const noexcept {
    if (type_ != other.type_) {
        // Int vs Double numeric equality?
        if (isNumber() && other.isNumber()) {
            // compare as double with int exactness? For tests, treat 1 == 1.0 as true
            return asDouble() == other.asDouble();
        }
        return false;
    }
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == other.bool_;
        case Type::Int: return int_ == other.int_;
        case Type::Double: return double_ == other.double_;
        case Type::String: return string_ == other.string_;
        case Type::Array:
            if (!array_ && !other.array_) return true;
            if (!array_ || !other.array_) return false;
            return *array_ == *other.array_;
        case Type::Object:
            if (!object_ && !other.object_) return true;
            if (!object_ || !other.object_) return false;
            return *object_ == *other.object_;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Stringify helpers
// ---------------------------------------------------------------------------

static std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string JsonValue::stringify() const {
    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return bool_ ? "true" : "false";
        case Type::Int: return std::to_string(int_);
        case Type::Double: {
            char buf[32];
            // %.17g gives shortest round-trip
            int n = std::snprintf(buf, sizeof(buf), "%.17g", double_);
            // ensure .0 for integer-like doubles? Keep as is (e.g., "1" vs "1.0" not required)
            // But ensure that string can be parsed as double
            return std::string(buf, static_cast<size_t>(n));
        }
        case Type::String: return escapeString(string_);
        case Type::Array: {
            std::string out = "[";
            if (array_) {
                for (size_t i = 0; i < array_->size(); ++i) {
                    if (i) out.push_back(',');
                    out += (*array_)[i].stringify();
                }
            }
            out.push_back(']');
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            if (object_) {
                bool first = true;
                for (auto& kv : *object_) {
                    if (!first) out.push_back(',');
                    first = false;
                    out += escapeString(kv.first);
                    out.push_back(':');
                    out += kv.second.stringify();
                }
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

// ---------------------------------------------------------------------------
// Parse helpers
// ---------------------------------------------------------------------------

namespace {

struct Parser {
    std::string_view s;
    size_t pos = 0;
    int depth = 0;
    std::string* err = nullptr;

    void setErr(const char* msg) {
        if (err && err->empty()) *err = msg;
    }
    void setErrAt(const char* msg) {
        if (err && err->empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s at %zu", msg, pos);
            *err = buf;
        }
    }
    void skipWs() {
        while (pos < s.size() && (s[pos]==' ' || s[pos]=='\t' || s[pos]=='\n' || s[pos]=='\r')) ++pos;
    }
    bool consume(char c) {
        skipWs();
        if (pos < s.size() && s[pos]==c) { ++pos; return true; }
        return false;
    }
    bool consumeLiteral(std::string_view lit) {
        if (s.substr(pos, lit.size()) == lit) { pos += lit.size(); return true; }
        return false;
    }

    std::optional<JsonValue> parseValue() {
        if (depth > 64) { setErr("depth too deep"); return std::nullopt; }
        skipWs();
        if (pos >= s.size()) { setErr("unexpected end"); return std::nullopt; }
        char c = s[pos];
        if (c == 'n') return parseNull();
        if (c == 't') return parseTrue();
        if (c == 'f') return parseFalse();
        if (c == '"') return parseString();
        if (c == '[') return parseArray();
        if (c == '{') return parseObject();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        setErrAt("unexpected character");
        return std::nullopt;
    }

    std::optional<JsonValue> parseNull() {
        if (consumeLiteral("null")) return JsonValue(nullptr);
        setErrAt("invalid null");
        return std::nullopt;
    }
    std::optional<JsonValue> parseTrue() {
        if (consumeLiteral("true")) return JsonValue(true);
        setErrAt("invalid true");
        return std::nullopt;
    }
    std::optional<JsonValue> parseFalse() {
        if (consumeLiteral("false")) return JsonValue(false);
        setErrAt("invalid false");
        return std::nullopt;
    }

    std::optional<JsonValue> parseString() {
        assert(s[pos]=='"');
        ++pos; // skip opening
        std::string out;
        out.reserve(32);
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') {
                return JsonValue(std::move(out));
            }
            if (c == '\\') {
                if (pos >= s.size()) { setErr("unterminated escape"); return std::nullopt; }
                char esc = s[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos + 4 > s.size()) { setErr("invalid \\u"); return std::nullopt; }
                        unsigned int cp = 0;
                        for (int i=0;i<4;++i) {
                            char h = s[pos++];
                            cp <<= 4;
                            if (h>='0' && h<='9') cp |= (h - '0');
                            else if (h>='a' && h<='f') cp |= (h - 'a' + 10);
                            else if (h>='A' && h<='F') cp |= (h - 'A' + 10);
                            else { setErr("invalid \\u hex"); return std::nullopt; }
                        }
                        // Encode cp as UTF-8 (BMP only, handles surrogate pairs not required for tests)
                        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
                        else if (cp <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: setErr("invalid escape"); return std::nullopt;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                setErr("unescaped control char"); return std::nullopt;
            } else {
                out.push_back(c);
            }
        }
        setErr("unterminated string");
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber() {
        size_t start = pos;
        if (s[pos]=='-') ++pos;
        if (pos >= s.size()) { setErr("invalid number"); return std::nullopt; }
        // integer part
        if (s[pos]=='0') {
            ++pos;
        } else if (s[pos]>='1' && s[pos]<='9') {
            while (pos < s.size() && s[pos]>='0' && s[pos]<='9') ++pos;
        } else { setErr("invalid number"); return std::nullopt; }
        bool isDouble = false;
        if (pos < s.size() && s[pos]=='.') {
            isDouble = true;
            ++pos;
            if (pos>=s.size() || !(s[pos]>='0' && s[pos]<='9')) { setErr("invalid fraction"); return std::nullopt; }
            while (pos < s.size() && s[pos]>='0' && s[pos]<='9') ++pos;
        }
        if (pos < s.size() && (s[pos]=='e' || s[pos]=='E')) {
            isDouble = true;
            ++pos;
            if (pos < s.size() && (s[pos]=='+' || s[pos]=='-')) ++pos;
            if (pos>=s.size() || !(s[pos]>='0' && s[pos]<='9')) { setErr("invalid exponent"); return std::nullopt; }
            while (pos < s.size() && s[pos]>='0' && s[pos]<='9') ++pos;
        }
        std::string_view numSv = s.substr(start, pos - start);
        if (!isDouble) {
            int64_t v = 0;
            auto res = std::from_chars(numSv.data(), numSv.data()+numSv.size(), v);
            if (res.ec == std::errc() && res.ptr == numSv.data()+numSv.size()) {
                return JsonValue(v);
            }
            // fallback to double if overflow
            isDouble = true;
        }
        if (isDouble) {
            // use strtod via string copy (from_chars double not in all MSVC)
            std::string tmp(numSv);
            char* end = nullptr;
            double d = std::strtod(tmp.c_str(), &end);
            if (end != tmp.c_str() + tmp.size()) { setErr("invalid double"); return std::nullopt; }
            return JsonValue(d);
        }
        setErr("invalid number");
        return std::nullopt;
    }

    std::optional<JsonValue> parseArray() {
        assert(s[pos]=='[');
        ++pos;
        ++depth;
        JsonValue arr = JsonValue::makeArray();
        skipWs();
        if (consume(']')) { --depth; return arr; }
        while (true) {
            auto val = parseValue();
            if (!val) { --depth; return std::nullopt; }
            arr.push_back(std::move(*val));
            skipWs();
            if (consume(']')) { --depth; return arr; }
            if (!consume(',')) { setErr("expected ',' or ']' in array"); --depth; return std::nullopt; }
        }
    }

    std::optional<JsonValue> parseObject() {
        assert(s[pos]=='{');
        ++pos;
        ++depth;
        JsonValue obj = JsonValue::makeObject();
        skipWs();
        if (consume('}')) { --depth; return obj; }
        while (true) {
            skipWs();
            if (pos >= s.size() || s[pos] != '"') { setErr("expected string key"); --depth; return std::nullopt; }
            auto k = parseString();
            if (!k) { --depth; return std::nullopt; }
            std::string key = k->asString();
            skipWs();
            if (!consume(':')) { setErr("expected ':'"); --depth; return std::nullopt; }
            auto v = parseValue();
            if (!v) { --depth; return std::nullopt; }
            // duplicate key check
            if (obj.has(key)) { setErr("duplicate key"); --depth; return std::nullopt; }
            obj.set(key, std::move(*v));
            skipWs();
            if (consume('}')) { --depth; return obj; }
            if (!consume(',')) { setErr("expected ',' or '}' in object"); --depth; return std::nullopt; }
        }
    }
};

} // anon

std::optional<JsonValue> JsonValue::parse(std::string_view s, std::string* err) {
    Parser p;
    p.s = s;
    p.pos = 0;
    p.err = err;
    if (err) err->clear();
    auto v = p.parseValue();
    if (!v) return std::nullopt;
    p.skipWs();
    if (p.pos != s.size()) {
        if (err && err->empty()) *err = "trailing garbage";
        return std::nullopt;
    }
    return v;
}

} // namespace gateway
