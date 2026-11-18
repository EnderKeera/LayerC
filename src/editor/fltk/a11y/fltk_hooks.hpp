#pragma once
// Frontend seams for the FLTK tree pass and actions. The accessibility layer
// knows FLTK's own widgets; editor widgets that lay out their own rows or run
// their own popup menus describe them here, so this layer never links the
// editor frontend, its styling or its platform services, and both platform
// adaptors share it.
#include "editor_geometry.hpp"

#include <optional>
#include <vector>

class Fl_Menu_;
class Fl_Window;

namespace layerc::editor::a11y {
// A browser whose rows are not FLTK's uniform rows. Entry i of row_tops() is
// where row i + 1 starts and the last entry is where the final row ends;
// row_bounds() is the visible slice of a 1-based row in window coordinates.
class RowGeometry {
public:
  virtual ~RowGeometry() = default;
  [[nodiscard]] virtual std::vector<int> row_tops() const = 0;
  [[nodiscard]] virtual std::optional<EditorBounds>
  row_bounds(int row, const std::vector<int> &tops) const = 0;
};

// The frontend's own popup menus. An empty member means the frontend has no
// such popup, which is also the state before anything is installed.
struct PopupMenus {
  // The menu whose popup is open, or nullptr.
  Fl_Menu_ *(*owner)(){};
  // Whether an item of the open popup is its current (highlighted) item.
  bool (*current)(const Fl_Menu_ &owner, int index){};
  // An open popup item's bounds in its document window's coordinates.
  std::optional<EditorBounds> (*item_bounds)(const Fl_Menu_ &owner, int index){};
  // The document window behind a popup's own modal window, or nullptr.
  Fl_Window *(*document_window)(const Fl_Window *transient){};
  // Chooses an item of the open popup; false when owner has none open.
  bool (*pick)(Fl_Menu_ &owner, int index){};
  // Opens the popup of a menu title.
  void (*open)(Fl_Menu_ &owner, int title){};
};
// FLTK thread only. The frontend that owns popup menus installs them once.
void set_popup_menus(const PopupMenus &menus) noexcept;

[[nodiscard]] Fl_Menu_ *popup_owner();
[[nodiscard]] bool popup_current(const Fl_Menu_ &owner, int index);
[[nodiscard]] std::optional<EditorBounds> popup_item_bounds(const Fl_Menu_ &owner, int index);
[[nodiscard]] Fl_Window *popup_document_window(const Fl_Window *transient);
[[nodiscard]] bool popup_pick(Fl_Menu_ &owner, int index);
// False when no frontend popup can open, so the caller falls back to FLTK.
[[nodiscard]] bool popup_open(Fl_Menu_ &owner, int title);
} // namespace layerc::editor::a11y
