#pragma once

#include "a11y/fltk_hooks.hpp"
#include "editor_appearance.hpp"
#include "editor_commands.hpp"
#include "editor_geometry.hpp"

#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Double_Window.H>
#include <array>
#include <string>
#include <vector>

class Fl_Menu_Bar;
class Fl_Menu_Button;

namespace layerc::editor {
class EditorApp;

// Linux frontend menus use the same accepted-edit commands as its buttons.
// Menu widgets are owned by the window; their callback data is owned here.
// Empty list copy: what the list says before anything exists, drawn inside the
// list itself (DesignSystem.md, EmptyState).
struct EditorEmptyState {
  const char *title;
  const char *hint;
};
[[nodiscard]] EditorEmptyState editor_empty_state(EditorMenuContext context) noexcept;
void draw_editor_empty_state(const EditorBounds &bounds, EditorEmptyState copy,
                             Fl_Font face, Fl_Fontsize size);

class EditorMenus final {
public:
  static constexpr int bar_height = 28;
  EditorMenus(EditorApp &app, Fl_Window &window);
  [[nodiscard]] Fl_Menu_Bar &bar() const { return *bar_; }
  void update();
  [[nodiscard]] bool shortcut(int key, int modifiers);
  void configure_context(Fl_Menu_Button &menu, EditorMenuContext context, bool has_target);
  void popup(EditorMenuContext context, bool has_target,
             std::optional<EditorBounds> anchor = std::nullopt);
  // One "shortcut<TAB>action" line per keyboard shortcut.
  [[nodiscard]] static std::string shortcut_help();
private:
  struct Action { EditorApp *app{}; EditorCommand command{}; };
  std::array<Action, static_cast<std::size_t>(EditorCommand::count)> actions_{};
  EditorApp &app_;
  Fl_Window &window_;
  Fl_Menu_Bar *bar_{};
  static void invoke(Fl_Widget *widget, void *data);
};

// Document shortcuts precede a focused text field's local undo, while
// ordinary text editing and other windows' modal input remain native.
class EditorWindow final : public Fl_Double_Window {
public:
  EditorWindow(EditorApp &app, int width, int height, const char *title);
  ~EditorWindow() override;
  EditorWindow(const EditorWindow &) = delete;
  EditorWindow &operator=(const EditorWindow &) = delete;
  int handle(int event) override;
private:
  EditorApp &app_;
  [[nodiscard]] bool document_shortcut();
  static int dispatch(int event, Fl_Window *window);
};

// Hit-testing uses FLTK's actual row geometry, including scroll position;
// right-clicking empty space never silently targets the previous selection.
class ContextBrowser final : public Fl_Hold_Browser, public a11y::RowGeometry,
                             public PaintedTextSource {
public:
  ContextBrowser(int x, int y, int width, int height, const char *label,
                 EditorApp &app, EditorMenuContext context);
  [[nodiscard]] std::span<const PaintedText> painted_text() const noexcept override;
  [[nodiscard]] int row_at(int x, int y);
  // The visible row slice in the browser viewport, not the entire browser.
  [[nodiscard]] std::optional<EditorBounds> row_bounds(int row);
  // The same slice from a row table a wider walk already read.
  [[nodiscard]] std::optional<EditorBounds>
  row_bounds(int row, const std::vector<int> &tops) const override;
  // Window y offsets for one walk of the items, anchored at the scrolled
  // viewport top: entry i is where row i+1 starts and entry size() is where the
  // last row ends. A caller asking about every row reads this once instead of
  // walking the item list per row.
  [[nodiscard]] std::vector<int> row_tops() const override;
  int handle(int event) override;
protected:
  void draw() override;
private:
  EditorApp &app_;
  EditorMenuContext context_;
};
} // namespace layerc::editor
