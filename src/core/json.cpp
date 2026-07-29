#include "layerc/core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace layerc::core::json {

Value::Value() noexcept : storage_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(const bool value) noexcept : storage_(value) {}
Value::Value(const double value) noexcept : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_number() const noexcept { return std::holds_alternative<double>(storage_); }
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Value::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Value::as_bool() const { return std::get<bool>(storage_); }
double Value::as_number() const { return std::get<double>(storage_); }
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

namespace {

constexpr std::size_t kMaximumNestingDepth = 128;

[[nodiscard]] bool is_valid_utf8(const std::string_view text) {
    std::size_t index{};
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t length{};
        std::uint32_t codepoint{};
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            codepoint = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }

        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }

        if ((length == 2 && codepoint < 0x80U) ||
            (length == 3 && codepoint < 0x800U) ||
            (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

class Parser {
public:
    explicit Parser(const std::string_view source) : source_(source) {}

    ParseResult run() {
        skip_whitespace();
        auto value = parse_value();
        if (error_.message.empty()) {
            skip_whitespace();
            if (position_ != source_.size()) {
                fail("unexpected trailing content");
            }
        }
        if (!error_.message.empty()) {
            return error_;
        }
        return value;
    }

private:
    Value parse_value(const std::size_t depth = 0) {
        if (position_ >= source_.size()) {
            fail("expected a JSON value");
            return {};
        }

        switch (source_[position_]) {
            case 'n': return parse_literal("null", Value(nullptr));
            case 't': return parse_literal("true", Value(true));
            case 'f': return parse_literal("false", Value(false));
            case '"': return Value(parse_string());
            case '[':
                if (depth >= kMaximumNestingDepth) {
                    fail("maximum nesting depth exceeded");
                    return {};
                }
                return parse_array(depth + 1);
            case '{':
                if (depth >= kMaximumNestingDepth) {
                    fail("maximum nesting depth exceeded");
                    return {};
                }
                return parse_object(depth + 1);
            default:
                if (source_[position_] == '-' || is_digit(source_[position_])) {
                    return parse_number();
                }
                fail("unexpected character while reading value");
                return {};
        }
    }

    Value parse_literal(const std::string_view literal, Value value) {
        if (source_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
            return {};
        }
        position_ += literal.size();
        return value;
    }

    Value parse_number() {
        const auto start = position_;
        if (peek('-')) {
            ++position_;
        }
        if (peek('0')) {
            ++position_;
        } else {
            if (!has_digit()) {
                fail("expected digit in number");
                return {};
            }
            while (has_digit()) {
                ++position_;
            }
        }
        if (peek('.')) {
            ++position_;
            if (!has_digit()) {
                fail("expected digit after decimal point");
                return {};
            }
            while (has_digit()) {
                ++position_;
            }
        }
        if (peek('e') || peek('E')) {
            ++position_;
            if (peek('+') || peek('-')) {
                ++position_;
            }
            if (!has_digit()) {
                fail("expected digit in exponent");
                return {};
            }
            while (has_digit()) {
                ++position_;
            }
        }

        double number{};
        const auto text = source_.substr(start, position_ - start);
        const auto conversion = std::from_chars(text.data(), text.data() + text.size(), number);
        if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size() || !std::isfinite(number)) {
            fail("invalid number");
            return {};
        }
        return Value(number);
    }

    std::string parse_string() {
        std::string result;
        ++position_;

        while (position_ < source_.size()) {
            const char current = source_[position_++];
            if (current == '"') {
                if (!is_valid_utf8(result)) {
                    fail("invalid UTF-8 in string");
                    return {};
                }
                return result;
            }
            if (static_cast<unsigned char>(current) < 0x20U) {
                fail("control character in string");
                return {};
            }
            if (current != '\\') {
                result.push_back(current);
                continue;
            }
            if (position_ >= source_.size()) {
                fail("unterminated escape sequence");
                return {};
            }

            const char escaped = source_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                    if (!append_unicode_escape(result)) {
                        return {};
                    }
                    break;
                default:
                    fail("invalid escape sequence");
                    return {};
            }
        }

        fail("unterminated string");
        return {};
    }

    bool append_unicode_escape(std::string& output) {
        auto codepoint = parse_hex_quad();
        if (!codepoint.has_value()) {
            return false;
        }
        if (*codepoint >= 0xD800U && *codepoint <= 0xDBFFU) {
            if (position_ + 2 > source_.size() || source_.substr(position_, 2) != "\\u") {
                fail("high surrogate without low surrogate");
                return false;
            }
            position_ += 2;
            const auto low = parse_hex_quad();
            if (!low.has_value() || *low < 0xDC00U || *low > 0xDFFFU) {
                fail("invalid low surrogate");
                return false;
            }
            *codepoint = 0x10000U + ((*codepoint - 0xD800U) << 10U) + (*low - 0xDC00U);
        } else if (*codepoint >= 0xDC00U && *codepoint <= 0xDFFFU) {
            fail("unexpected low surrogate");
            return false;
        }

        append_utf8(output, *codepoint);
        return true;
    }

    std::optional<std::uint32_t> parse_hex_quad() {
        if (position_ + 4 > source_.size()) {
            fail("incomplete unicode escape");
            return std::nullopt;
        }
        std::uint32_t value{};
        for (int index = 0; index < 4; ++index) {
            const char character = source_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail("invalid hex digit in unicode escape");
                return std::nullopt;
            }
        }
        return value;
    }

    static void append_utf8(std::string& output, const std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    Value parse_array(const std::size_t depth) {
        Value::Array result;
        ++position_;
        skip_whitespace();
        if (peek(']')) {
            ++position_;
            return Value(std::move(result));
        }

        while (error_.message.empty()) {
            skip_whitespace();
            result.push_back(parse_value(depth));
            skip_whitespace();
            if (peek(']')) {
                ++position_;
                return Value(std::move(result));
            }
            if (!peek(',')) {
                fail("expected ',' or ']' in array");
                return {};
            }
            ++position_;
        }
        return {};
    }

    Value parse_object(const std::size_t depth) {
        Value::Object result;
        ++position_;
        skip_whitespace();
        if (peek('}')) {
            ++position_;
            return Value(std::move(result));
        }

        while (error_.message.empty()) {
            skip_whitespace();
            if (!peek('"')) {
                fail("expected string key in object");
                return {};
            }
            auto key = parse_string();
            skip_whitespace();
            if (!peek(':')) {
                fail("expected ':' after object key");
                return {};
            }
            ++position_;
            skip_whitespace();
            auto value = parse_value(depth);
            if (!result.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate object key");
                return {};
            }
            skip_whitespace();
            if (peek('}')) {
                ++position_;
                return Value(std::move(result));
            }
            if (!peek(',')) {
                fail("expected ',' or '}' in object");
                return {};
            }
            ++position_;
        }
        return {};
    }

    void skip_whitespace() {
        while (position_ < source_.size()) {
            const char current = source_[position_];
            if (current != ' ' && current != '\t' && current != '\n' && current != '\r') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool peek(const char expected) const {
        return position_ < source_.size() && source_[position_] == expected;
    }

    [[nodiscard]] bool has_digit() const {
        return position_ < source_.size() && is_digit(source_[position_]);
    }

    static bool is_digit(const char character) {
        return character >= '0' && character <= '9';
    }

    void fail(std::string message) {
        if (error_.message.empty()) {
            error_ = ParseError{.offset = position_, .message = std::move(message)};
        }
    }

    std::string_view source_;
    std::size_t position_{};
    ParseError error_;
};

}  // namespace

ParseResult parse(const std::string_view source) {
    return Parser(source).run();
}

}  // namespace layerc::core::json
