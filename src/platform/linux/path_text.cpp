#include "layerc/platform/text.hpp"

#include "layerc/core/utf8.hpp"

namespace layerc::platform {

std::optional<std::string>
path_to_utf8(const std::filesystem::path &path) {
  try {
    const auto value = path.generic_string();
    return core::is_valid_utf8(value) ? std::optional(value) : std::nullopt;
  } catch (const std::filesystem::filesystem_error &) {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path>
path_from_utf8(const std::string_view text) {
  return core::is_valid_utf8(text)
             ? std::optional(std::filesystem::path(text))
             : std::nullopt;
}

} // namespace layerc::platform
