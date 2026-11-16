#include "editor_style.hpp"
#include "editor_appearance.hpp"
#include "platform/services.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Input_.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace layerc::editor {
namespace {

// FLTK's first free slot; registered only by install_editor_style.
constexpr Fl_Boxtype menu_bar_box = FL_FREE_BOXTYPE;
constexpr auto primary_box = static_cast<Fl_Boxtype>(FL_FREE_BOXTYPE + 2);
bool installed = false;

std::uint32_t rgb_of(const Fl_Color color) {
  uchar r{}, g{}, b{};
  Fl::get_color(color, r, g, b);
  return (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
}

// The appearance the authored drawing resolves its tokens from; the dialogs
// and the shell set it when they apply an appearance.
EditorAppearance style_appearance;

Fl_Color fl_of(const std::uint32_t rgb) {
  return fl_rgb_color(static_cast<uchar>(rgb >> 16U), static_cast<uchar>(rgb >> 8U),
                      static_cast<uchar>(rgb));
}

// An inactive control's outline fades halfway into its fill.
Fl_Color outline_for(const Fl_Color fill) {
  const auto rgb = rgb_of(fill);
  const auto border = fl_of(control_border(rgb));
  return Fl::draw_box_active() ? border : fl_color_average(border, fill, 0.5F);
}

bool has_visible_focus(const Fl_Widget *widget) {
  return widget && widget->active_r() && widget->visible_focus() && Fl::visible_focus();
}

void focused_frame(const int x, const int y, const int w, const int h) {
  const auto *focus = Fl::focus();
  if (!has_visible_focus(focus) || focus->x() != x || focus->y() != y ||
      focus->w() != w || focus->h() != h) return;
  if (dynamic_cast<const Fl_Input_ *>(focus)) draw_editor_focus(x, y, w, h);
  else if (dynamic_cast<const Fl_Browser_ *>(focus)) {
    fl_color(fl_of(current_editor_tokens().accent));
    fl_rectf(x, y + std::max(0, h - 2), w, std::min(h, 2));
  }
}

// FLTK registers this as a boxtype focus callback: the box type and both
// colours come from Fl, the ring uses this widget's own tokens.
void focus_box(Fl_Boxtype, const int x, const int y, const int w, const int h,
               Fl_Color, Fl_Color) {
  const auto *focus = Fl::focus();
  if (!has_visible_focus(focus)) return;
  if (dynamic_cast<const Fl_Browser_ *>(focus) && focus->x() == x &&
      focus->y() == y && focus->w() == w && focus->h() == h) {
    focused_frame(x, y, w, h);
    return;
  }
  draw_editor_focus(x, y, w, h, is_primary_button(*focus));
}

void no_box(int, int, int, int, Fl_Color) {}
void solid_box(int x, int y, int w, int h, Fl_Color color) {
  fl_rectf(x, y, w, h, Fl::box_color(color));
}

void flat_frame(const int x, const int y, const int w, const int h, const Fl_Color color) {
  fl_color(outline_for(Fl::box_color(color)));
  fl_rect(x, y, w, h);
  focused_frame(x, y, w, h);
}

void flat_box(const int x, const int y, const int w, const int h, const Fl_Color color) {
  const Fl_Color fill = Fl::box_color(color);
  fl_rectf(x, y, w, h, fill);
  fl_color(outline_for(fill));
  fl_rect(x, y, w, h);
  focused_frame(x, y, w, h);
}

void bar_box(const int x, const int y, const int w, const int h, const Fl_Color color) {
  const Fl_Color fill = Fl::box_color(color);
  fl_rectf(x, y, w, h, fill);
  fl_color(fl_color_average(outline_for(fill), fill, 0.5F));
  fl_xyline(x, y + h - 1, x + w - 1);
}

// FLTK names its sans faces " sans"/"Bsans" (Xft) or "Sans Bold" (Pango);
// replacing "sans" keeps each face's style marker.
void apply_desktop_font() {
  const auto description = desktop_font_description();
  const auto font =
      description ? parse_desktop_font(*description) : std::optional<DesktopFont>{};
  if (!font) return;
  static std::array<std::string, 4> names;
  static constexpr std::array<Fl_Font, 4> faces{FL_HELVETICA, FL_HELVETICA_BOLD,
                                                FL_HELVETICA_ITALIC, FL_HELVETICA_BOLD_ITALIC};
  for (std::size_t index = 0; index < faces.size(); ++index) {
    const char *current = Fl::get_font(faces[index]);
    if (current == nullptr) continue;
    std::string name = current;
    std::string folded = name;
    for (auto &c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto at = folded.find("sans");
    if (at == std::string::npos) continue;
    names[index] = name.substr(0, at) + font->family + name.substr(at + 4);
    Fl::set_font(faces[index], names[index].c_str());
  }
  // The forms are authored at FLTK's 14 px. A larger desktop size scales this
  // app's screens; XSETTINGS and the rest of the desktop are never touched
  // (DesignSystem.md, Density).
  const auto metrics = desktop_text_metrics(font->pixels);
  FL_NORMAL_SIZE = metrics.normal_size;
  for (int screen = 0; screen < Fl::screen_count(); ++screen)
    Fl::screen_scale(screen, Fl::screen_scale(screen) * metrics.scale);
}

} // namespace

void install_editor_style() {
  if (installed) return;
  installed = true;
  for (const auto box : {FL_UP_BOX, FL_DOWN_BOX, FL_THIN_UP_BOX, FL_THIN_DOWN_BOX,
                         FL_ENGRAVED_BOX, FL_EMBOSSED_BOX})
    Fl::set_boxtype(box, flat_box, 1, 1, 2, 2, focus_box);
  for (const auto frame : {FL_UP_FRAME, FL_DOWN_FRAME, FL_THIN_UP_FRAME, FL_THIN_DOWN_FRAME,
                           FL_ENGRAVED_FRAME, FL_EMBOSSED_FRAME})
    Fl::set_boxtype(frame, flat_frame, 1, 1, 2, 2, focus_box);
  Fl::set_boxtype(FL_BORDER_BOX, flat_box, 1, 1, 2, 2, focus_box);
  Fl::set_boxtype(FL_FLAT_BOX, solid_box, 0, 0, 0, 0, focus_box);
  Fl::set_boxtype(FL_NO_BOX, no_box, 0, 0, 0, 0, focus_box);
  Fl::set_boxtype(primary_box, flat_box, 1, 1, 2, 2, focus_box);
  Fl::set_boxtype(menu_bar_box, bar_box, 0, 0, 0, 1, focus_box);
  apply_desktop_font();
}

Fl_Boxtype editor_menu_bar_box() noexcept { return installed ? menu_bar_box : FL_UP_BOX; }

void set_editor_style_appearance(const EditorAppearance &appearance) {
  style_appearance = appearance;
}

EditorTokens current_editor_tokens() {
  if (style_appearance.theme != EditorTheme::system)
    return editor_tokens(style_appearance);
  // System keeps FLTK's own palette: the tokens follow it so authored drawing
  // stays legible instead of imposing the dark ramp (DesignSystem.md, Themes).
  EditorTokens tokens = editor_tokens(style_appearance);
  tokens.background = rgb_of(FL_BACKGROUND_COLOR);
  tokens.panel = tokens.background;
  tokens.input = rgb_of(FL_BACKGROUND2_COLOR);
  tokens.text = rgb_of(FL_FOREGROUND_COLOR);
  tokens.secondary = tokens.subtext = tokens.verbose = tokens.text;
  tokens.accent = style_appearance.accent.value_or(rgb_of(FL_SELECTION_COLOR));
  tokens.selection = rgb_of(FL_SELECTION_COLOR);
  tokens.on_accent = rgb_of(fl_contrast(FL_FOREGROUND_COLOR, fl_of(tokens.accent)));
  tokens.yes = tokens.no = tokens.warning = tokens.accent_2 = tokens.text;
  tokens.on_status = tokens.background;
  return tokens;
}

void draw_editor_focus(int x, int y, int width, int height, bool primary) {
  if (width < 4 || height < 4) return;
  const auto tokens = current_editor_tokens();
  const auto color = primary ? (style_appearance.theme == EditorTheme::system
      ? tokens.on_accent : tokens.text) : tokens.accent;
  fl_color(fl_of(color));
  fl_rect(x, y, width, height);
  fl_rect(x + 1, y + 1, width - 2, height - 2);
}

bool is_primary_button(const Fl_Widget &widget) noexcept {
  return widget.box() == primary_box;
}

void refresh_primary_button(Fl_Button &button) {
  const auto tokens = current_editor_tokens();
  button.color(fl_of(tokens.accent));
  button.selection_color(fl_color_average(fl_of(tokens.accent), fl_of(tokens.panel),
                                           195.0F / 255.0F));
  button.labelcolor(fl_of(tokens.on_accent));
  button.redraw();
}

void primary_button(Fl_Button &button) {
  button.box(primary_box);
  button.down_box(primary_box);
  refresh_primary_button(button);
}

void EditorChoice::draw() {
  draw_box(box(), color());
  fl_push_clip(x() + 6, y() + 2, std::max(0, w() - 28), std::max(0, h() - 4));
  if (const auto *item = mvalue(); item && item->label()) {
    Fl_Label selected{};
    selected.value = item->label();
    selected.font = textfont();
    selected.size = textsize();
    selected.color = active_r() ? textcolor() : fl_inactive(textcolor());
    selected.type = item->labeltype();
    const auto shortcuts = fl_draw_shortcut;
    fl_draw_shortcut = 0;
    selected.draw(x() + 6, y() + 2, std::max(0, w() - 28), std::max(0, h() - 4),
                  FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    fl_draw_shortcut = shortcuts;
  }
  fl_pop_clip();
  fl_color(active_r() ? textcolor() : fl_inactive(textcolor()));
  const int center_x = x() + w() - 13, center_y = y() + h() / 2;
  fl_line_style(FL_SOLID, 2);
  fl_line(center_x - 3, center_y - 1, center_x, center_y + 2);
  fl_line(center_x, center_y + 2, center_x + 3, center_y - 1);
  fl_line_style(FL_SOLID, 0);
  if (Fl::focus() == this && has_visible_focus(this))
    draw_editor_focus(x(), y(), w(), h());
  draw_label();
}

EditorStatusMark::EditorStatusMark(int x, int y, int width, int height)
    : Fl_Box(x, y, width, height) {
  box(FL_FLAT_BOX);
  clear_visible_focus();
}
void EditorStatusMark::kind(const StatusKind value) {
  if (kind_ != value) { kind_ = value; redraw(); }
}
void EditorStatusMark::draw() {
  draw_box();
  const int top = y() + std::max(0, (std::min(h(), FL_NORMAL_SIZE + 8) - 12) / 2);
  draw_editor_status_glyph(kind_, x() + (w() - 12) / 2, top);
}

void draw_editor_status_glyph(const StatusKind kind, const int x, const int y,
                              const int size) {
  if (kind == StatusKind::none || size <= 0) return;
  const auto tokens = current_editor_tokens();
  const auto color = [&] {
    switch (kind) {
    case StatusKind::failure: return fl_of(tokens.no);
    case StatusKind::warning: return fl_of(tokens.warning);
    case StatusKind::success: return fl_of(tokens.yes);
    default: return fl_of(tokens.accent_2);
    }
  }();
  // The glyph itself is colored ink, not a tinted message background.
  const int unit = std::max(1, size / 6);
  fl_color(color);
  fl_line_style(FL_SOLID, std::max(2, unit));
  const int left = x + unit, right = x + size - unit;
  const int top = y + unit, bottom = y + size - unit;
  const int middle = y + size / 2;
  switch (kind) {
  case StatusKind::success:
    // A tick: down to the low point, then up to the right shoulder.
    fl_line(left, middle, x + size / 2, bottom);
    fl_line(x + size / 2, bottom, right, top + unit / 2);
    break;
  case StatusKind::failure:
    fl_line(left, top, right, bottom);
    fl_line(left, bottom, right, top);
    break;
  case StatusKind::warning:
    // An exclamation: the stem, then its dot.
    fl_line(x + size / 2, top, x + size / 2, bottom - unit * 2);
    fl_rectf(x + size / 2 - unit / 2, bottom - unit, unit, unit);
    break;
  default:
    // Information: three dots.
    for (int dot = 0; dot < 3; ++dot)
      fl_rectf(x + unit + dot * (size - 2 * unit) / 2 - unit / 2, middle - unit / 2,
               unit, unit);
    break;
  }
  fl_line_style(FL_SOLID, 0);
}

} // namespace layerc::editor
