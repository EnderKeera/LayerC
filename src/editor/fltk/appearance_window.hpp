#pragma once

#include "editor_appearance.hpp"

#include <cstdint>
#include <optional>

class Fl_Widget;
class Fl_Window;

namespace layerc::editor {

void reapply_editor_desktop_appearance(Fl_Window &root, const Fl_Widget *sidebar = nullptr,
                                       const Fl_Widget *preview = nullptr);

// Sets FLTK's palette for the theme and each widget's role colours below
// `root`. The system theme restores FLTK's startup palette and class colours;
// `preview` keeps its own dark default and `sidebar` is the profile list.
void apply_editor_appearance(const EditorAppearance &appearance, Fl_Window &root,
                             const Fl_Widget *sidebar = nullptr,
                             const Fl_Widget *preview = nullptr);

// FLTK's colour chooser for a #RRGGBB value; nullopt when cancelled.
[[nodiscard]] std::optional<std::uint32_t> choose_color(const char *title, std::uint32_t initial);

// Modal Tools > Appearance: preset, accent and per-role colours, previewed
// live on `root`. Returns the applied choice, or nullopt when cancelled
// (the current appearance is restored).
[[nodiscard]] std::optional<EditorAppearance>
edit_editor_appearance(const EditorAppearance &current, Fl_Window &root,
                       const Fl_Widget *sidebar, const Fl_Widget *preview);

} // namespace layerc::editor
