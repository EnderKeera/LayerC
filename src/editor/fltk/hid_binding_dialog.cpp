#include "hid_binding_dialog.hpp"

#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "editor_style.hpp"
#include "profile_mutations.hpp"
#include "layerc/core/hid.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::editor {
namespace {

constexpr int hid_margin = 15;
constexpr int hid_width = 620;
constexpr int hid_height = 473;
constexpr int hid_content_width = hid_width - 2 * hid_margin;
constexpr int hid_action_width = 130;
constexpr int hid_action_gap = 10;
constexpr char system_warning[] =
    "Keyboard and mouse devices may also send normal input. One action can be observed twice.";
constexpr char replace_warning[] = "Replacing a slot can affect other profiles on this PC.";

class HidBindingDialog final : public Fl_Double_Window {
public:
  HidBindingDialog(HidBindingCapture &capture, const AcceptHidBinding &accept,
                   const HidBindingAnnouncement &announce)
      : Fl_Double_Window(hid_width, hid_height, "Capture HID binding"),
        capture_(capture), accept_(accept), announce_(announce) {
    begin();
    include_system_ = new Fl_Check_Button(hid_margin, hid_margin, hid_content_width, kit::metric(kit::Metric::control_height),
                                          "Include keyboard and mouse devices (advanced)");
    include_system_->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->scan(true);
    }, this);
    auto *system_note = new Fl_Box(hid_margin, 46, hid_content_width, 40, system_warning);
    system_note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    include_system_->tooltip(system_warning);
    accessibility::describe_with(*include_system_, *system_note);

    devices_ = new Fl_Hold_Browser(hid_margin, 112, hid_content_width, 96, "Connected devices");
    devices_->align(FL_ALIGN_TOP_LEFT);
    devices_->format_char(0);
    devices_->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->select_device();
    }, this);

    auto *refresh = new Fl_Button(hid_margin, 224, hid_action_width, kit::metric(kit::Metric::control_height), "Refresh devices");
    refresh->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->scan(true);
    }, this);
    listen_ = new Fl_Button(hid_margin + hid_action_width + hid_action_gap, 224, hid_action_width,
                            kit::metric(kit::Metric::control_height), "Capture");
    listen_->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->listen();
    }, this);
    status_mark_ = new EditorStatusMark(hid_margin, 267, 14, 16);
    status_ = new Fl_Box(hid_margin + 20, 267, hid_content_width - 20, 54);
    status_->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    accessibility::describe_with(*listen_, *status_);

    constexpr int field_width = (hid_content_width - 2 * hid_action_gap) / 3;
    const auto field = [](int column, const char *label) {
      auto *edit = new EditorField<Fl_Input>(hid_margin + column * (field_width + hid_action_gap),
                                             343, field_width, kit::metric(kit::Metric::control_height), label);
      edit->align(FL_ALIGN_TOP_LEFT);
      return edit;
    };
    slot_ = field(0, "Stable slot (1–32)");
    slot_->value("1");
    threshold_ = field(1, "Axis threshold (%)");
    hysteresis_ = field(2, "Axis hysteresis (%)");

    replace_ = new Fl_Check_Button(hid_margin, 378, hid_content_width, kit::metric(kit::Metric::control_height),
                                   "Allow replacing another device in this slot");
    replace_->tooltip(replace_warning);
    auto *replace_note = new Fl_Box(hid_margin + 20, 408, hid_content_width - 20, 18, replace_warning);
    replace_note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    accessibility::describe_with(*replace_, *replace_note);

    const int action_y = hid_height - hid_margin - kit::metric(kit::Metric::control_height);
    apply_ = new Fl_Return_Button(hid_width - hid_margin - 2 * hid_action_width - hid_action_gap,
                                  action_y, hid_action_width, kit::metric(kit::Metric::control_height), "Use binding");
    primary_button(*apply_);
    accessibility::describe_with(*apply_, *status_);
    apply_->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->apply();
    }, this);
    auto *cancel = new Fl_Button(hid_width - hid_margin - hid_action_width, action_y,
                                 hid_action_width, kit::metric(kit::Metric::control_height), "Cancel");
    cancel->callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->hide();
    }, this);
    callback([](Fl_Widget *, void *self) {
      static_cast<HidBindingDialog *>(self)->hide();
    }, this);
    end();
    set_modal();
    scan(false);
  }

  ~HidBindingDialog() override { capture_.stop(); }

  void hide() override {
    capture_.stop();
    Fl_Double_Window::hide();
  }

  [[nodiscard]] bool accepted() const noexcept { return accepted_; }

private:
  void status(const StatusKind kind, const std::string &message,
              const bool speak = true) {
    status_mark_->kind(kind);
    status_->copy_label(message.c_str());
    listen_->copy_tooltip(listen_->active() ? nullptr : message.c_str());
    apply_->copy_tooltip(apply_->active() ? nullptr : message.c_str());
    status_->redraw();
    if (speak && announce_) announce_(status_, message);
  }

  [[nodiscard]] bool axis() const noexcept {
    return control_ && (control_->activation == core::HidActivationMode::axis_positive ||
                        control_->activation == core::HidActivationMode::axis_negative);
  }

  void update_controls() {
    listen_->copy_label(listening_ ? "Listening..." : "Capture");
    if (devices_->value() > 0 && !listening_) listen_->activate();
    else listen_->deactivate();
    if (control_) {
      apply_->activate();
      slot_->activate();
      replace_->activate();
    } else {
      apply_->deactivate();
      slot_->deactivate();
      replace_->deactivate();
    }
    refresh_primary_button(*apply_);
    if (axis()) {
      threshold_->activate();
      hysteresis_->activate();
    } else {
      threshold_->deactivate();
      hysteresis_->deactivate();
    }
  }

  void forget_control() {
    capture_.stop();
    listening_ = false;
    control_.reset();
    replace_->value(0);
    threshold_->value("");
    hysteresis_->value("");
  }

  void scan(const bool speak) {
    forget_control();
    devices_->clear();
    for (const auto &name : capture_.scan(include_system_->value() != 0))
      devices_->add(name.c_str());
    if (devices_->size() != 0) devices_->value(1);
    update_controls();
    if (devices_->size() == 0) {
      status(StatusKind::information,
             "No matching devices. Connect a device and press Refresh devices.", speak);
    } else {
      status(StatusKind::information, "Choose a device and press Capture.", speak);
    }
  }

  void select_device() {
    forget_control();
    update_controls();
    status(StatusKind::information, "Press Capture for the selected device.", false);
  }

  void listen() {
    if (listening_ || devices_->value() < 1) return;
    const auto index = static_cast<std::size_t>(devices_->value() - 1);
    forget_control();
    const auto error = capture_.start(index, [this](HidCaptureUpdate update) {
      listening_ = update.listening;
      control_ = std::move(update.control);
      if (axis()) {
        threshold_->value(percent(control_->threshold).c_str());
        hysteresis_->value(percent(control_->hysteresis).c_str());
      }
      update_controls();
      const auto kind = control_ ? StatusKind::success :
                        listening_ ? StatusKind::information : StatusKind::warning;
      status(kind, update.text);
    });
    listening_ = !error;
    update_controls();
    if (error) status(StatusKind::failure, *error);
    else status(StatusKind::information, "Leave controls idle first, then move or press one control.");
  }

  static std::string percent(const double value) {
    return std::to_string(std::lround(value * 100));
  }

  void apply() {
    if (!control_ || listening_) return;
    const auto fail = [this](Fl_Widget &field, const char *text) {
      field.take_focus();
      status(StatusKind::failure, text);
    };
    auto binding = *control_;
    const auto slot = parse_count(slot_->value(), 1, 32);
    if (!slot) {
      fail(*slot_, "Stable slot must be a whole number from 1 to 32.");
      return;
    }
    binding.device.stable_slot = static_cast<std::uint16_t>(*slot);
    if (axis()) {
      const auto threshold = parse_count(threshold_->value(), 0, 100);
      const auto hysteresis = parse_count(hysteresis_->value(), 0, 49);
      if (!threshold) {
        fail(*threshold_, "Axis threshold must be a whole percent from 0 to 100.");
        return;
      }
      if (!hysteresis) {
        fail(*hysteresis_, "Axis hysteresis must be a whole percent from 0 to 49.");
        return;
      }
      // Compare whole percentages, then use the profile's normalized values.
      if (*hysteresis > std::min(*threshold, 100U - *threshold)) {
        fail(*hysteresis_, "Axis hysteresis crosses the threshold.");
        return;
      }
      binding.threshold = static_cast<double>(*threshold) / 100.0;
      // Keep an exactly equal percentage boundary valid after conversion.
      binding.hysteresis = std::min(static_cast<double>(*hysteresis) / 100.0,
                                   std::min(binding.threshold, 1.0 - binding.threshold));
    }
    binding.allow_system_class =
        include_system_->value() != 0 && core::is_system_keyboard_or_mouse(binding.device);
    if (core::is_system_keyboard_or_mouse(binding.device) && !binding.allow_system_class) {
      status(StatusKind::failure, "Keyboard and mouse devices require the advanced opt-in.");
      return;
    }
    if (const auto error = accept_(binding, replace_->value() != 0)) {
      status(StatusKind::failure, *error);
      return;
    }
    accepted_ = true;
    hide();
  }

  HidBindingCapture &capture_;
  const AcceptHidBinding &accept_;
  const HidBindingAnnouncement &announce_;
  Fl_Check_Button *include_system_{};
  Fl_Hold_Browser *devices_{};
  Fl_Button *listen_{};
  EditorStatusMark *status_mark_{};
  Fl_Box *status_{};
  Fl_Input *slot_{};
  Fl_Input *threshold_{};
  Fl_Input *hysteresis_{};
  Fl_Check_Button *replace_{};
  Fl_Return_Button *apply_{};
  std::optional<core::HidControlBinding> control_;
  bool listening_{};
  bool accepted_{};
};

} // namespace

bool capture_hid_binding(HidBindingCapture &capture, const EditorAppearance &appearance,
                         const AcceptHidBinding &accept,
                         const HidBindingAnnouncement &announce) {
  HidBindingDialog dialog(capture, accept, announce);
  apply_editor_appearance(appearance, dialog);
  dialog.show();
  while (dialog.shown()) Fl::wait();
  return dialog.accepted();
}

} // namespace layerc::editor
