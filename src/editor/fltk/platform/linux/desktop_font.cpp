#include "platform/services.hpp"

#include <FL/Fl.H>
#include <FL/platform.H>

#include <span>
#include <string>

namespace layerc::editor {

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

} // namespace layerc::editor
