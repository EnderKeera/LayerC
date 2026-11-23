#pragma once

#include "editor_appearance.hpp"

#include <cstdint>
#include <optional>
#include <vector>

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

// Styles a newly shown auxiliary window without changing the desktop palette.
void apply_editor_window_appearance(Fl_Window &window);

// The text pairs FLTK paints below `root`, walked like the
// theme application, hidden pages included and each pair once: a widget's
// text on its own fill when its box paints, else on the nearest painted
// surface beneath; outside labels in the Labels text on their parent; tab
// labels on the selected tab and, with a `preset`, on the Buttons fill of
// the others. Primary buttons have a separate accent-pair check; preview
// contents use the profile palette.
[[nodiscard]] std::vector<PaintedTextPair> painted_text_pairs(Fl_Widget &root,
                                                              const Fl_Widget *sidebar,
                                                              const Fl_Widget *preview,
                                                              bool preset);

// FLTK's colour chooser for a #RRGGBB value; nullopt when cancelled.
[[nodiscard]] std::optional<std::uint32_t> choose_color(const char *title, std::uint32_t initial);

// Modal Tools > Appearance: preset, accent and per-role colours, previewed
// live on `root`. Returns the applied choice, or nullopt when cancelled
// (the current appearance is restored).
[[nodiscard]] std::optional<EditorAppearance>
edit_editor_appearance(const EditorAppearance &current, Fl_Window &root,
                       const Fl_Widget *sidebar, const Fl_Widget *preview);

} // namespace layerc::editor
