#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace layerc::core::detail {

[[nodiscard]] bool is_valid_utf8(std::string_view text);
void append_utf8(std::string &output, std::uint32_t codepoint);

} // namespace layerc::core::detail