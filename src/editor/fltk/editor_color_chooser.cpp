#include "appearance_window.hpp"
#include "accessible_widgets.hpp"
#include "editor_style.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Value_Input.H>
#include <algorithm>
#include <array>
#include <cmath>

namespace layerc::editor {
namespace {
// Keep FLTK's hue plane, value control, numeric editing and mode conversion.
// Only the shell and its mode popup are editor-owned; no private FLTK API.
class ColorDialog final : public Fl_Double_Window {
public:
  explicit ColorDialog(const char *title, std::uint32_t initial)
      : Fl_Double_Window(215, 1, title) {
    const int control_height = kit::metric(kit::Metric::control_height);
    const int combo_height = kit::metric(kit::Metric::combo_height);
    const int first_channel = std::max(30, combo_height);
    const int pitch = std::max(30, control_height);
    const int chooser_height = std::max(115, first_channel + 2 * pitch + control_height);
    const int extra = chooser_height - 115;
    chooser_ = new Fl_Color_Chooser(10, 10, 195, chooser_height);
    Fl_Choice *native_mode{};
    int area = 0, channel_y = chooser_->y() + first_channel;
    for (int i = 0; i < chooser_->children(); ++i) {
      auto &child = *chooser_->child(i);
      if (auto *choice = dynamic_cast<Fl_Choice *>(&child)) native_mode = choice;
      else if (auto *value = dynamic_cast<Fl_Value_Input *>(&child)) {
        value->resize(value->x(), channel_y, value->w(), control_height);
        channel_y += pitch;
      } else if (!dynamic_cast<Fl_Box *>(&child))
        accessibility::set_accessible_name(child, area++ == 0 ? "Hue and saturation" : "Value");
    }
    // The embedded choice cannot be subclassed. Replace only that control,
    // using its public geometry/menu, and keep mode() as the native authority.
    if (native_mode) {
      chooser_->begin();
      mode_ = new EditorChoice(native_mode->x(), native_mode->y(), native_mode->w(), combo_height);
      mode_->copy(native_mode->menu());
      accessibility::set_accessible_name(*mode_, "Color mode");
      native_mode->hide();
      chooser_->end();
      mode_->value(2);
      mode_->callback([](Fl_Widget *, void *data) {
        auto &self = *static_cast<ColorDialog *>(data);
        self.chooser_->mode(self.mode_->value()); self.name_channels();
      }, this);
    }
    chooser_->mode(2);
    chooser_->rgb(static_cast<double>((initial >> 16U) & 255U) / 255.0,
                   static_cast<double>((initial >> 8U) & 255U) / 255.0,
                   static_cast<double>(initial & 255U) / 255.0);
    name_channels();
    begin();
    current_ = new Fl_Box(FL_DOWN_BOX, 10, 130 + extra, 95, 25, nullptr);
    auto *original = new Fl_Box(FL_DOWN_BOX, 110, 130 + extra, 95, 25, nullptr);
    original->color(fl_rgb_color(static_cast<uchar>(initial >> 16U),
                                 static_cast<uchar>(initial >> 8U), static_cast<uchar>(initial)));
    auto *ok = new Fl_Return_Button(10, 165 + extra, 95, control_height, "OK");
    primary_button(*ok);
    ok->callback([](Fl_Widget *, void *data) {
      auto &self = *static_cast<ColorDialog *>(data); self.accepted = true; self.hide();
    }, this);
    auto *cancel = new Fl_Button(110, 165 + extra, 95, control_height, "Cancel");
    cancel->shortcut(FL_Escape);
    cancel->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    chooser_->callback([](Fl_Widget *, void *data) { static_cast<ColorDialog *>(data)->update_chip(); }, this);
    update_chip();
    end(); set_modal();
    size(215, 175 + extra + control_height);
    // This compact native chooser has fixed-size fields and actions.
    size_range(w(), h(), w(), h());
  }
  bool accepted{};
  std::uint32_t color_value() const {
    return (static_cast<std::uint32_t>(std::lround(chooser_->r() * 255.0)) << 16U) |
           (static_cast<std::uint32_t>(std::lround(chooser_->g() * 255.0)) << 8U) |
            static_cast<std::uint32_t>(std::lround(chooser_->b() * 255.0));
  }
private:
  Fl_Color_Chooser *chooser_{};
  EditorChoice *mode_{};
  Fl_Box *current_{};
  void name_channels() {
    constexpr std::array rgb{"Red", "Green", "Blue"};
    constexpr std::array hsv{"Hue", "Saturation", "Value"};
    const auto &names = chooser_->mode() == 3 ? hsv : rgb;
    std::size_t channel = 0;
    for (int i = 0; i < chooser_->children(); ++i)
      if (auto *value = dynamic_cast<Fl_Value_Input *>(chooser_->child(i)); value && channel < names.size())
        accessibility::set_accessible_name(*value, names[channel++]);
  }
  void update_chip() {
    const auto value = color_value();
    current_->color(fl_rgb_color(static_cast<uchar>(value >> 16U),
                                 static_cast<uchar>(value >> 8U), static_cast<uchar>(value)));
    current_->redraw();
  }
};
}
std::optional<std::uint32_t> choose_color(const char *title, const std::uint32_t initial) {
  ColorDialog dialog(title, initial);
  dialog.show();
  while (dialog.shown()) Fl::wait();
  return dialog.accepted ? std::optional{dialog.color_value()} : std::nullopt;
}
} // namespace layerc::editor
