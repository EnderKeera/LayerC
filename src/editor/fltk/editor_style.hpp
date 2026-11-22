#pragma once

#include "platform/contrast.hpp"
#include "status_kind.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>

#include <cstdint>
#include <string_view>

namespace layerc::editor {

// LayerE's flat, square look on every FLTK surface, matching the Windows
// editor: each up/down/thin/engraved box is a solid fill inside a 1 px
// outline (control_border), and the menu bar keeps only a bottom divider.
// Also adopts the desktop's interface font (XSETTINGS Gtk/FontName, else
// fontconfig's sans) for FLTK's sans faces, at a size the fixed form geometry
// fits. Call once after the display opens and before any widget is built.
void install_editor_style();

// The menu bar's box once the style is installed, else FL_UP_BOX.
[[nodiscard]] Fl_Boxtype editor_menu_bar_box() noexcept;

// One height per single-line control class: authored forms never repeat the
// number (DesignSystem.md, Density).
inline constexpr int editor_input_height = 28;
inline constexpr int editor_button_height = 30;

// The appearance the authored drawing resolves its tokens from.
void set_editor_style_appearance(const EditorAppearance &appearance);
[[nodiscard]] const EditorAppearance &current_editor_style_appearance() noexcept;
[[nodiscard]] const std::optional<ContrastPalette> &editor_style_contrast_palette() noexcept;
[[nodiscard]] RoleColors current_editor_toolbar_colors();

// Paints the status strip's leading mark from the producer's semantic kind.
void draw_editor_status_glyph(StatusKind kind, int x, int y, int size = 12);

// The palette authored drawing resolves for the applied appearance.
[[nodiscard]] EditorTokens current_editor_tokens();

void draw_editor_focus(int x, int y, int width, int height, bool primary = false);
void primary_button(Fl_Button &button);
[[nodiscard]] bool is_primary_button(const Fl_Widget &widget) noexcept;
void refresh_primary_button(Fl_Button &button);

// Redraw the whole field without changing its native editing behavior.
template <class Base> class EditorField : public Base {
public:
  using Base::Base;
  int handle(int event) override {
    const int result = Base::handle(event);
    if (event == FL_FOCUS || event == FL_UNFOCUS) this->redraw();
    return result;
  }
protected:
  void draw() override {
    Base::draw();
    if (Fl::focus() == this && this->active_r() && this->visible_focus() &&
        Fl::visible_focus())
      draw_editor_focus(this->x(), this->y(), this->w(), this->h());
  }
};

class EditorChoice final : public Fl_Choice {
public:
  using Fl_Choice::Fl_Choice;
  int handle(int event) override;
protected:
  void draw() override;
};

// Decorative: the adjacent existing output owns the accessible message.
class EditorStatusMark final : public Fl_Box {
public:
  EditorStatusMark(int x, int y, int width, int height);
  void kind(StatusKind value);
  [[nodiscard]] StatusKind kind() const noexcept { return kind_; }
protected:
  void draw() override;
private:
  StatusKind kind_{StatusKind::none};
};

} // namespace layerc::editor
