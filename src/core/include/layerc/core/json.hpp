#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core::json {

inline constexpr std::size_t maximum_nesting_depth = 128;
inline constexpr std::string_view value_count_limit_error =
    "maximum JSON value count exceeded";
// Counts every JSON value, including the root and containers, not object keys.
// Exhaustion is reported before constructing the next value.

namespace detail { class Parser; }

class Value {
  // Only the parser can create this: its spelling has passed JSON grammar
  // and finite-number validation. Opaque fields must not round away digits.
  struct ParsedInteger {
    std::int64_t value;
    std::string source;
    bool operator==(const ParsedInteger &) const = default;
  };
  struct ParsedNumber {
    double value;
    std::string source;
    bool operator==(const ParsedNumber &) const = default;
  };
public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value, std::less<>>;
  // The public alternatives remain source-compatible; complex payloads are
  // immutable and shared internally.
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                               std::string, Array, Object, ParsedNumber, ParsedInteger>;

  Value(const Value &) = default;
  Value &operator=(const Value &other) noexcept;
  Value(Value &&other) noexcept;
  Value &operator=(Value &&other) noexcept;

  Value() noexcept;
  explicit Value(std::nullptr_t) noexcept;
  explicit Value(bool value) noexcept;
  explicit Value(std::int64_t value) noexcept;
  explicit Value(double value) noexcept;
  explicit Value(std::string value);
  explicit Value(Array value);
  explicit Value(Object value);

  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  // Both integer and double storage satisfy is_number(); exact integer
  // readers use is_integer()/as_integer() instead of re-deriving the value
  // from a double.
  [[nodiscard]] bool is_integer() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;

  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] std::int64_t as_integer() const;
  [[nodiscard]] double as_number() const;
  [[nodiscard]] std::string_view number_source() const noexcept;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] const Object& as_object() const;
  [[nodiscard]] const Value* find(std::string_view key) const;

  bool operator==(const Value &other) const;

private:
  friend class detail::Parser;
  using SharedString = std::shared_ptr<const std::string>;
  using SharedArray = std::shared_ptr<const Array>;
  using SharedObject = std::shared_ptr<const Object>;
  using SharedNumber = std::shared_ptr<const ParsedNumber>;
  using SharedInteger = std::shared_ptr<const ParsedInteger>;
  using SharedStorage = std::variant<std::nullptr_t, bool, std::int64_t, double,
      SharedString, SharedArray, SharedObject, SharedNumber, SharedInteger>;
  Value(std::int64_t value, std::string source);
  Value(double value, std::string source);
  // Shared complex payloads are immutable; equality remains value-based.
  SharedStorage storage_;
};

struct ParseError {
  std::size_t offset{};
  std::string message;
};

using ParseResult = std::variant<Value, ParseError>;

[[nodiscard]] ParseResult parse(std::string_view source);
[[nodiscard]] ParseResult parse(std::string_view source, std::size_t max_values);
void append_unsigned(std::string &output, std::uint64_t value);
// Rejects non-finite values with std::invalid_argument and leaves output unchanged.
void append_number(std::string &output, double value);
void append_quoted(std::string &output, std::string_view text);
// Appends a quoted uppercase "#RRGGBB" color (for example "#50BAC8").
// Shared by the profile and visual serializers so the hex casing, width,
// and quoting stay byte-stable in one place.
void append_color_quoted(std::string &output, std::uint32_t color);
[[nodiscard]] std::string color_text(std::uint32_t color);
// Inverse of color_text: "#RRGGBB" in either hex case, otherwise nullopt.
[[nodiscard]] std::optional<std::uint32_t>
parse_color_text(std::string_view text) noexcept;
// Appends "true" or "false" for a boolean field. Shared by the serializers so
// the spelling stays byte-stable in one place.
void append_bool(std::string &output, bool value);
// Generic deterministic JSON emission; indent=0 is compact, otherwise CRLF
// and that many spaces per level. Parsed numeric values retain their digits.
// A non-finite double is rejected with std::invalid_argument; output is restored
// to its original size before the exception propagates.
void append_value(std::string &output, const Value &value, unsigned indent = 0);
// max_output_bytes bounds the complete output, including any existing prefix.
// False leaves output unchanged. Preflight allocates no formatted containers;
// successful output is byte-identical to append_value with the same indent.
[[nodiscard]] bool append_value_bounded(std::string &output, const Value &value,
                                        std::size_t max_output_bytes,
                                        unsigned indent = 0);
// Invalid source is returned unchanged so an editor never discards an error.
[[nodiscard]] std::string format(std::string_view source);

// Emits the ',' between array elements or object members: call once per
// element before writing it; the first call emits nothing. Replaces the
// "if (index != 0) output.push_back(',');" boilerplate in serializers.
class Join {
public:
  void operator()(std::string &output) {
    if (first_) {
      first_ = false;
      return;
    }
    output.push_back(',');
  }

private:
  bool first_{true};
};

}  // namespace layerc::core::json
