#include "fltk_hooks.hpp"

namespace layerc::editor::a11y {
namespace {
// Constant-initialized, so a frontend installing its popups during static
// initialization never races this definition. Written once by the frontend
// before its first menu exists; read on the FLTK thread only.
constinit PopupMenus installed{};
} // namespace

void set_popup_menus(const PopupMenus &menus) noexcept { installed = menus; }

Fl_Menu_ *popup_owner() { return installed.owner ? installed.owner() : nullptr; }

bool popup_current(const Fl_Menu_ &owner, int index) {
  return installed.current && installed.current(owner, index);
}

std::optional<EditorBounds> popup_item_bounds(const Fl_Menu_ &owner, int index) {
  return installed.item_bounds ? installed.item_bounds(owner, index)
                               : std::optional<EditorBounds>{};
}

Fl_Window *popup_document_window(const Fl_Window *transient) {
  return installed.document_window ? installed.document_window(transient) : nullptr;
}

bool popup_pick(Fl_Menu_ &owner, int index) {
  return installed.pick && installed.pick(owner, index);
}

bool popup_open(Fl_Menu_ &owner, int title) {
  if (!installed.open) return false;
  installed.open(owner, title);
  return true;
}

bool popup_supported() { return installed.open != nullptr; }

bool popup_dismiss(Fl_Menu_ &owner) {
  return installed.dismiss && installed.dismiss(owner);
}

int popup_title(const Fl_Menu_ &owner) {
  return installed.title ? installed.title(owner) : -1;
}
} // namespace layerc::editor::a11y
