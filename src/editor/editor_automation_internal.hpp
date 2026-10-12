#pragma once

#include "automation_graph.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace layerc::editor {

inline constexpr UINT_PTR automation_simulator_timer = 94'001;

using layerc::core::expr::literal;

// Shared output uses LF; the native multiline edit control needs CRLF.
[[nodiscard]] std::wstring trace_window_text(std::string_view text);

[[nodiscard]] inline constexpr LPARAM automation_tree_item_data(
    const int rule, const int action) noexcept {
  return static_cast<LPARAM>((rule + 1) | ((action + 1) << 16));
}

[[nodiscard]] inline constexpr int
automation_tree_rule(const LRESULT data) noexcept {
  return static_cast<int>(static_cast<unsigned long long>(data) & 0xffffU) - 1;
}

[[nodiscard]] inline constexpr int
automation_tree_action(const LRESULT data) noexcept {
  return static_cast<int>((static_cast<unsigned long long>(data) >> 16) &
                          0xffffU) -
         1;
}

} // namespace layerc::editor
