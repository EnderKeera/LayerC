#include "kit/metrics.hpp"
#include "test_support.hpp"

#include <array>

namespace {
namespace kit = layerc::editor::kit;
using kit::Metric;
using layerc::test::expect;
}

void fltk_kit_metric_scaling() {
  constexpr std::array bodies{11, 12, 13, 14, 16};
  struct Row { Metric metric; std::array<int, bodies.size()> expected; };
  constexpr std::array rows{
      Row{Metric::control_height, {26, 26, 28, 30, 35}},
      Row{Metric::combo_height, {23, 23, 25, 27, 31}},
      Row{Metric::row_pitch, {34, 34, 37, 40, 45}},
      Row{Metric::list_row, {27, 27, 29, 32, 36}},
      Row{Metric::outline, {1, 1, 1, 1, 1}},
      Row{Metric::focus_width, {2, 2, 2, 2, 3}},
      Row{Metric::accent_rail, {3, 3, 3, 4, 4}},
      Row{Metric::gap_dialog, {8, 8, 9, 9, 11}},
  };
  for (const auto &row : rows) {
    for (std::size_t i = 0; i < bodies.size(); ++i) {
      expect(kit::metric_at(row.metric, bodies[i]) == row.expected[i],
             "kit uses the body floor, token scaling and half-away-from-zero rounding");
    }
  }
  expect(kit::scaled_at(480, 11) == 480 && kit::scaled_at(480, 14) == 560 &&
             kit::scaled_at(110, 14) == 128, "dialog dimensions share the kit body and its floor");
  expect(kit::scaled_at(6, 13) == 7 && kit::scaled_at(-6, 13) == -7 &&
             kit::scaled(110) == kit::scaled_at(110, kit::body()),
         "dialog dimensions round half away from zero and use the current body");
}

void fltk_kit_metrics_change_notifications() {
  const int old_body = kit::body();
  const int first = old_body == 14 ? 12 : 14;
  const int second = first == 14 ? 12 : 14;
  int first_calls = 0, second_calls = 0;
  const auto a = kit::subscribe_metrics_changed([&] {
    ++first_calls;
    expect(kit::metric(Metric::control_height) ==
               kit::metric_at(Metric::control_height, kit::body()),
           "subscribers see the updated body and metric");
  });
  const auto b = kit::subscribe_metrics_changed([&] { ++second_calls; });
  kit::set_body(old_body);
  expect(first_calls == 0 && second_calls == 0, "an equal initial body does not notify");
  kit::set_body(first);
  kit::set_body(first);
  expect(kit::body() == first && first_calls == 1 && second_calls == 1,
         "both subscribers receive exactly one notification for one change");
  kit::unsubscribe_metrics_changed(a);
  kit::set_body(second);
  expect(kit::body() == second && first_calls == 1 && second_calls == 2,
         "an unsubscribed callback is not notified on the next change");
  kit::unsubscribe_metrics_changed(b);
  kit::set_body(old_body);
  expect(second_calls == 2, "the last unsubscribed callback is no longer notified");
}

#ifdef LAYERC_KIT_WIDGET_TESTS
#include "appearance_window.hpp"
#include "binding_capture.hpp"
#include "cooldown_groups_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_commands.hpp"
#include "editor_file_chooser.hpp"
#include "editor_test_environment.hpp"
#include "editor_tools.hpp"
#include "export_selection_window.hpp"
#include "hid_binding_dialog.hpp"
#include "layerc/platform/profile_store.hpp"
#include "linux_editor_test_access.hpp"
#include "options_window.hpp"
#include "panel_item_dialog.hpp"
#include "platform/services.hpp"
#include "profile_edit.hpp"
#ifdef _WIN32
#include "platform/win32/window_chrome.hpp"
#include "platform/win32/window_settings.hpp"
#else
#include "calibration_window.hpp"
#endif

#include <FL/Fl_Spinner.H>
#include <FL/Fl_Value_Input.H>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using Access = editor::EditorAppTestAccess;

struct Edges {
  int left, top, right, bottom;
};

Edges edges(const Fl_Widget &widget) {
  const auto *window = dynamic_cast<const Fl_Window *>(&widget);
  const int x = window ? 0 : widget.x();
  const int y = window ? 0 : widget.y();
  return {x, y, x + widget.w(), y + widget.h()};
}

bool contains(Edges parent, Edges child) {
  return parent.left <= child.left && parent.top <= child.top &&
         child.right <= parent.right && child.bottom <= parent.bottom;
}

bool intersects(Edges a, Edges b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

std::string name(const Fl_Widget &widget) {
  return widget.label() ? widget.label() :
      "(" + std::to_string(widget.x()) + ", " + std::to_string(widget.y()) + ")";
}

std::optional<Metric> control_metric(const Fl_Widget &widget) {
  if (dynamic_cast<const Fl_Choice *>(&widget)) return Metric::combo_height;
  if (const auto *input = dynamic_cast<const Fl_Input_ *>(&widget);
      input && input->input_type() != FL_MULTILINE_INPUT)
    return Metric::control_height;
  if (dynamic_cast<const Fl_Button *>(&widget) || dynamic_cast<const Fl_Spinner *>(&widget) ||
      dynamic_cast<const Fl_Value_Input *>(&widget))
    return Metric::control_height;
  return std::nullopt;
}

bool is_control(const Fl_Widget &widget) {
  return control_metric(widget).has_value() || dynamic_cast<const Fl_Input_ *>(&widget);
}

int inspect(Fl_Group &parent, const std::string &surface) {
  int count = 0;
  for (int i = 0; i < parent.children(); ++i) {
    const auto &child = *parent.child(i);
    if (!child.visible_r()) continue;
    const auto metric = control_metric(child);
    if (is_control(child)) {
      const auto here = surface + ": " + name(child);
      if (metric) {
        ++count;
        const int height = kit::metric(*metric);
        expect(child.h() == height, here + ": height " + std::to_string(child.h()) +
            ", expected " + std::to_string(height));
      }
      // A scroll viewport clips its content; the content can extend past it.
      if (!dynamic_cast<const Fl_Scroll *>(&parent))
        expect(contains(edges(parent), edges(child)), here + ": stays inside its parent");
      for (int j = i + 1; j < parent.children(); ++j) {
        const auto &sibling = *parent.child(j);
        if (sibling.visible_r() && is_control(sibling))
          expect(!intersects(edges(child), edges(sibling)), here + ": clears sibling " + name(sibling));
      }
    }
    if (auto *group = dynamic_cast<Fl_Group *>(parent.child(i));
        group && !metric)
      count += inspect(*group, surface);
  }
  return count;
}

void inspect_surface(Fl_Window &window, const std::string &surface) {
  expect(window.shown(), surface + ": real FLTK window is shown");
  expect(inspect(window, surface) != 0, surface + ": exercises visible single-line controls");
}

void show_and_inspect(Fl_Window &window) {
  window.show();
  Fl::check();
  inspect_surface(window, name(window));
  window.hide();
}

class ModalProbe {
public:
  ModalProbe(std::string title, std::function<void(Fl_Window &)> inspect_window)
      : title_(std::move(title)), inspect_(std::move(inspect_window)), prior_(Fl::modal()) {
    Fl::add_timeout(.01, inspect_modal, this);
  }
  ~ModalProbe() {
    Fl::remove_timeout(inspect_modal, this);
    expect(delivered_, title_ + ": expected modal opened and was inspected");
  }
  ModalProbe(const ModalProbe &) = delete;
  ModalProbe &operator=(const ModalProbe &) = delete;

private:
  static void inspect_modal(void *data) {
    auto &self = *static_cast<ModalProbe *>(data);
    auto *window = Fl::modal();
    if (window && window != self.prior_ && window->shown() && name(*window) == self.title_) {
      self.delivered_ = true;
      self.inspect_(*window);
      window->do_callback();
    } else if (++self.attempts_ < 100) {
      Fl::add_timeout(.01, inspect_modal, data);
    } else if (window && window != self.prior_) {
      window->hide();
    }
  }

  std::string title_;
  std::function<void(Fl_Window &)> inspect_;
  Fl_Window *prior_;
  int attempts_{};
  bool delivered_{};
};

template <typename Open>
void inspect_modal(std::string title, Open open) {
  ModalProbe probe(title, [&](Fl_Window &window) { inspect_surface(window, title); });
  open();
}

core::GameProfile write_fixture() {
  auto profile = editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  auto cooldown = profile.cooldowns.front();
  cooldown.id = "second";
  cooldown.name = "Second";
  profile.cooldowns.push_back(std::move(cooldown));
  const auto file = platform::profile_directory() / "fixture.json";
  expect(platform::write_profile_file(file, core::serialize_profile_json(profile)).ok(),
         "save only the private metric test fixture");
  return profile;
}

class HidFixture final : public editor::HidBindingCapture {
public:
  std::vector<std::string> scan(bool) override { return {"Fixture HID controller"}; }
  std::optional<std::string> start(std::size_t, Update) override { return "Not used"; }
  void stop() noexcept override {}
  std::optional<std::string> assign_slot(const core::HidDeviceSelector &, bool) override {
    return "Not used";
  }
};

} // namespace

void fltk_kit_main_control_geometry() {
  layerc::test::EditorTestEnvironment fixture("layerc-kit-main", layerc::test::EditorTestScope::widget);
  (void)write_fixture();
  editor::install_editor_style();
  editor::EditorApp app;
  app.build();
  app.show();
  Fl::check();
  expect(kit::body() == FL_NORMAL_SIZE, "desktop font installation sets the kit body");
  auto *tabs = Access::tabs(app);
  if (!expect(tabs != nullptr && tabs->children() >= 6, "metric fixture owns the editor pages")) return;
  for (int i = 0; i < tabs->children(); ++i) {
    if (auto *page = dynamic_cast<Fl_Group *>(tabs->child(i))) {
      tabs->value(page);
      app.tab_changed();
      Fl::check();
      expect(page->visible_r(), "the page under inspection is visible");
      inspect_surface(*app.main_window(), name(*page));
    }
  }
  app.main_window()->hide();
}

void fltk_kit_dialog_control_geometry() {
  layerc::test::EditorTestEnvironment fixture("layerc-kit-dialog", layerc::test::EditorTestScope::widget);
  const auto profile = write_fixture();
  editor::install_editor_style();
  editor::EditorApp app;
  app.build();
  app.show();
  Fl::check();
  {
    editor::OptionsWindow options({}, app.appearance(), 640,
                                  [](const auto &) { return std::nullopt; }, {}, {});
    show_and_inspect(options);
    editor::CooldownGroupsDialog groups(app.session);
    show_and_inspect(groups);
    editor::ExportSelectionDialog selection(app.session.profiles().profiles, 0);
    show_and_inspect(selection);
    editor::PanelItemDialog item(profile, std::nullopt, std::nullopt,
                                 [](const auto &) { return std::nullopt; });
    show_and_inspect(item);
  }
  inspect_modal("Appearance — LayerE", [&] { app.command(editor::EditorCommand::appearance); });
  inspect_modal("Capture focused binding", [] { (void)editor::capture_focused_binding(); });
  inspect_modal("Fixture color", [&] { (void)editor::choose_color("Fixture color", 0x334455); });
  inspect_modal("Panel colors — LayerE", [&] { (void)editor::edit_panel_colors({}); });
  inspect_modal("Keyboard shortcuts — LayerE", [] { editor::show_shortcuts_dialog("Ctrl+P\tProfile"); });
  inspect_modal("About LayerE", [] { editor::show_about_dialog("Offline fixture", "Test fixture"); });
  for (const auto &[command, title] : {
      std::pair{editor::EditorCommand::diagnostics, "Runtime diagnostics — LayerE"},
      std::pair{editor::EditorCommand::tasks, "Detected Tasks — LayerE"}}) {
    app.command(command);
    Fl::check();
    auto *window = Fl::first_window();
    while (window && name(*window) != title) window = Fl::next_window(window);
    if (expect(window && window != app.main_window(),
               std::string(title) + ": modeless command opens its own window")) {
      inspect_surface(*window, title);
      window->hide();
    }
  }
  HidFixture hid;
  inspect_modal("Capture HID binding", [&] {
    (void)editor::capture_hid_binding(hid, app.appearance(),
                                     [](const auto &, bool) { return std::nullopt; });
  });
#ifdef _WIN32
  {
    editor::WindowsWindowDecorator decorator;
    struct ResetPrompt {
      ~ResetPrompt() {
        editor::set_file_prompt_observer_for_test({});
        editor::set_native_file_presenter_for_test({});
        editor::set_editor_capture_exclusion(false);
      }
    } reset;
    bool inspected = false;
    editor::set_editor_capture_exclusion(true);
    editor::set_native_file_presenter_for_test([](const auto &, const auto &) {
      expect(false, "the typed file prompt does not reach the native chooser on cancel");
      return std::nullopt;
    });
    editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
      if (stage != editor::FilePromptStage::shown_and_verified) return;
      inspect_surface(view.window, "Fixture file prompt");
      inspected = true;
      view.cancel.do_callback();
    });
    expect(!editor::choose_editor_file({app, editor::EditorFileRequest::Mode::open,
        "Fixture file prompt", "Import", "JSON", {"json"}}) && inspected,
        "the private file prompt was inspected and cancelled");
  }
#else
  auto seed = editor::CalibrationModel::open(profile, profile.cooldowns.front().id, std::nullopt);
  if (expect(seed.has_value(), "the calibration fixture has a selected cooldown")) {
    inspect_modal("LayerE — X11 visual calibration", [&] {
      (void)editor::run_x11_calibration({}, std::move(*seed), [](const auto &) { return std::string("Not used"); });
    });
  }
#endif
  app.main_window()->hide();
}
#endif
