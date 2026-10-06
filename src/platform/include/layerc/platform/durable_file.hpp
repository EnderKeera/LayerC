#pragma once

// Durable file primitives live in the dependency-free layerc_durable library
// (shared with the launcher); this header adds the core text conveniences.
#include "layerc/core/utf8.hpp"
#include "layerc/durable/durable_file.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace layerc::platform {

[[nodiscard]] inline std::optional<std::string>
read_durable_text(const std::filesystem::path &path,
                  const std::size_t maximum_bytes) {
  auto result = read_durable_file(path, maximum_bytes);
  if (!result.has_value()) return std::nullopt;
  return layerc::core::as_text(*result);
}

} // namespace layerc::platform
