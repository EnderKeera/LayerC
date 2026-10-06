#pragma once

#include "layerc/core/document_error.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/enum_wire.hpp"

#include <initializer_list>
#include <limits>
#include <optional>

namespace layerc::core::json {

enum class Required { no, yes };
enum class EmptyString { allow, reject };
enum class IntegerEncoding { number, integer_token, decimal_string, number_or_string };

using FieldError = layerc::core::DocumentError;

// Syntax errors use the shared $@<byte-offset> path convention. A size
// violation has no parse offset and is reported at the document root.
[[nodiscard]] std::variant<Value, FieldError>
parse_bounded(std::string_view source, std::size_t max_bytes);

inline constexpr std::int64_t max_exact_json_integer = INT64_C(9'007'199'254'740'991);

// Integral tokens retain their full range. Floating-point tokens must be safe
// exact integers; decimal strings are accepted only by an explicit wire policy.
[[nodiscard]] std::optional<std::int64_t>
read_integer(const Value &value, IntegerEncoding encoding = IntegerEncoding::number);
[[nodiscard]] std::optional<std::uint64_t>
read_unsigned(const Value &value, IntegerEncoding encoding = IntegerEncoding::number);
void append_exact_integer(std::string &output, std::int64_t value);
void append_exact_unsigned(std::string &output, std::uint64_t value);
// Value form of append_exact_unsigned: a number while exactly representable,
// otherwise a decimal string.
[[nodiscard]] Value exact_unsigned_value(std::uint64_t value);
[[nodiscard]] Value exact_integer_value(std::int64_t value);
// Always a decimal string for full-width IPC identifiers.
[[nodiscard]] Value decimal_unsigned_value(std::uint64_t value);

// One accumulator can read nested objects using their domain paths. Optional
// fields preserve the supplied fallback/output; errors always retain the first
// failing path. Unknown-field rejection is a deliberate schema policy.
class Reader {
public:
  explicit Reader(Required default_required = Required::yes) noexcept
      : default_required_(default_required) {}
  [[nodiscard]] bool ok() const noexcept { return error_.message.empty(); }
  [[nodiscard]] const FieldError &error() const noexcept { return error_; }
  bool fail(std::string path, std::string message);

  const Value *field(const Value &parent, std::string_view key,
                     const std::string &path, Required required);
  const Value *object(const Value &parent, std::string_view key,
                      const std::string &path, Required required);
  const Value::Array *array(const Value &parent, std::string_view key,
                            const std::string &path, Required required);
  bool string(const Value &parent, std::string_view key, const std::string &path,
              std::string &output, Required required, EmptyString empty,
              std::size_t maximum = std::string::npos);
  std::optional<std::string>
  string(const Value &parent, std::string_view key, const std::string &path,
         Required required, EmptyString empty, std::size_t maximum = std::string::npos);
  bool boolean(const Value &parent, std::string_view key, const std::string &path,
               bool &output, Required required);
  std::optional<std::int64_t>
  integer(const Value &parent, std::string_view key, const std::string &path,
          std::int64_t minimum, std::int64_t maximum, Required required,
          std::int64_t fallback = 0,
          IntegerEncoding encoding = IntegerEncoding::number);
  std::optional<std::uint64_t>
  unsigned_integer(const Value &parent, std::string_view key, const std::string &path,
                   std::uint64_t maximum, Required required,
                   std::uint64_t fallback = 0,
                   IntegerEncoding encoding = IntegerEncoding::number);
  std::optional<double>
  number(const Value &parent, std::string_view key, const std::string &path,
         double minimum, double maximum, Required required, double fallback = 0);
  std::optional<std::uint32_t>
  color(const Value &parent, std::string_view key, const std::string &path,
        Required required, std::uint32_t fallback = 0);
  bool exact_fields(const Value &parent,
                    std::initializer_list<std::string_view> allowed,
                    const std::string &path);
  bool required_fields(const Value &parent,
                       std::initializer_list<std::string_view> required,
                       const std::string &path);

  // Default-policy overloads apply the constructor's Required policy, so a
  // schema reader states its presence rule once instead of per field.
  const Value *field(const Value &parent, std::string_view key,
                     const std::string &path) {
    return field(parent, key, path, default_required_);
  }
  const Value *object(const Value &parent, std::string_view key,
                      const std::string &path) {
    return object(parent, key, path, default_required_);
  }
  const Value::Array *array(const Value &parent, std::string_view key,
                            const std::string &path) {
    return array(parent, key, path, default_required_);
  }
  bool string(const Value &parent, std::string_view key, const std::string &path,
              std::string &output, EmptyString empty = EmptyString::reject) {
    return string(parent, key, path, output, default_required_, empty);
  }
  std::optional<std::int64_t> integer(const Value &parent, std::string_view key,
                                      const std::string &path,
                                      std::int64_t minimum, std::int64_t maximum) {
    return integer(parent, key, path, minimum, maximum, default_required_);
  }
  std::optional<double> number(const Value &parent, std::string_view key,
                               const std::string &path, double minimum,
                               double maximum) {
    return number(parent, key, path, minimum, maximum, default_required_);
  }
  std::optional<std::uint32_t> color(const Value &parent, std::string_view key,
                                     const std::string &path) {
    return color(parent, key, path, default_required_);
  }
  template <typename Enum>
  std::optional<Enum> enumeration(const Value &parent, std::string_view key,
                                  const std::string &path,
                                  std::span<const EnumWireEntry<Enum>> table,
                                  Required required, Enum fallback) {
    const auto *value = field(parent, key, path, required);
    if (!value) return ok() ? std::optional(fallback) : std::nullopt;
    if (value->is_string()) {
      if (const auto parsed = parse_enum_wire(table, value->as_string()))
        return parsed;
    }
    fail(path + "." + std::string(key), "unknown enum value");
    return std::nullopt;
  }
  template <typename Fn>
  bool each(const Value::Array &array, const std::string &path, Fn &&fn) {
    for (std::size_t index = 0; index < array.size() && ok(); ++index)
      fn(array[index], path + "[" + std::to_string(index) + "]");
    return ok();
  }

private:
  Required default_required_;
  FieldError error_;
};

} // namespace layerc::core::json
