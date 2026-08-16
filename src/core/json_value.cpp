#include "layerc/core/json.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace layerc::core::json {
Value::Value() noexcept : storage_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(const bool value) noexcept : storage_(value) {}
Value::Value(const std::int64_t value) noexcept : storage_(value) {}
Value::Value(const double value) noexcept : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_integer() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool Value::is_number() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_) ||
           std::holds_alternative<double>(storage_);
}
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Value::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Value::as_bool() const { return std::get<bool>(storage_); }
std::int64_t Value::as_integer() const { return std::get<std::int64_t>(storage_); }
double Value::as_number() const {
    if (const auto *integer = std::get_if<std::int64_t>(&storage_))
        return static_cast<double>(*integer);
    return std::get<double>(storage_);
}
const std::string& Value::as_string() const { return std::get<std::string>(storage_); }
const Value::Array& Value::as_array() const { return std::get<Array>(storage_); }
const Value::Object& Value::as_object() const { return std::get<Object>(storage_); }

const Value* Value::find(const std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    const auto& object = as_object();
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

void append_unsigned(std::string &output, std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto converted = std::to_chars(buffer.data(),
                                         buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) {
        output.append(buffer.data(), converted.ptr);
        return;
    }
    assert(false && "the fixed uint64 JSON buffer is too small");
    output.push_back('0');
}

void append_number(std::string &output, double value) {
    if (!std::isfinite(value)) {
        output += "0.0";
        return;
    }
    std::array<char, 64> buffer;
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec == std::errc{}) {
        output.append(buffer.data(), converted.ptr);
        return;
    }
    assert(false && "the fixed double JSON buffer is too small");
    output += "0.0";
}

void append_quoted(std::string &output, std::string_view text) {
    constexpr std::string_view hex = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : text) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(hex[(character >> 4U) & 0x0FU]);
                output.push_back(hex[character & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

} // namespace layerc::core::json
