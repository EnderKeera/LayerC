#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <type_traits>
#include <utility>

namespace layerc::core::json {
namespace {
template <typename Integer>
std::optional<Integer> decimal(const std::string_view source) {
  Integer result{};
  const auto [tail, code] =
      std::from_chars(source.data(), source.data() + source.size(), result);
  if (code != std::errc{} || tail != source.data() + source.size())
    return std::nullopt;
  return result;
}

template <typename Integer>
std::optional<Integer> integral(const Value &value, const IntegerEncoding encoding) {
  if (value.is_string() && (encoding == IntegerEncoding::decimal_string ||
                            encoding == IntegerEncoding::number_or_string))
    return decimal<Integer>(value.as_string());
  if (encoding == IntegerEncoding::decimal_string)
    return std::nullopt;
  if (value.is_integer()) {
    const auto integer = value.as_integer();
    if constexpr (std::is_unsigned_v<Integer>)
      if (integer < 0) return std::nullopt;
    return static_cast<Integer>(integer);
  }
  if (encoding == IntegerEncoding::integer_token || !value.is_number())
    return std::nullopt;
  const double number = value.as_number();
  const double minimum = std::is_unsigned_v<Integer>
                             ? 0.0 : -static_cast<double>(max_exact_json_integer);
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < minimum || number > static_cast<double>(max_exact_json_integer))
    return std::nullopt;
  return static_cast<Integer>(number);
}
} // namespace

std::optional<std::int64_t> read_integer(const Value &value, const IntegerEncoding encoding) {
  return integral<std::int64_t>(value, encoding);
}
std::optional<std::uint64_t> read_unsigned(const Value &value, const IntegerEncoding encoding) {
  return integral<std::uint64_t>(value, encoding);
}
void append_exact_unsigned(std::string &output, const std::uint64_t value) {
  if (value <= static_cast<std::uint64_t>(max_exact_json_integer))
    append_unsigned(output, value);
  else
    append_quoted(output, std::to_string(value));
}
Value exact_unsigned_value(const std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(max_exact_json_integer))
    return Value(std::to_string(value));
  return Value(static_cast<std::int64_t>(value));
}
void append_exact_integer(std::string &output, const std::int64_t value) {
  if (value < -max_exact_json_integer || value > max_exact_json_integer) {
    append_quoted(output, std::to_string(value));
    return;
  }
  if (value < 0) {
    output.push_back('-');
    append_unsigned(output, static_cast<std::uint64_t>(-(value + 1)) + 1U);
  } else {
    append_unsigned(output, static_cast<std::uint64_t>(value));
  }
}

std::variant<Value, FieldError> parse_bounded(const std::string_view source,
                                              const std::size_t max_bytes) {
  if (source.size() > max_bytes)
    return FieldError{"$", "JSON exceeds " + std::to_string(max_bytes) + " bytes"};
  auto parsed = parse(source);
  if (const auto *error = std::get_if<ParseError>(&parsed))
    return FieldError{"$@" + std::to_string(error->offset), error->message};
  return std::get<Value>(std::move(parsed));
}

std::string FieldError::describe() const {
  return message.empty() ? std::string{} : path + ": " + message;
}
bool Reader::fail(std::string path, std::string message) {
  if (ok()) error_ = {std::move(path), std::move(message)};
  return false;
}
const Value *Reader::field(const Value &parent, const std::string_view key,
                           const std::string &path, const Required required) {
  if (!ok()) return nullptr;
  if (!parent.is_object()) {
    fail(path, "expected object");
    return nullptr;
  }
  const auto *value = parent.find(key);
  if (value == nullptr && required == Required::yes)
    fail(path + "." + std::string(key), "required value is missing");
  return value;
}
const Value *Reader::object(const Value &parent, const std::string_view key,
                            const std::string &path, const Required required) {
  const auto *value = field(parent, key, path, required);
  if (value && !value->is_object()) {
    fail(path + "." + std::string(key), "expected object");
    return nullptr;
  }
  return value;
}
const Value::Array *Reader::array(const Value &parent, const std::string_view key,
                                  const std::string &path, const Required required) {
  const auto *value = field(parent, key, path, required);
  if (!value) return nullptr;
  if (!value->is_array()) {
    fail(path + "." + std::string(key), "expected array");
    return nullptr;
  }
  return &value->as_array();
}
bool Reader::string(const Value &parent, const std::string_view key,
                    const std::string &path, std::string &output,
                    const Required required, const EmptyString empty,
                    const std::size_t maximum) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok();
  if (!value->is_string() || (empty == EmptyString::reject && value->as_string().empty()))
    return fail(path + "." + std::string(key), empty == EmptyString::reject
                   ? "expected non-empty string" : "expected string");
  if (value->as_string().size() > maximum)
    return fail(path + "." + std::string(key),
                "string exceeds " + std::to_string(maximum) + " UTF-8 bytes");
  output = value->as_string();
  return true;
}
std::optional<std::string> Reader::string(
    const Value &parent, const std::string_view key, const std::string &path,
    const Required required, const EmptyString empty, const std::size_t maximum) {
  std::string output;
  if (!string(parent, key, path, output, required, empty, maximum)) return std::nullopt;
  return output;
}
bool Reader::boolean(const Value &parent, const std::string_view key,
                     const std::string &path, bool &output, const Required required) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok();
  if (!value->is_bool()) return fail(path + "." + std::string(key), "expected boolean");
  output = value->as_bool();
  return true;
}
std::optional<std::int64_t>
Reader::integer(const Value &parent, const std::string_view key, const std::string &path,
                const std::int64_t minimum, const std::int64_t maximum,
                const Required required, const std::int64_t fallback,
                const IntegerEncoding encoding) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok() ? std::optional(fallback) : std::nullopt;
  const auto result = read_integer(*value, encoding);
  if (!result || *result < minimum || *result > maximum) {
    fail(path + "." + std::string(key), "integer is outside the supported range or encoding");
    return std::nullopt;
  }
  return result;
}
std::optional<std::uint64_t>
Reader::unsigned_integer(const Value &parent, const std::string_view key,
                         const std::string &path, const std::uint64_t maximum,
                         const Required required, const std::uint64_t fallback,
                         const IntegerEncoding encoding) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok() ? std::optional(fallback) : std::nullopt;
  const auto result = read_unsigned(*value, encoding);
  if (!result || *result > maximum) {
    fail(path + "." + std::string(key), "integer is outside the supported range or encoding");
    return std::nullopt;
  }
  return result;
}
std::optional<double>
Reader::number(const Value &parent, const std::string_view key, const std::string &path,
               const double minimum, const double maximum, const Required required,
               const double fallback) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok() ? std::optional(fallback) : std::nullopt;
  if (!value->is_number() || !std::isfinite(value->as_number()) ||
      value->as_number() < minimum || value->as_number() > maximum) {
    fail(path + "." + std::string(key), "number is outside the supported range");
    return std::nullopt;
  }
  return value->as_number();
}
std::optional<std::uint32_t>
Reader::color(const Value &parent, const std::string_view key, const std::string &path,
              const Required required, const std::uint32_t fallback) {
  const auto *value = field(parent, key, path, required);
  if (!value) return ok() ? std::optional(fallback) : std::nullopt;
  if (value->is_string()) {
    const auto &source = value->as_string();
    if (source.size() == 7 && source.front() == '#') {
      std::uint32_t result{};
      const auto [tail, code] = std::from_chars(source.data() + 1, source.data() + 7, result, 16);
      if (code == std::errc{} && tail == source.data() + 7) return result;
    }
  }
  fail(path + "." + std::string(key), "expected #RRGGBB color string");
  return std::nullopt;
}
bool Reader::exact_fields(const Value &parent,
                          const std::initializer_list<std::string_view> allowed,
                          const std::string &path) {
  if (!ok()) return false;
  if (!parent.is_object()) return fail(path, "expected object");
  for (const auto &[key, ignored] : parent.as_object()) {
    (void)ignored;
    if (std::ranges::find(allowed, key) == allowed.end())
      return fail(path + "." + key, "unknown field");
  }
  return true;
}
bool Reader::required_fields(const Value &parent,
                             const std::initializer_list<std::string_view> required,
                             const std::string &path) {
  for (const auto key : required)
    if (!field(parent, key, path, Required::yes)) return false;
  return ok();
}

} // namespace layerc::core::json
