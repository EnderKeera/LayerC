#pragma once

#include "editor_text_scale.hpp"

#include <optional>
#include <string_view>

namespace layerc::editor {

struct EditorOptions {
  bool visual_recognition{true};
  bool overlay_capture_exclusion{true};
  bool editor_capture_exclusion{};
  bool launcher_updates{};
  int text_scale{text_scale_choices.front()};
};

struct EditorOptionsLoad {
  EditorOptions options;
  std::string_view warning;
};

[[nodiscard]] bool editor_options_supported() noexcept;
void select_editor_text_scale(int percent);
[[nodiscard]] EditorOptionsLoad load_editor_options(bool editor_capture_exclusion, int text_scale);
void notify_editor_options_reload();
[[nodiscard]] std::optional<std::string_view>
commit_editor_options(const EditorOptions &options, void (*reload)() = notify_editor_options_reload);

[[nodiscard]] int current_editor_text_scale() noexcept;
// The stored preference does not imply every editor window accepted it.
[[nodiscard]] bool editor_capture_exclusion_requested() noexcept;
[[nodiscard]] std::string_view editor_capture_exclusion_warning() noexcept;
inline constexpr char capture_exclusion_incomplete[] =
    "Capture exclusion incomplete — the editor may appear in captures.";
inline constexpr char capture_exclusion_not_cleared[] =
    "Capture exclusion could not be cleared — the editor may remain hidden.";
void apply_editor_options_live(const EditorOptions &options);

} // namespace layerc::editor
