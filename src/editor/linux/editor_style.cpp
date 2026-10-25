#include "editor_style.hpp"
#include "editor_appearance.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace layerc::editor {
namespace {

// FLTK's first free slot; registered only by install_editor_style.
constexpr Fl_Boxtype menu_bar_box = FL_FREE_BOXTYPE;
bool installed = false;

std::uint32_t rgb_of(const Fl_Color color) {
  uchar r{}, g{}, b{};
  Fl::get_color(color, r, g, b);
  return (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
}

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

void flat_frame(const int x, const int y, const int w, const int h, const Fl_Color color) {
  fl_color(outline_for(Fl::box_color(color)));
  fl_rect(x, y, w, h);
}

void flat_box(const int x, const int y, const int w, const int h, const Fl_Color color) {
  const Fl_Color fill = Fl::box_color(color);
  fl_rectf(x, y, w, h, fill);
  fl_color(outline_for(fill));
  fl_rect(x, y, w, h);
}

void bar_box(const int x, const int y, const int w, const int h, const Fl_Color color) {
  const Fl_Color fill = Fl::box_color(color);
  fl_rectf(x, y, w, h, fill);
  fl_color(fl_color_average(outline_for(fill), fill, 0.5F));
  fl_xyline(x, y + h - 1, x + w - 1);
}

// Gtk/FontName from the XSETTINGS manager of FLTK's screen, if one runs.
std::optional<std::string> desktop_font_description() {
  fl_open_display();
  Display *display = fl_display;
  if (display == nullptr) return std::nullopt;
  const auto selection = "_XSETTINGS_S" + std::to_string(fl_screen);
  const Atom selection_atom = XInternAtom(display, selection.c_str(), True);
  const Atom settings_atom = XInternAtom(display, "_XSETTINGS_SETTINGS", True);
  if (selection_atom == None || settings_atom == None) return std::nullopt;
  const Window owner = XGetSelectionOwner(display, selection_atom);
  if (owner == None) return std::nullopt;
  Atom type{};
  int format{};
  unsigned long count{}, remaining{};
  unsigned char *data{};
  const int status = XGetWindowProperty(display, owner, settings_atom, 0, 64 * 1024 / 4, False,
                                        AnyPropertyType, &type, &format, &count, &remaining,
                                        &data);
  std::optional<std::string> font;
  if (status == Success && data != nullptr && format == 8)
    font = xsettings_string(std::span<const unsigned char>(data, count), "Gtk/FontName");
  if (data != nullptr) XFree(data);
  return font;
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
  // The forms are authored at FLTK's 14 px; a smaller desktop size is kept.
  if (font->pixels) FL_NORMAL_SIZE = std::clamp(*font->pixels, 11, 14);
}

} // namespace

void install_editor_style() {
  if (installed) return;
  installed = true;
  for (const auto box : {FL_UP_BOX, FL_DOWN_BOX, FL_THIN_UP_BOX, FL_THIN_DOWN_BOX,
                         FL_ENGRAVED_BOX, FL_EMBOSSED_BOX})
    Fl::set_boxtype(box, flat_box, 1, 1, 2, 2);
  for (const auto frame : {FL_UP_FRAME, FL_DOWN_FRAME, FL_THIN_UP_FRAME, FL_THIN_DOWN_FRAME,
                           FL_ENGRAVED_FRAME, FL_EMBOSSED_FRAME})
    Fl::set_boxtype(frame, flat_frame, 1, 1, 2, 2);
  Fl::set_boxtype(menu_bar_box, bar_box, 0, 0, 0, 1);
  apply_desktop_font();
}

Fl_Boxtype editor_menu_bar_box() noexcept { return installed ? menu_bar_box : FL_UP_BOX; }

} // namespace layerc::editor
