#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace layerc::core {

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
[[nodiscard]] bool is_lowercase_sha256(std::string_view value) noexcept;

} // namespace layerc::core
