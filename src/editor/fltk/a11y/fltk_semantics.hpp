#pragma once

// Toolkit-only accessibility queries of the FLTK tree pass. Nothing here
// includes X11, Win32, D-Bus or protocol headers: the platform seam owns screen
// origins, and protocol adaptors own their own types.

#include "editor_geometry.hpp"
#include "model.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Widget.H>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Fl_Input_;
class Fl_Menu_;
class Fl_Valuator;

namespace layerc::editor::a11y {
class RowGeometry;

// The equivalent of an ATK object's identity: a whole widget, one browser row,
// or one inline menu item of a menu widget.
enum class TargetKind { application, widget, row, menu_item };

struct Target {
  Fl_Widget *widget{};
  TargetKind kind{};
  int index{}; // Browser row (1-based) or menu item index.
};

// Fl_Menu_::size() walks the whole menu and every menu node asks several times
// per pass; a pass reuses each menu's size. Menus never change while a pass
// runs: accessibility requests only read widgets or defer application
// callbacks to FLTK's loop.
struct MenuSize {
  const Fl_Menu_ *menu;
  const Fl_Menu_Item *items;
  int size;
};

// A browser's row offsets for one pass. Row geometry walks the item list, so a
// pass that asks about every row reads it once per list, not once per row.
struct RowTops {
  const RowGeometry *list;
  std::vector<int> tops;
};
class TreePass final {
public:
  TreePass();
  ~TreePass();
  TreePass(const TreePass &) = delete;
  TreePass &operator=(const TreePass &) = delete;
private:
  std::vector<MenuSize> sizes_;
  std::vector<MenuSize> *previous_;
  std::vector<RowTops> rows_;
  std::vector<RowTops> *previous_rows_;
};
[[nodiscard]] int menu_size(const Fl_Menu_ &menu);
// One row's visible slice inside its list viewport, from the pass's row table.
[[nodiscard]] std::optional<EditorBounds> list_row_bounds(const RowGeometry &list, int row);
[[nodiscard]] const Fl_Menu_Item *menu_item_of(const Target &target);
[[nodiscard]] std::vector<int> menu_level(const Fl_Menu_ &menu, int first);

[[nodiscard]] std::string clean_label(const char *label);
// GTK accelerator notation, as AT-SPI clients expect, for printable keys.
[[nodiscard]] std::string accelerator(int shortcut);
[[nodiscard]] std::string mnemonic(const char *label);
// Repairs invalid UTF-8 instead of publishing text a protocol would reject.
[[nodiscard]] std::string valid_utf8(const std::string &text);

// The caller must obtain widget pointers from its still-live tracker. These
// toolkit-thread queries do not make an untracked pointer safe to dereference.
[[nodiscard]] bool live(const Target &target);
[[nodiscard]] bool exported(const Fl_Widget &widget);
[[nodiscard]] bool showing(const Target &target);
[[nodiscard]] bool enabled(const Target &target);
[[nodiscard]] bool focused(const Target &target);
[[nodiscard]] bool selected(const Target &target);
[[nodiscard]] bool checked(const Target &target);
[[nodiscard]] bool modal_allows(Fl_Widget &widget);
// Whether the node publishes an expansion state at all: a drop-down widget or a
// submenu title, never a leaf.
[[nodiscard]] bool expansion_published(const Target &target);
// Whether that node's popup is open now. Always false when none is published.
[[nodiscard]] bool expanded_now(const Target &target);
// The menu whose popup that node expands, or null when it publishes none.
[[nodiscard]] Fl_Menu_ *expansion_menu(const Target &target);

[[nodiscard]] Role role_of(const Target &target);
[[nodiscard]] std::string name(const Target &target);
[[nodiscard]] std::string description(const Target &target, std::string inherited = {});
[[nodiscard]] std::string keybinding(const Target &target);
[[nodiscard]] std::vector<std::string> action_names(const Target &target);
[[nodiscard]] States states(const Target &target);
// Widget-local logical bounds, or nothing when the target is not showing.
// Adaptors add the owning window's screen origin and their own scale.
[[nodiscard]] std::optional<Bounds> local_bounds(const Target &target);
[[nodiscard]] std::optional<Range> range_of(const Fl_Valuator &valuator);
// Scrolled content of a list or scroll pane; nothing for any other widget.
[[nodiscard]] std::optional<Scroll> scroll_of(const Target &target);
// Moves a list or scroll pane; a negative percentage keeps that axis. FLTK
// clamps the position, so a call always lands inside the content.
[[nodiscard]] bool set_scroll(const Target &target, double horizontal, double vertical);
[[nodiscard]] std::string input_text(const Target &target);
struct Selection {
  std::size_t begin{}, end{}; // UTF-8 byte offsets.
};
// One selection only, like today's ATK text interface.
[[nodiscard]] bool selection_of(const Fl_Input_ &field, Selection &selection);
[[nodiscard]] bool editable(const Target &target);

// Children in export order. `previous` keeps surviving children in place when
// the toolkit reorders its own list (FLTK reorders windows on activation).
[[nodiscard]] std::vector<Target> desired_children(const Target &target,
                                                  const std::vector<Target> &previous);

} // namespace layerc::editor::a11y
