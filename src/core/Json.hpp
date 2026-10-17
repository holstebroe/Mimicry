#pragma once
// Minimal JSON value, parser and writer (plan OQ1: own JSON is the default). Used for the plugin
// profile, run reports and the worker protocol. Numbers are doubles; objects keep key order sorted.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
public:
    Value() = default;  // null
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(double d) : v_(d) {}
    Value(int i) : v_(static_cast<double>(i)) {}
    Value(std::int64_t i) : v_(static_cast<double>(i)) {}
    Value(std::uint32_t i) : v_(static_cast<double>(i)) {}
    Value(std::size_t i) : v_(static_cast<double>(i)) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v_); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(v_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(v_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(v_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(v_); }

    // Accessors with fallbacks: a wrong type yields the fallback, never an exception.
    [[nodiscard]] bool as_bool(bool fallback = false) const;
    [[nodiscard]] double as_number(double fallback = 0) const;
    [[nodiscard]] const std::string& as_string() const;  // empty string if not a string
    [[nodiscard]] const Array& as_array() const;         // empty array if not an array
    [[nodiscard]] const Object& as_object() const;      // empty object if not an object

    // Object member lookup; null Value if absent or not an object.
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    // Mutable access; turns a null value into an object / array.
    Value& set(std::string key, Value value);
    Value& push(Value value);

    // Compact when indent < 0, else pretty-printed with `indent` spaces.
    [[nodiscard]] std::string dump(int indent = -1) const;

    friend bool operator==(const Value&, const Value&) = default;

private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> v_;
};

[[nodiscard]] expected<Value, std::string> parse(std::string_view text);

}  // namespace mimicry::json
