#include "profile_reader.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace layerc::core::detail {
namespace {

std::optional<std::uint32_t> parse_hex_color(const std::string_view value) {
  if (value.size() != 7 || value.front() != '#') {
    return std::nullopt;
  }

  std::uint32_t color{};
  for (std::size_t index = 1; index < value.size(); ++index) {
    const auto character = value[index];
    std::uint32_t digit{};
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint32_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint32_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint32_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    color = (color << 4U) | digit;
  }
  return color;
}

} // namespace
bool ProfileReader::ok() const { return error_.message.empty(); }

const json::Value *ProfileReader::required_object(const json::Value &parent,
                                     const std::string_view key,
                                     const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_object()) {
      set_error(path + "." + std::string(key), "expected object");
      return nullptr;
    }
    return value;
  }

const json::Value::Array *ProfileReader::required_array(const json::Value &parent,
                                            const std::string_view key,
                                            const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_array()) {
      set_error(path + "." + std::string(key), "expected array");
      return nullptr;
    }
    return &value->as_array();
  }

bool ProfileReader::required_string(const json::Value &parent, const std::string_view key,
                       const std::string &path, std::string &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return set_error(path + "." + std::string(key), "required value is missing");
    }
    if (!value->is_string() || value->as_string().empty()) {
      return set_error(path + "." + std::string(key), "expected non-empty string");
    }
    output = value->as_string();
    return true;
  }

void ProfileReader::optional_string(const json::Value &parent, const std::string_view key,
                       const std::string &path, std::string &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_string()) {
      set_error(path + "." + std::string(key), "expected string");
      return;
    }
    output = value->as_string();
  }

void ProfileReader::optional_boolean(const json::Value &parent, const std::string_view key,
                        const std::string &path, bool &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_bool()) {
      set_error(path + "." + std::string(key), "expected boolean");
      return;
    }
    output = value->as_bool();
  }

void ProfileReader::optional_color(const json::Value &parent, const std::string_view key,
                      const std::string &path, std::uint32_t &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_string()) {
      set_error(path + "." + std::string(key), "expected #RRGGBB color string");
      return;
    }
    const auto parsed = parse_hex_color(value->as_string());
    if (!parsed.has_value()) {
      set_error(path + "." + std::string(key), "expected #RRGGBB color string");
      return;
    }
    output = *parsed;
  }

std::optional<std::int64_t> ProfileReader::required_integer(const json::Value &parent,
                                                const std::string_view key,
                                                const std::string &path,
                                                const std::int64_t minimum,
                                                const std::int64_t maximum) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return std::nullopt;
    }
    return checked_integer(*value, path + "." + std::string(key), minimum, maximum);
  }

std::optional<std::int64_t> ProfileReader::optional_integer(const json::Value &parent,
                                                const std::string_view key,
                                                const std::string &path,
                                                const std::int64_t minimum,
                                                const std::int64_t maximum,
                                                const std::int64_t fallback) {
    const auto *value = parent.find(key);
    return value == nullptr ? std::optional<std::int64_t>(fallback) :
        checked_integer(*value, path + "." + std::string(key), minimum, maximum);
  }

std::optional<std::int64_t> ProfileReader::checked_integer(const json::Value &value,
                                               const std::string &path,
                                               const std::int64_t minimum,
                                               const std::int64_t maximum) {
    if (!value.is_number()) {
      set_error(path, "expected integer");
      return std::nullopt;
    }
    if (value.is_integer()) {
      const auto integer = value.as_integer();
      if (integer < minimum || integer > maximum) {
        set_error(path, "integer is outside the supported range");
        return std::nullopt;
      }
      return integer;
    }
    const auto number = value.as_number();
    if (std::floor(number) != number || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) {
      set_error(path, "integer is outside the supported range");
      return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
  }

std::optional<double> ProfileReader::optional_number(const json::Value &parent,
                                        const std::string_view key,
                                        const std::string &path,
                                        const double minimum,
                                        const double maximum,
                                        const double fallback) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return fallback;
    }
    if (!value->is_number() || value->as_number() < minimum ||
        value->as_number() > maximum) {
      set_error(path + "." + std::string(key), "number is outside the supported range");
      return std::nullopt;
    }
    return value->as_number();
  }

ProfileError ProfileReader::error(const std::string &path, const std::string &message) {
    return ProfileError{.path = path, .message = message};
  }

bool ProfileReader::set_error(std::string path, std::string message) {
    if (error_.message.empty()) {
      error_ = ProfileError{.path = std::move(path), .message = std::move(message)};
    }
    return false;
  }

} // namespace layerc::core::detail
