#pragma once

#include "layerc/core/profile.hpp"

#include <cstddef>
#include <optional>

class Fl_Box;
class Fl_Button;
class Fl_Group;
class Fl_Hold_Browser;
class Fl_Widget;

namespace layerc::editor {
class EditorApp;

// Ordered per-panel rows. FLTK owns these widgets; this object owns callbacks.
// Viewing a legacy panel never promotes it or allocates item IDs.
class PanelItemsControls final {
public:
  explicit PanelItemsControls(EditorApp &app) noexcept : app_(app) {}
  void build();
  void show(const core::GameProfile &profile, std::optional<std::size_t> panel);
  [[nodiscard]] bool owns(const Fl_Widget *widget) const;
private:
  friend struct EditorAppTestAccess;
  void edit(bool adding);
  void remove();
  void move(int direction);
  void select();
  [[nodiscard]] std::optional<std::size_t> selected() const;
  EditorApp &app_;
  Fl_Group *group_{};
  Fl_Hold_Browser *items_{};
  Fl_Box *reason_{};
  Fl_Button *add_{}, *edit_{}, *remove_{}, *up_{}, *down_{};
};

} // namespace layerc::editor
