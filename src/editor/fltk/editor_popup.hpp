#pragma once

#include "editor_geometry.hpp"
#include <FL/Fl_Menu_Bar.H>
#include <optional>

namespace layerc::editor {

// Tracking, drawing and accessibility share this one source of popup state.
// It owns transient windows, never application commands or menu arrays.
void track_editor_popup(Fl_Menu_ &owner, int title = -1,
                        std::optional<EditorBounds> anchor = std::nullopt);
[[nodiscard]] Fl_Menu_ *popup_owner();
[[nodiscard]] bool popup_current(const Fl_Menu_ &owner, int index);
[[nodiscard]] std::optional<EditorBounds> popup_item_bounds(const Fl_Menu_ &owner, int index);
[[nodiscard]] Fl_Window *popup_document_window(const Fl_Window *transient);
// An accessible action closes tracking before Fl_Menu_::picked runs callbacks.
[[nodiscard]] bool popup_pick(Fl_Menu_ &owner, int index);

class EditorMenuBar final : public Fl_Menu_Bar {
public:
  using Fl_Menu_Bar::Fl_Menu_Bar;
  int handle(int event) override;
protected:
  void draw() override;
};
} // namespace layerc::editor
