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
void append_number(std::string &output, double value);
void append_quoted(std::string &output, std::string_view text);

}  // namespace layerc::core::json
