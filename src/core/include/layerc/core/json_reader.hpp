#pragma once

#include "layerc/core/json.hpp"

#include <initializer_list>
#include <limits>
#include <optional>

namespace layerc::core::json {

enum class Required { no, yes };
enum class EmptyString { allow, reject };
enum class IntegerEncoding { number, integer_token, decimal_string, number_or_string };

struct FieldError {
  std::string path;
  std::string message;
  [[nodiscard]] std::string describe() const;
};

inline constexpr std::int64_t max_exact_json_integer = INT64_C(9'007'199'254'740'991);

// Integral tokens retain their full range. Floating-point tokens must be safe
// exact integers; decimal strings are accepted only by an explicit wire policy.
[[nodiscard]] std::optional<std::int64_t>
read_integer(const Value &value, IntegerEncoding encoding = IntegerEncoding::number);
[[nodiscard]] std::optional<std::uint64_t>
read_unsigned(const Value &value, IntegerEncoding encoding = IntegerEncoding::number);
void append_exact_integer(std::string &output, std::int64_t value);
void append_exact_unsigned(std::string &output, std::uint64_t value);

// One accumulator can read nested objects using their domain paths. Optional
// fields preserve the supplied fallback/output; errors always retain the first
// failing path. Unknown-field rejection is a deliberate schema policy.
class Reader {
public:
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

private:
  FieldError error_;
};

} // namespace layerc::core::json
