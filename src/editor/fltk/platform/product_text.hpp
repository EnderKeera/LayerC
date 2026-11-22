#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor {

[[nodiscard]] std::string_view editor_product_line() noexcept;
[[nodiscard]] std::string editor_window_title(
    std::optional<std::string_view> profile_name = std::nullopt, bool dirty = false);
[[nodiscard]] const char *editor_options_text() noexcept;
[[nodiscard]] const char *visual_tab_note() noexcept;
[[nodiscard]] const char *placement_drag_note() noexcept;

} // namespace layerc::editor
