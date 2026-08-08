#include "layerc/core/json.hpp"

#include "utf8.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace layerc::core::json {

namespace {

constexpr std::size_t kMaximumNestingDepth = 128;

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
            if (has_digit()) {
                fail("leading zero is not allowed in number");
                return {};
            }
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
                if (!detail::is_valid_utf8(result)) {
                    fail("invalid UTF-8 in string");
                    return {};
                }
                return result;
            }
            if (static_cast<unsigned char>(current) < 0x20U) {
                fail_at(position_ - 1, "control character in string");
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
                    fail_at(position_ - 1, "invalid escape sequence");
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

        detail::append_utf8(output, *codepoint);
        return true;
    }

    std::optional<std::uint32_t> parse_hex_quad() {
        if (position_ + 4 > source_.size()) {
            fail("incomplete unicode escape");
            return std::nullopt;
        }
        std::uint32_t value{};
        for (int index = 0; index < 4; ++index) {
            const auto character_offset = position_;
            const char character = source_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail_at(character_offset, "invalid hex digit in unicode escape");
                return std::nullopt;
            }
        }
        return value;
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
            auto value = parse_value(depth);
            if (!error_.message.empty()) return {};
            result.push_back(std::move(value));
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
            if (!error_.message.empty()) return {};
            skip_whitespace();
            if (!peek(':')) {
                fail("expected ':' after object key");
                return {};
            }
            ++position_;
            skip_whitespace();
            auto value = parse_value(depth);
            if (!error_.message.empty()) return {};
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
        fail_at(position_, std::move(message));
    }

    void fail_at(const std::size_t offset, std::string message) {
        if (error_.message.empty()) {
            error_ = ParseError{.offset = offset, .message = std::move(message)};
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
