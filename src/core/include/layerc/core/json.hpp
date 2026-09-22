#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                                 std::string, Array, Object>;

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
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] const Value* find(std::string_view key) const;

private:
    Storage storage_;
};

struct ParseError {
    std::size_t offset{};
    std::string message;
};

using ParseResult = std::variant<Value, ParseError>;

[[nodiscard]] ParseResult parse(std::string_view source);
void append_unsigned(std::string &output, std::uint64_t value);
// Rejects non-finite values with std::invalid_argument and leaves output unchanged.
void append_number(std::string &output, double value);
void append_quoted(std::string &output, std::string_view text);
// Appends a quoted uppercase "#RRGGBB" color (for example "#50BAC8").
// Shared by the profile and visual serializers so the hex casing, width,
// and quoting stay byte-stable in one place.
void append_color_quoted(std::string &output, std::uint32_t color);
// Appends "true" or "false" for a boolean field. Shared by the serializers so
// the spelling stays byte-stable in one place.
void append_bool(std::string &output, bool value);
// Generic deterministic JSON emission; indent=0 is compact, otherwise CRLF
// and that many spaces per level. Parsed integer values retain full precision.
// A non-finite double is rejected with std::invalid_argument; output is restored
// to its original size before the exception propagates.
void append_value(std::string &output, const Value &value, unsigned indent = 0);
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
