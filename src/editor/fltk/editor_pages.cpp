#include "editor_pages.hpp"
#include "platform/product_text.hpp"
#include "accessible_widgets.hpp"
#include "editor_app.hpp"
#include "overlay_contrast.hpp"
#include "platform/hid_binding_capture.hpp"
#include "platform/services.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>

#include <algorithm>
#include <cstring>

#include "editor_style.hpp"

namespace layerc::editor {
namespace {

// A button that issues its command through EditorApp, keeping the button's own
// guard messages for an unavailable action.
class CommandButton final : public Fl_Button {
public:
  CommandButton(int x, int y, int width, const char *label, EditorApp &app,
                EditorCommand action)
      : Fl_Button(x, y, width, editor_button_height, label), app_(app), action_(action) {
    callback([](Fl_Widget *, void *data) {
      auto &self = *static_cast<CommandButton *>(data);
      self.app_.command(self.action_);
    }, this);
  }
private:
  EditorApp &app_;
  EditorCommand action_;
};

class SteppedField final : public EditorField<Fl_Input> {
public:
  SteppedField(int x, int y, int width, const char *label, EditorApp &app,
               NumberStep step)
      : EditorField<Fl_Input>(x, y, width, editor_input_height, label), app_(app), step_(step) {
    accessibility::describe_with_tooltip(*this, step == NumberStep::duration
        ? "Up / Down or the mouse wheel step 0.1 s (Shift: 1 s)"
        : "Up / Down or the mouse wheel step 1 px");
  }

  int handle(const int event) override {
    if (Fl::focus() == this) {
      const int key = Fl::event_key();
      const int steps = event == FL_KEYDOWN ? (key == FL_Up ? 1 : key == FL_Down ? -1 : 0)
                        : event == FL_MOUSEWHEEL ? -Fl::event_dy() : 0;
      if (steps != 0) {
        change(static_cast<std::int64_t>(steps) * (Fl::event_shift() ? 10 : 1));
        return 1;
      }
    }
    return EditorField<Fl_Input>::handle(event);
  }

private:
  void change(const std::int64_t steps) {
    const bool duration = step_ == NumberStep::duration;
    const std::int64_t minimum = duration ? 1 : step_ == NumberStep::bar_width
        ? core::min_bar_width : core::min_bar_height;
    const std::int64_t maximum = duration ? core::max_cooldown_duration.count() / 100
        : step_ == NumberStep::bar_width ? core::max_bar_width : core::max_bar_height;
    const std::string_view text(value());
    double current{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), current);
    const auto units = parsed.ec == std::errc{} && std::isfinite(current)
        ? std::llround(std::clamp(current * (duration ? 10.0 : 1.0),
                                 static_cast<double>(minimum), static_cast<double>(maximum)))
        : minimum;
    const auto next = std::clamp(static_cast<std::int64_t>(units) + steps, minimum, maximum);
    const auto replacement = duration ? format_seconds(core::Milliseconds(next * 100))
                                      : std::to_string(next);
    if (value(replacement.c_str())) {
      app_.session.end_text_edit();
      do_callback();
      app_.session.end_text_edit();
    }
    insert_position(size(), 0);
  }

  EditorApp &app_;
  NumberStep step_;
};

void describe_application(Fl_Input &field, const core::GameMatch &game) {
  const auto tip = !game.executable_path.empty() ? "Matches exactly " + game.executable_path
      : !game.executable_name.empty() ? "Matches any " + game.executable_name + " regardless of folder"
      : "No application chosen yet";
  if (!field.tooltip() || tip != field.tooltip())
    accessibility::describe_with_tooltip(field, tip);
}

class DisabledReason final : public Fl_Box {
public:
  using Fl_Box::Fl_Box;
private:
  void draw() override {
    const auto ink = current_editor_tokens().subtext;
    labelcolor(fl_rgb_color(static_cast<uchar>(ink >> 16U),
                            static_cast<uchar>(ink >> 8U), static_cast<uchar>(ink)));
    Fl_Box::draw();
  }
};

} // namespace

Fl_Box *make_disabled_reason(const int x, const int y, const int width, const int height) {
  auto *reason = new DisabledReason(x, y, width, height);
  reason->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  reason->labelsize(FL_NORMAL_SIZE);
  reason->hide();
  return reason;
}

void add_choice(Fl_Choice &choice, const std::string &name) {
  // FLTK menu paths interpret '/', '&' and '\'. Authored names are text.
  const auto ordinal = choice.size();
  const auto index = choice.add(("__layerc_item_" + std::to_string(ordinal)).c_str());
  const auto label = "[" + std::to_string(index + 1) + "] " + name;
  choice.replace(index, label.c_str());
}

Fl_Input *FormBuilder::input(int x, int y, int width, const char *label, NumberStep step) {
  Fl_Input *field = windows_editor_keys && step != NumberStep::none
      ? static_cast<Fl_Input *>(new SteppedField(x, y, width, label, app_, step))
      : new EditorField<Fl_Input>(x, y, width, editor_input_height, label);
  field->align(FL_ALIGN_TOP_LEFT);
  field->when(FL_WHEN_CHANGED);
  field->callback([](Fl_Widget *w, void *data) {
    static_cast<EditorApp *>(data)->field_changed(w);
  }, &app_);
  return field;
}

Fl_Choice *FormBuilder::choice(int x, int y, int width, const char *label,
                               const char *items) {
  auto *field = new EditorChoice(x, y, width, editor_input_height, label);
  field->align(FL_ALIGN_TOP_LEFT);
  if (items) field->add(items);
  field->value(0);
  field->callback([](Fl_Widget *w, void *data) {
    static_cast<EditorApp *>(data)->field_changed(w);
  }, &app_);
  return field;
}

Fl_Check_Button *FormBuilder::check(int x, int y, int width, const char *label) {
  auto *field = new Fl_Check_Button(x, y, width, editor_input_height, label);
  field->callback([](Fl_Widget *w, void *data) {
    static_cast<EditorApp *>(data)->field_changed(w);
  }, &app_);
  return field;
}

ContextBrowser *FormBuilder::browser(int x, int y, int width, int height,
                                     const char *label, EditorMenuContext context) {
  auto *field = new ContextBrowser(x, y, width, height, label, app_, context);
  field->align(FL_ALIGN_TOP_LEFT);
  field->format_char(0);
  field->callback([](Fl_Widget *w, void *data) {
    static_cast<EditorApp *>(data)->selection_changed(w);
  }, &app_);
  return field;
}

Fl_Button *FormBuilder::button(int x, int y, int width, const char *label,
                               EditorCommand action) {
  return new CommandButton(x, y, width, label, app_, action);
}

void StatusPane::build_modules(FormBuilder &builder, const int x, const int y,
                               const int width, const int height) {
  (void)builder;
  module_mark_ = new EditorStatusMark(x, y, 18, height);
  module_status_ = new EditorField<Fl_Multiline_Output>(x + 18, y, width - 18, height,
                                                       "Preservation / module faults");
  module_status_->wrap(1);
  module_status_->align(FL_ALIGN_TOP_LEFT);
  module_status_->textfont(FL_COURIER);
  module_status_->textsize(FL_NORMAL_SIZE);
}

void StatusPane::build_footer(FormBuilder &builder) {
  (void)builder.button(15, 722, 100, "&Save", EditorCommand::save)
      ->shortcut(FL_CTRL | 's');
  undo_button_ = builder.button(125, 722, 80, "&Undo", EditorCommand::undo);
  undo_button_->shortcut(FL_CTRL | 'z');
  redo_button_ = builder.button(215, 722, 80, "&Redo", EditorCommand::redo);
  redo_button_->shortcut(FL_CTRL | FL_SHIFT | 'z');
  (void)builder.button(305, 722, 130, "LayerD status", EditorCommand::refresh);
  status_mark_ = new EditorStatusMark(270, 676, 18, 38);
  status_ = new EditorField<Fl_Multiline_Output>(288, 676, 757, 38, "Save / reload outcome");
  status_->wrap(1);
  status_->align(FL_ALIGN_TOP_LEFT);
  status_->textsize(FL_NORMAL_SIZE);
  daemon_mark_ = new EditorStatusMark(15, 572, 18, 142);
  daemon_status_ = new EditorField<Fl_Multiline_Output>(
      33, 572, 222, 142, "LayerD status and capability faults");
  daemon_status_->wrap(1);
  daemon_status_->align(FL_ALIGN_TOP_LEFT);
  daemon_status_->textsize(FL_NORMAL_SIZE);
  accessibility_status_ = new Fl_Box(450, 723, 595, 28);
  accessibility_status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  accessibility_status_->labelsize(FL_NORMAL_SIZE);
}

void StatusPane::set_status(const StatusKind kind, const std::string &text) {
  status_->value(text.c_str());
  status_mark_->kind(kind);
  status_->redraw();
}

void StatusPane::set_daemon(const StatusKind kind, const std::string &text) {
  daemon_status_->value(text.c_str());
  daemon_mark_->kind(kind);
}

void StatusPane::set_modules(const StatusKind kind, const std::string &text) {
  module_status_->value(text.c_str());
  module_mark_->kind(kind);
}

void StatusPane::set_accessibility(const std::string &text) {
  accessibility_status_->copy_label(text.c_str());
}

void StatusPane::set_history(const bool undo_active, const bool redo_active) {
  if (undo_active)
    undo_button_->activate();
  else
    undo_button_->deactivate();
  if (redo_active)
    redo_button_->activate();
  else
    redo_button_->deactivate();
}

std::string StatusPane::accessibility_text() const {
  return accessibility_status_->label() ? accessibility_status_->label() : "";
}

void ProfilePage::build(FormBuilder &builder) {
  profile_tab_ = new Fl_Group(270, 45, 775, 405, "Profile");
  profile_tab_->begin();
  name_ = builder.input(285, 80, 340, "Profile name");
  executable_ = builder.input(660, 80, 370, "Executable name");
  executable_path_ = builder.input(285, 145, windows_editor_keys ? 615 : 745,
                                   "Executable path (optional)");
  if constexpr (windows_editor_keys) {
    browse_ = new Fl_Button(915, 145, 115, editor_button_height, "&Browse...");
    browse_->labelsize(FL_NORMAL_SIZE);
    browse_->callback([](Fl_Widget *, void *data) {
      static_cast<ProfilePage *>(data)->browse_application();
    }, this);
    accessibility::describe_with_tooltip(*browse_, "Choose the game's .exe file");
  }
  describe_application(*executable_path_, {});
  window_class_ = builder.input(285, 210, 340, "Window class (optional)");
  window_title_ = builder.input(660, 210, 370, "Window title (optional)");
  enabled_ = builder.check(285, 265, 260, "Profile enabled");
  if constexpr (windows_editor_keys) {
    profile_id_ = builder.input(285, 320, 745, "Profile ID");
  } else {
    auto *id = new EditorField<Fl_Multiline_Output>(285, 320, 745, 30, "Profile ID (read-only)");
    id->wrap(1);
    id->align(FL_ALIGN_TOP_LEFT);
    profile_id_ = id;
  }
  auto *note = new Fl_Box(285, 370, 745, 65);
  const auto note_text = std::string(
      "Explicit Save publishes edits and requests an authenticated LayerD reload.\n"
      "Offline editing is supported. The Automation tab edits rules and replays events.\n") +
      visual_tab_note();
  note->copy_label(note_text.c_str());
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  profile_tab_->end();
}

void ProfilePage::show(const layerc::core::GameProfile &profile) {
  name_->value(profile.name.c_str());
  executable_->value(profile.game.executable_name.c_str());
  executable_path_->value(profile.game.executable_path.c_str());
  describe_application(*executable_path_, profile.game);
  window_class_->value(profile.game.window_class.c_str());
  window_title_->value(profile.game.window_title.c_str());
  enabled_->value(profile.enabled ? 1 : 0);
  profile_id_->value(profile.id.c_str());
}

void ProfilePage::show(const layerc::core::GameProfile *profile) {
  if (profile == nullptr) {
    profile_id_->value("");
    describe_application(*executable_path_, {});
    return;
  }
  show(*profile);
}

void ProfilePage::show_identity(const std::string &id) {
  profile_id_->value(id.c_str());
}

void ProfilePage::set_enabled_value(const bool enabled) {
  enabled_->value(enabled ? 1 : 0);
}

void ProfilePage::browse_application() {
  std::optional<std::string> chosen;
  if (choose_application_) {
    chosen = choose_application_();
  } else {
    Fl_Native_File_Chooser chooser(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.title("Choose the game application");
    chooser.filter("Applications\t*.exe");
    if (chooser.show() == 0 && chooser.filename() && *chooser.filename())
      chosen = chooser.filename();
  }
  if (!chosen) return;
  const auto path = platform::path_from_utf8(*chosen);
  if (!path) return;
  std::error_code file_error;
  if (!std::filesystem::is_regular_file(*path, file_error)) return;
  const auto name = platform::path_to_utf8(path->filename());
  if (!name || name->empty()) return;
  app_.session.end_text_edit();
  const auto error = app_.session.edit([&](auto &profile) {
    read(profile);
    profile.game.executable_name = *name;
    profile.game.global = false;
    if (!profile.game.executable_path.empty()) profile.game.executable_path = *chosen;
  });
  if (error) { app_.changed(error); return; }
  app_.refresh();
  app_.report(StatusKind::information, "Application set to " + *name + ".");
}

void ProfilePage::read(layerc::core::GameProfile &candidate) const {
  candidate.name = name_->value();
  if constexpr (windows_editor_keys) candidate.id = profile_id_->value();
  candidate.game.executable_name = executable_->value();
  candidate.game.executable_path = executable_path_->value();
  describe_application(*executable_path_, candidate.game);
  candidate.game.window_class = window_class_->value();
  candidate.game.window_title = window_title_->value();
  candidate.enabled = enabled_->value() != 0;
}

void ProfilePage::focus_name() {
  name_->take_focus();
  name_->insert_position(name_->size(), 0);
}

bool ProfilePage::owns(const Fl_Widget *widget) const {
  return widget != nullptr &&
         (widget == profile_tab_ || widget == name_ || widget == executable_ ||
          widget == executable_path_ || widget == window_class_ ||
          widget == window_title_ || widget == enabled_ || widget == profile_id_ ||
          widget == browse_);
}

void CooldownPage::build(FormBuilder &builder) {
  cooldown_tab_ = new Fl_Group(270, 45, 775, 405, "Cooldowns");
  cooldown_tab_->begin();
  cooldowns_ = builder.browser(285, 80, 230, 290, "Cooldowns", EditorMenuContext::cooldowns);
  (void)builder.button(285, 390, 110, "Add cooldown", EditorCommand::add_cooldown);
  remove_ = builder.button(405, 390, 110, "Remove", EditorCommand::delete_cooldown);
  remove_->deactivate();
  remove_reason_ = make_disabled_reason(cooldowns_->x(), remove_->y() + remove_->h(),
                                        cooldowns_->w(), editor_input_height);
  cooldown_fields_ = new Fl_Group(535, 65, 495, 325);
  cooldown_fields_->begin();
  cooldown_name_ = builder.input(545, 90, 470, "Cooldown name");
  duration_ = builder.input(545, 155, 220, "Duration (seconds)", NumberStep::duration);
  charges_ = builder.input(795, 155, 220, "Max charges");
  recovery_ = builder.choice(545, 225, 470, "Recovery mode", "Single|Sequential|Parallel");
  retrigger_ = builder.choice(545, 280, 470, "Retrigger policy", "Ignore|Restart");
  active_duration_ = builder.input(545, 350, 470, "Active duration (seconds; 0 = off)");
  cooldown_fields_->end();
  cooldown_tab_->end();
}

void CooldownPage::clear_list() { cooldowns_->clear(); }

void CooldownPage::fill_list(const layerc::core::GameProfile &profile,
                             const std::optional<std::size_t> selected) {
  cooldowns_->clear();
  for (const auto &item : profile.cooldowns) cooldowns_->add(item.name.c_str());
  if (selected) cooldowns_->value(static_cast<int>(*selected + 1));
  const bool last = selected && profile.cooldowns.size() == 1;
  const char *reason = last ? "A profile needs at least one cooldown." : "";
  if (selected && !last) remove_->activate(); else remove_->deactivate();
  accessibility::describe_with_tooltip(*remove_, reason);
  remove_reason_->label(reason);
  if (last) remove_reason_->show(); else remove_reason_->hide();
}

void CooldownPage::show(const layerc::core::GameProfile &profile,
                        const std::optional<std::size_t> index) {
  if (!index) {
    cooldown_fields_->deactivate();
    return;
  }
  cooldown_fields_->activate();
  const auto &item = profile.cooldowns[*index];
  cooldown_name_->value(item.name.c_str());
  duration_->value(format_seconds(item.duration).c_str());
  active_duration_->value(format_seconds(item.active_duration).c_str());
  charges_->value(std::to_string(item.max_charges).c_str());
  recovery_->value(static_cast<int>(item.recovery));
  retrigger_->value(static_cast<int>(item.retrigger));
}

std::optional<std::string>
CooldownPage::read(layerc::core::CooldownDefinition &item) const {
  const auto seconds = parse_duration_seconds(duration_->value());
  const auto count = parse_count(charges_->value(), layerc::core::min_cooldown_charges,
                                 layerc::core::max_cooldown_charges);
  if (!seconds)
    return "Duration must be a positive number of seconds up to " +
           std::to_string(layerc::core::max_cooldown_duration.count() / 1000) + ".";
  if (!count)
    return "Max charges must be between " +
           std::to_string(layerc::core::min_cooldown_charges) + " and " +
           std::to_string(layerc::core::max_cooldown_charges) + ".";
  const std::string_view active_text = active_duration_->value();
  const auto active = active_text.empty() || active_text == "0"
      ? std::optional<core::Milliseconds>(core::Milliseconds::zero())
      : parse_duration_seconds(active_text);
  if (!active) return "Active duration must be 0 (off) or a valid positive number of seconds.";
  item.active_duration = *active;
  item.name = cooldown_name_->value();
  item.duration = *seconds;
  item.max_charges = *count;
  item.recovery = static_cast<layerc::core::RecoveryMode>(recovery_->value());
  item.retrigger = static_cast<layerc::core::RetriggerPolicy>(retrigger_->value());
  return std::nullopt;
}

void CooldownPage::focus_name() {
  cooldown_name_->take_focus();
  cooldown_name_->insert_position(cooldown_name_->size(), 0);
}

bool CooldownPage::owns_list(const Fl_Widget *widget) const {
  return widget != nullptr && widget == cooldowns_;
}

bool CooldownPage::owns(const Fl_Widget *widget) const {
  return widget != nullptr &&
         (widget == cooldown_tab_ || widget == cooldown_fields_ || widget == cooldowns_ ||
          widget == cooldown_name_ || widget == duration_ || widget == active_duration_ ||
          widget == charges_ ||
          widget == recovery_ || widget == retrigger_);
}

void BindingPage::build(FormBuilder &builder) {
  binding_tab_ = new Fl_Group(270, 45, 775, 405, "Bindings");
  binding_tab_->begin();
  bindings_ = builder.browser(285, 80, 400, 240,
                              "Bindings (unsupported device tokens are retained)",
                              EditorMenuContext::bindings);
  (void)builder.button(285, 350, 160, "Remove selected", EditorCommand::delete_binding);
  binding_target_ = builder.choice(710, 80, 320, "Target cooldown", nullptr);
  key_code_ = builder.input(710, 145, 155, "Physical scan code");
  key_code_->value("18");
  (void)builder.button(880, 145, 150, "Add key", EditorCommand::add_keyboard);
  mouse_button_ = builder.choice(710, 210, 155, "Mouse input",
                                 "Left|Right|Middle|Back|Forward|Wheel up|Wheel down");
  (void)builder.button(880, 210, 150, "Add mouse", EditorCommand::add_mouse);
  auto *capture = builder.button(710, 275, 320, "Capture focused chord...",
                       EditorCommand::capture_binding);
  const bool hid = hid_binding_capture_available();
  if (hid)
    accessibility::describe_with_tooltip(
        *builder.button(710, 313, 320, "Capture HID binding...", EditorCommand::capture_hid_binding),
        "Bind a control on a generic HID device");
  auto *capture_note = new Fl_Box(710, hid ? 352 : 330, 320, hid ? 65 : 80);
  std::string capture_text = "Capture reads only events in its focused editor dialog.";
  if (!hid) capture_text += "\n" + std::string(focused_binding_hid_note());
  capture_note->copy_label(capture_text.c_str());
  capture_note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  capture_note->labelsize(11);
  // The tooltip and the note under the button are both its description
  // (DesignSystem.md, Screen readers).
  constexpr auto capture_tip = "Press the key or chord that starts this cooldown";
  capture->tooltip(capture_tip);
  std::string capture_description = std::string(capture_tip) + ". " + capture_text;
  std::ranges::replace(capture_description, '\n', ' ');
  accessibility::set_accessible_description(*capture, std::move(capture_description));
  binding_tab_->end();
}

void BindingPage::clear_list() { bindings_->clear(); }

void BindingPage::fill_list(const layerc::core::GameProfile &profile, const int selected) {
  bindings_->clear();
  for (const auto &item : profile.bindings) {
    const auto found = std::ranges::find_if(profile.cooldowns,
                                           [&](const auto &c) { return c.id == item.cooldown_id; });
    const auto label = (found == profile.cooldowns.end() ? item.cooldown_id : found->name) +
                       " — " + chord_label(item.chord, profile);
    bindings_->add(label.c_str());
  }
  bindings_->value(selected + 1);
}

void BindingPage::show(const layerc::core::GameProfile &profile) {
  const auto target = binding_target_->value();
  binding_target_->clear();
  for (const auto &item : profile.cooldowns) add_choice(*binding_target_, item.name);
  const auto count = static_cast<int>(profile.cooldowns.size());
  binding_target_->value(count ? std::clamp(target, 0, count - 1) : -1);
}

int BindingPage::target() const { return binding_target_->value(); }

std::string BindingPage::key_code() const { return key_code_->value(); }

int BindingPage::mouse() const { return mouse_button_->value(); }

bool BindingPage::owns_list(const Fl_Widget *widget) const {
  return widget != nullptr && widget == bindings_;
}

bool BindingPage::owns(const Fl_Widget *widget) const {
  return widget != nullptr &&
         (widget == binding_tab_ || widget == bindings_ || widget == binding_target_ ||
          widget == key_code_ || widget == mouse_button_);
}

void PanelPage::build(FormBuilder &builder) {
  panel_tab_ = new Fl_Group(270, 45, 775, 405, "Panels");
  panel_tab_->begin();
  panels_ = builder.browser(285, 80, 190, 230, "Panels", EditorMenuContext::panels);
  (void)builder.button(285, 325, 90, "Add panel", EditorCommand::add_panel);
  (void)builder.button(385, 325, 90, "Remove", EditorCommand::delete_panel);
  position_button_ =
      builder.button(285, 365, 190, "Position on window...", EditorCommand::position);
  accessibility::describe_with_tooltip(*position_button_,
      "Place the bars directly on the running game window");
  if (on_window_edit_sessions()) {
    // Its own callback: the choice is session state, not a profile field.
    direct_drag_ = new Fl_Check_Button(285, 400, 190, editor_input_height,
                                       "Direct drag in game");
    direct_drag_->callback([](Fl_Widget *w, void *data) {
      static_cast<EditorApp *>(data)->direct_drag_changed(
          static_cast<Fl_Check_Button *>(w)->value() != 0);
    }, &app_);
    accessibility::describe_with_tooltip(*direct_drag_,
        "While positioning, drag the bars on the game window itself");
    direct_drag_->deactivate();
  }
  panel_fields_ = new Fl_Group(490, 60, 550, 380);
  panel_fields_->begin();
  panel_name_ = builder.input(500, 85, 260, "Panel name");
  layout_ = builder.choice(780, 85, 120, "Layout", "Row|Column|Grid");
  style_ = builder.choice(915, 85, 115, "Style", "Card|Bar");
  anchor_x_ = builder.input(500, 145, 120, "X (0–1)");
  anchor_y_ = builder.input(635, 145, 125, "Y (0–1)");
  bar_width_ = builder.input(780, 145, 120, "Bar width", NumberStep::bar_width);
  bar_height_ = builder.input(915, 145, 115, "Bar height", NumberStep::bar_height);
  fill_ = builder.input(500, 205, 120, "Fill #RRGGBB");
  background_ = builder.input(635, 205, 125, "Background");
  border_ = builder.input(780, 205, 120, "Border");
  text_color_ = builder.input(915, 205, 115, "Text color");
  // DesignSystem.md, OverlayBar: one warning line under the colour fields
  // while a pair is below its Overlay minimum; hidden once every pair passes.
  contrast_mark_ = new EditorStatusMark(500, 237, 18, 36);
  contrast_line_ = new Fl_Box(518, 237, 512, 36);
  contrast_line_->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  contrast_mark_->hide();
  contrast_line_->hide();
  show_text_ = builder.check(500, 280, 180, "Show bar text");
  (void)builder.button(685, 275, 90, "Colours...", EditorCommand::panel_colors);
  hide_ready_ = builder.check(780, 280, 250, "Hide bars when ready");
  items_.build();
  panel_fields_->end();
  panel_tab_->end();
}

void PanelPage::clear_list() { panels_->clear(); }

void PanelPage::fill_list(const layerc::core::GameProfile &profile,
                          const std::optional<std::size_t> selected) {
  panels_->clear();
  for (const auto &item : profile.panels) panels_->add(item.name.c_str());
  if (selected) panels_->value(static_cast<int>(*selected + 1));
}

void PanelPage::show(const layerc::core::GameProfile &profile,
                     const std::optional<std::size_t> selected) {
  items_.show(profile, selected);
  if (!selected) {
    set_contrast({});
    panel_fields_->deactivate();
    return;
  }
  panel_fields_->activate();
  const auto &item = profile.panels[*selected];
  panel_name_->value(item.name.c_str());
  layout_->value(static_cast<int>(item.layout));
  style_->value(static_cast<int>(item.style));
  anchor_x_->value(format_decimal(item.anchor_x).c_str());
  anchor_y_->value(format_decimal(item.anchor_y).c_str());
  bar_width_->value(std::to_string(item.bar.width).c_str());
  bar_height_->value(std::to_string(item.bar.height).c_str());
  fill_->value(format_rgb(item.bar.fill_color).c_str());
  background_->value(format_rgb(item.bar.background_color).c_str());
  border_->value(format_rgb(item.bar.border_color).c_str());
  text_color_->value(format_rgb(item.bar.text_color).c_str());
  show_text_->value(item.bar.show_text ? 1 : 0);
  hide_ready_->value(item.bar.hide_when_ready ? 1 : 0);
  update_contrast();
}

// Read from the fields' own text so the line follows typing before the edit is
// accepted; while any colour field is unparsable the last line stays.
void PanelPage::update_contrast() {
  if (!panel_fields_->active()) return;
  const auto fill = parse_rgb(fill_->value());
  const auto background = parse_rgb(background_->value());
  const auto border = parse_rgb(border_->value());
  const auto text = parse_rgb(text_color_->value());
  if (!fill || !background || !border || !text) return;
  set_contrast(overlay_contrast_line(*fill, *background, *border, *text,
                                     show_text_->value() != 0));
}

void PanelPage::set_contrast(const std::string &line) {
  if (line == contrast_text_) return;
  contrast_text_ = line;
  contrast_line_->copy_label(line.c_str());
  contrast_mark_->kind(line.empty() ? StatusKind::none : StatusKind::warning);
  if (line.empty()) {
    contrast_mark_->hide();
    contrast_line_->hide();
  } else {
    contrast_mark_->show();
    contrast_line_->show();
  }
  // The four fields carry the line for screen readers, cleared with it.
  for (Fl_Input *field : {fill_, background_, border_, text_color_})
    accessibility::set_accessible_description(*field, line);
  panel_fields_->redraw();
}

std::optional<std::string> PanelPage::read(layerc::core::Panel &item) const {
  const auto x = parse_anchor(anchor_x_->value());
  const auto y = parse_anchor(anchor_y_->value());
  const auto width = parse_count(bar_width_->value(), layerc::core::min_bar_width,
                                 layerc::core::max_bar_width);
  const auto height = parse_count(bar_height_->value(), layerc::core::min_bar_height,
                                  layerc::core::max_bar_height);
  const auto fg = parse_rgb(fill_->value());
  const auto bg = parse_rgb(background_->value());
  const auto edge = parse_rgb(border_->value());
  const auto text = parse_rgb(text_color_->value());
  if (!x) return "Anchor X must be between 0 and 1.";
  if (!y) return "Anchor Y must be between 0 and 1.";
  if (!width)
    return "Bar width must be between " + std::to_string(layerc::core::min_bar_width) +
           " and " + std::to_string(layerc::core::max_bar_width) + ".";
  if (!height)
    return "Bar height must be between " + std::to_string(layerc::core::min_bar_height) +
           " and " + std::to_string(layerc::core::max_bar_height) + ".";
  if (!fg) return "Fill must be #RRGGBB.";
  if (!bg) return "Background must be #RRGGBB.";
  if (!edge) return "Border must be #RRGGBB.";
  if (!text) return "Text color must be #RRGGBB.";
  item.name = panel_name_->value();
  item.anchor_x = *x;
  item.anchor_y = *y;
  item.layout = static_cast<layerc::core::PanelLayout>(layout_->value());
  item.style = static_cast<layerc::core::PanelStyle>(style_->value());
  item.bar.width = *width;
  item.bar.height = *height;
  item.bar.fill_color = *fg;
  item.bar.background_color = *bg;
  item.bar.border_color = *edge;
  item.bar.text_color = *text;
  item.bar.show_text = show_text_->value() != 0;
  item.bar.hide_when_ready = hide_ready_->value() != 0;
  return std::nullopt;
}

bool PanelPage::owns_list(const Fl_Widget *widget) const {
  return widget != nullptr && widget == panels_;
}

bool PanelPage::owns(const Fl_Widget *widget) const {
  return widget != nullptr &&
         (widget == panel_tab_ || widget == panel_fields_ || widget == panels_ ||
          widget == panel_name_ || widget == anchor_x_ || widget == anchor_y_ ||
          widget == bar_width_ || widget == bar_height_ || widget == fill_ ||
          widget == background_ || widget == border_ || widget == text_color_ ||
          widget == layout_ || widget == style_ || items_.owns(widget) ||
          widget == show_text_ || widget == hide_ready_ || widget == position_button_ ||
          widget == contrast_mark_ || widget == contrast_line_ ||
          (direct_drag_ != nullptr && widget == direct_drag_));
}

void PanelPage::show_position(const bool active) {
  if (!position_button_) return;
  position_button_->copy_label(active ? "End positioning" : "Position on window...");
  position_button_->redraw();
}

void PanelPage::show_direct_drag(const bool available, const bool enabled) {
  if (!direct_drag_) return;
  direct_drag_->value(available && enabled ? 1 : 0);
  if (available) direct_drag_->activate();
  else direct_drag_->deactivate();
  direct_drag_->redraw();
}

bool PanelPage::direct_drag_enabled() const {
  return direct_drag_ && direct_drag_->active() && direct_drag_->value() != 0;
}

} // namespace layerc::editor
