#pragma once

// Minimal JSON codec — dependency-free, hand-rolled for gateway.
// Supports null, bool, int64, double, string (UTF-8 escapes), array, object.
// Parser rejects duplicates, trailing garbage, depth>64, invalid escapes.
// Writer emits canonical JSON (no pretty print). All numbers are integers
// when possible (no ".0"); doubles use %.17g.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gateway {

class JsonValue {
public:
    enum class Type : uint8_t {
        Null,
        Bool,
        Int,
        Double,
        String,
        Array,
        Object
    };

    JsonValue() noexcept : type_(Type::Null) {}
    JsonValue(std::nullptr_t) noexcept : type_(Type::Null) {}
    JsonValue(bool b) noexcept : type_(Type::Bool), bool_(b) {}
    JsonValue(int b) : type_(Type::Int), int_(b) {}
    JsonValue(int64_t i) noexcept : type_(Type::Int), int_(i) {}
    JsonValue(uint64_t u) : type_(Type::Int), int_(static_cast<int64_t>(u)) {}
    JsonValue(double d) noexcept : type_(Type::Double), double_(d) {}
    JsonValue(const std::string& s) : type_(Type::String), string_(s) {}
    JsonValue(std::string&& s) noexcept : type_(Type::String), string_(std::move(s)) {}
    JsonValue(const char* s) : type_(Type::String), string_(s) {}

    // Factories for array/object
    static JsonValue makeArray();
    static JsonValue makeObject();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isInt() const noexcept { return type_ == Type::Int; }
    [[nodiscard]] bool isDouble() const noexcept { return type_ == Type::Double; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Int || type_ == Type::Double; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool asBool() const noexcept { return bool_; }
    [[nodiscard]] int64_t asInt() const noexcept { return int_; }
    [[nodiscard]] double asDouble() const noexcept { return type_ == Type::Double ? double_ : static_cast<double>(int_); }
    [[nodiscard]] const std::string& asString() const noexcept { return string_; }
    [[nodiscard]] const std::vector<JsonValue>& asArray() const noexcept { return *array_; }
    [[nodiscard]] const std::map<std::string, JsonValue>& asObject() const noexcept { return *object_; }
    [[nodiscard]] std::vector<JsonValue>& asArray() noexcept { return *array_; }
    [[nodiscard]] std::map<std::string, JsonValue>& asObject() noexcept { return *object_; }

    // Object helpers
    [[nodiscard]] bool has(const std::string& key) const noexcept;
    [[nodiscard]] const JsonValue* get(const std::string& key) const noexcept;
    JsonValue* get(const std::string& key) noexcept;
    void set(const std::string& key, JsonValue val);
    // Array helpers
    void push_back(JsonValue val);

    // Operator for building: obj["key"] = value; ensures object type
    JsonValue& operator[](const std::string& key);
    const JsonValue& operator[](const std::string& key) const;

    // Equality for tests (deep compare, object key order irrelevant)
    [[nodiscard]] bool operator==(const JsonValue& other) const noexcept;
    [[nodiscard]] bool operator!=(const JsonValue& other) const noexcept { return !(*this == other); }

    // Serialization
    [[nodiscard]] std::string stringify() const;

    // Parsing: returns nullopt on error, optionally fills *err
    static std::optional<JsonValue> parse(std::string_view s, std::string* err = nullptr);
    static std::optional<JsonValue> parse(std::string_view s, std::string& err) { return parse(s, &err); }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::shared_ptr<std::vector<JsonValue>> array_;
    std::shared_ptr<std::map<std::string, JsonValue>> object_;
};

} // namespace gateway
