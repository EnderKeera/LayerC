#include "layerc/platform/text.hpp"

#include "layerc/core/utf8.hpp"

namespace layerc::platform {

std::optional<std::string>
path_to_utf8(const std::filesystem::path &path) {
  try {
    return try_wide_to_utf8(path.generic_wstring());
  } catch (const std::filesystem::filesystem_error &) {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path>
path_from_utf8(const std::string_view text) {
  if (!core::is_valid_utf8(text)) return std::nullopt;
  const auto wide = try_utf8_to_wide(text);
  return wide.has_value() ? std::optional(std::filesystem::path(*wide))
                          : std::nullopt;
}

} // namespace layerc::platform
