#pragma once

#include "editor_commands.hpp"
#include "editor_geometry.hpp"

#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Double_Window.H>
#include <array>
#include <string>

class Fl_Menu_Bar;
class Fl_Menu_Button;

namespace layerc::editor {
class EditorApp;

// Linux frontend menus use the same accepted-edit commands as its buttons.
// Menu widgets are owned by the window; their callback data is owned here.
class EditorMenus final {
public:
  static constexpr int bar_height = 28;
  EditorMenus(EditorApp &app, Fl_Window &window);
  [[nodiscard]] Fl_Menu_Bar &bar() const { return *bar_; }
  void update();
  [[nodiscard]] bool shortcut(int key, int modifiers);
  void configure_context(Fl_Menu_Button &menu, EditorMenuContext context, bool has_target);
  void popup(EditorMenuContext context, bool has_target);
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
class ContextBrowser final : public Fl_Hold_Browser {
public:
  ContextBrowser(int x, int y, int width, int height, const char *label,
                 EditorApp &app, EditorMenuContext context);
  [[nodiscard]] int row_at(int x, int y);
  // The visible row slice in the browser viewport, not the entire browser.
  [[nodiscard]] std::optional<EditorBounds> row_bounds(int row);
  int handle(int event) override;
private:
  EditorApp &app_;
  EditorMenuContext context_;
};
} // namespace layerc::editor
