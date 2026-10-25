#include "layerc/core/json.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace layerc::core::json {
Value::Value() noexcept : storage_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(const bool value) noexcept : storage_(value) {}
Value::Value(const std::int64_t value) noexcept : storage_(value) {}
Value::Value(const double value) noexcept : storage_(value) {}
Value::Value(const std::int64_t value, std::string source)
    : storage_(std::make_shared<const ParsedInteger>(
          ParsedInteger{value, std::move(source)})) {}
Value::Value(const double value, std::string source)
    : storage_(std::make_shared<const ParsedNumber>(
          ParsedNumber{value, std::move(source)})) {}
Value::Value(std::string value)
    : storage_(std::make_shared<const std::string>(std::move(value))) {}
Value::Value(Array value)
    : storage_(std::make_shared<const Array>(std::move(value))) {}
Value::Value(Object value)
    : storage_(std::make_shared<const Object>(std::move(value))) {}

Value::Value(Value &&other) noexcept : storage_(std::move(other.storage_)) {
    other.storage_ = nullptr;
}
Value &Value::operator=(const Value &other) noexcept {
    // Snapshot first: other may live inside this value's current payload, and
    // variant assignment destroys the old alternative before copying.
    SharedStorage snapshot = other.storage_;
    storage_ = std::move(snapshot);
    return *this;
}
Value &Value::operator=(Value &&other) noexcept {
    if (this != &other) {
        SharedStorage taken = std::move(other.storage_);
        other.storage_ = nullptr;
        storage_ = std::move(taken);
    }
    return *this;
}
bool Value::operator==(const Value &other) const {
    if (storage_.index() != other.storage_.index()) return false;
    return std::visit([&](const auto &item) {
        using T = std::decay_t<decltype(item)>;
        const auto &rhs = std::get<T>(other.storage_);
        if constexpr (requires { *item; }) {
            return item == rhs || *item == *rhs;
        } else {
            return item == rhs;
        }
    }, storage_);
}

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_integer() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_) ||
           std::holds_alternative<SharedInteger>(storage_);
}
bool Value::is_number() const noexcept {
    return is_integer() ||
           std::holds_alternative<double>(storage_) ||
           std::holds_alternative<SharedNumber>(storage_);
}
bool Value::is_string() const noexcept { return std::holds_alternative<SharedString>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<SharedArray>(storage_); }
bool Value::is_object() const noexcept { return std::holds_alternative<SharedObject>(storage_); }

bool Value::as_bool() const { return std::get<bool>(storage_); }
std::int64_t Value::as_integer() const {
    if (const auto *parsed = std::get_if<SharedInteger>(&storage_)) return (*parsed)->value;
    return std::get<std::int64_t>(storage_);
}
double Value::as_number() const {
    if (is_integer()) return static_cast<double>(as_integer());
    if (const auto *parsed = std::get_if<SharedNumber>(&storage_)) return (*parsed)->value;
    return std::get<double>(storage_);
}
std::string_view Value::number_source() const noexcept {
    if (const auto *parsed = std::get_if<SharedInteger>(&storage_)) return (*parsed)->source;
    if (const auto *parsed = std::get_if<SharedNumber>(&storage_)) return (*parsed)->source;
    return {};
}
const std::string& Value::as_string() const { return *std::get<SharedString>(storage_); }
const Value::Array& Value::as_array() const { return *std::get<SharedArray>(storage_); }
const Value::Object& Value::as_object() const { return *std::get<SharedObject>(storage_); }

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

void append_number(std::string &output, const double value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("JSON numbers must be finite");
    std::array<char, 64> buffer;
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec == std::errc{}) {
        output.append(buffer.data(), converted.ptr);
        return;
    }
    assert(false && "the fixed double JSON buffer is too small");
    throw std::runtime_error("failed to format JSON number");
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

std::string color_text(std::uint32_t color) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string text = "#000000";
    for (int shift = 20, index = 1; shift >= 0; shift -= 4, ++index)
        text[static_cast<std::size_t>(index)] =
            hex[(color >> static_cast<unsigned>(shift)) & 0xFU];
    return text;
}

std::optional<std::uint32_t>
parse_color_text(const std::string_view text) noexcept {
    if (text.size() != 7 || text.front() != '#') return std::nullopt;
    const auto *const last = text.data() + text.size();
    std::uint32_t color{};
    const auto [tail, code] = std::from_chars(text.data() + 1, last, color, 16);
    if (code != std::errc{} || tail != last) return std::nullopt;
    return color;
}

void append_color_quoted(std::string &output, std::uint32_t color) {
    append_quoted(output, color_text(color));
}

void append_bool(std::string &output, bool value) {
    output += value ? "true" : "false";
}

namespace {
class EmissionBudget {
public:
  explicit EmissionBudget(const std::size_t remaining) : remaining_(remaining) {}
  bool value(const Value &item, const unsigned indent, const unsigned depth = 0) {
    if (item.is_null()) return take(4);
    if (item.is_bool()) return take(item.as_bool() ? 4U : 5U);
    if (!item.number_source().empty()) return take(item.number_source().size());
    if (item.is_integer()) return take(std::to_string(item.as_integer()).size());
    if (item.is_number()) {
      std::string scalar;
      append_number(scalar, item.as_number());
      return take(scalar.size());
    }
    if (item.is_string()) return quoted(item.as_string());
    if (!take(1)) return false;
    bool first = true;
    if (item.is_array()) {
      for (const auto &child : item.as_array()) {
        if ((!first && !take(1)) || !newline(depth + 1, indent) ||
            !value(child, indent, depth + 1)) return false;
        first = false;
      }
    } else {
      for (const auto &[key, child] : item.as_object()) {
        if ((!first && !take(1)) || !newline(depth + 1, indent) ||
            !quoted(key) || !take(indent ? 2U : 1U) ||
            !value(child, indent, depth + 1)) return false;
        first = false;
      }
    }
    return (first || newline(depth, indent)) && take(1);
  }
private:
  bool take(const std::size_t bytes) {
    if (bytes > remaining_) return false;
    remaining_ -= bytes;
    return true;
  }
  bool newline(const unsigned depth, const unsigned indent) {
    if (indent == 0) return true;
    if (!take(2) || static_cast<std::size_t>(depth) > remaining_ / indent)
      return false;
    return take(static_cast<std::size_t>(depth) * indent);
  }
  bool quoted(const std::string_view text) {
    if (!take(1)) return false;
    for (const unsigned char character : text) {
      std::size_t count = 1;
      switch (character) {
      case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
        count = 2; break;
      default: if (character < 0x20U) count = 6; break;
      }
      if (!take(count)) return false;
    }
    return take(1);
  }
  std::size_t remaining_;
};

void emit_value(std::string &output, const Value &value, const unsigned indent,
                const unsigned depth) {
  const auto newline = [&](unsigned level) {
    if (indent) {
      output += "\r\n";
      output.append(static_cast<std::size_t>(level) * indent, ' ');
    }
  };
  if (value.is_null()) output += "null";
  else if (value.is_bool()) append_bool(output, value.as_bool());
  else if (!value.number_source().empty()) output += value.number_source();
  else if (value.is_integer()) output += std::to_string(value.as_integer());
  else if (value.is_number()) append_number(output, value.as_number());
  else if (value.is_string()) append_quoted(output, value.as_string());
  else if (value.is_array()) {
    output += '[';
    Join join;
    for (const auto &item : value.as_array()) {
      join(output);
      newline(depth + 1);
      emit_value(output, item, indent, depth + 1);
    }
    if (!value.as_array().empty()) newline(depth);
    output += ']';
  } else {
    output += '{';
    Join join;
    for (const auto &[key, item] : value.as_object()) {
      join(output);
      newline(depth + 1);
      append_quoted(output, key);
      output += indent ? ": " : ":";
      emit_value(output, item, indent, depth + 1);
    }
    if (!value.as_object().empty()) newline(depth);
    output += '}';
  }
}
} // namespace

void append_value(std::string &output, const Value &value, const unsigned indent) {
  const auto original_size = output.size();
  try {
    emit_value(output, value, indent, 0);
  } catch (...) {
    output.resize(original_size);
    throw;
  }
}

bool append_value_bounded(std::string &output, const Value &value,
                          const std::size_t max_output_bytes,
                          const unsigned indent) {
  if (output.size() > max_output_bytes) return false;
  EmissionBudget budget(max_output_bytes - output.size());
  if (!budget.value(value, indent)) return false;
  append_value(output, value, indent);
  return true;
}

std::string format(const std::string_view source) {
  const auto parsed = parse(source);
  const auto *value = std::get_if<Value>(&parsed);
  if (!value) return std::string(source);
  std::string output;
  append_value(output, *value, 2);
  return output;
}

} // namespace layerc::core::json
