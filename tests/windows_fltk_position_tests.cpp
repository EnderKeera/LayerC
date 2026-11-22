#include "linux_editor_test_access.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>
#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <windows.h>

// Link the frontend against a deliberately disconnected status/reload surface;
// the edit fake below and the private-pipe hub suite exercise its two clients.
namespace layerc::editor {
std::string DaemonStatus::summary() const { return "LayerD is not running"; }
StatusKind DaemonStatus::kind() const noexcept { return StatusKind::failure; }
bool DaemonStatus::wayland_session() const noexcept { return false; }
bool wayland_environment() { return false; }
std::string wayland_scope_note(const DaemonStatus &, bool) { return {}; }
DaemonStatus query_daemon_status() {
  DaemonStatus status;
  status.socket_missing = true;
  return status;
}
ReloadResult request_daemon_reload(const std::optional<CommittedProfile> &) { return {}; }
void shutdown_daemon_link() noexcept {}
} // namespace layerc::editor

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using Access = editor::EditorAppTestAccess;
using Event = editor::OnWindowEditEvent;
using layerc::test::expect;

struct AnchorSend {
  std::string panel_id;
  double x{}, y{};
  bool final{};
  bool operator==(const AnchorSend &) const = default;
};

struct EditCalls {
  std::uint64_t target{};
  editor::OnWindowEditBegin begin;
  std::vector<std::string> profiles;
  std::vector<AnchorSend> anchors;
  std::vector<bool> direct_drag;
  std::vector<bool> end;
  std::vector<Event> events;

  void deliver(editor::EditorApp &app, Event event) {
    events.push_back(std::move(event));
    Access::on_window_ready(app);
  }
};

class FakeEdit final : public editor::OnWindowEdit {
public:
  explicit FakeEdit(EditCalls &calls) : calls_(calls) {}
  void begin(std::uint64_t target, editor::OnWindowEditBegin payload) override {
    calls_.target = target;
    calls_.begin = std::move(payload);
  }
  void live_profile(std::string json) override { calls_.profiles.push_back(std::move(json)); }
  void anchor(std::string panel, double x, double y, bool final) override {
    calls_.anchors.push_back({std::move(panel), x, y, final});
  }
  void direct_drag(bool value) override { calls_.direct_drag.push_back(value); }
  void end(bool notify) override { calls_.end.push_back(notify); }
  std::vector<Event> take_events() override { return std::exchange(calls_.events, {}); }

private:
  EditCalls &calls_;
};

class Fixture {
public:
  ~Fixture() {
    app_.reset();
    const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, key_.c_str());
    expect(removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND, "remove fixture key");
  }

  bool build() {
    if (!expect(settings_.applied() && profiles_.applied(), "isolate profile store and settings"))
      return false;
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    profile.id = "position-first";
    profile.name = "A Position fixture";
    if (!expect(layerc::test::write_file(first(), core::serialize_profile_json(profile)),
                "write the current-schema profile fixture")) return false;
    profile.id = "position-next";
    profile.name = "B Other fixture";
    if (!expect(layerc::test::write_file(directory_.path() / "next.json",
                                        core::serialize_profile_json(profile)),
                "write the switch-profile fixture")) return false;
    editor::install_editor_style();
    app_ = std::make_unique<editor::EditorApp>();
    app_->build();
    Access::set_on_window_edit(*app_, std::make_unique<FakeEdit>(edit));
    return expect(app_->session.has_document() && app_->session.document().path() == first(),
                  "open the disposable initial profile");
  }

  void start() { Access::position_on(app(), task); }
  void started() { edit.deliver(app(), {.kind = Event::Kind::started, .panel_id = {}, .text = {}}); }
  std::filesystem::path first() const { return directory_.path() / "first.json"; }
  editor::EditorApp &app() const { return *app_; }
  const core::Panel &panel() const { return app().session.document().profile().panels.front(); }

  const editor::TaskCandidate task{.window = 0x1234, .title = "Position fixture", .class_name = {},
                                   .executable_name = "fixture.exe", .executable_path = {},
                                   .pid = {}, .width = 800, .height = 600};
  EditCalls edit;

private:
  static inline unsigned sequence_{};
  const std::wstring key_ = L"Software\\LayerC.FltkPositionTests." +
      std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence_);
  const layerc::test::TempDirectory directory_{"LayerC-FltkPositionTests"};
  const layerc::test::ScopedEnvironment settings_{"LAYERC_SETTINGS_KEY", std::filesystem::path(key_)};
  const layerc::test::ScopedEnvironment profiles_{"LAYERC_PROFILE_DIR", directory_.path()};
  std::unique_ptr<editor::EditorApp> app_;
};

int preview_event(editor::PanelPreview &preview, const int event, const int x, const int y) {
  const int previous_x = Fl::e_x, previous_y = Fl::e_y, previous_key = Fl::e_keysym;
  Fl::e_x = x;
  Fl::e_y = y;
  Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
  const int handled = preview.handle(event);
  Fl::e_x = previous_x;
  Fl::e_y = previous_y;
  Fl::e_keysym = previous_key;
  return handled;
}

void begin_toggle_and_preview_drag() {
  Fixture fixture;
  if (!fixture.build()) return;
  auto &app = fixture.app();
  auto *const drag = Access::direct_drag(app);
  if (!expect(Access::position_button(app) && drag && !drag->active() && !drag->value(), "Direct Drag starts disabled and off"))
    return;
  fixture.start();
  const auto &begin = fixture.edit.begin;
  expect(app.placement_active() && fixture.edit.target == fixture.task.window &&
             begin.profile_key == platform::canonical_profile_key(fixture.first()) &&
             begin.panel_id == app.selected_panel() && begin.panel_id == fixture.panel().id &&
             begin.profile_json == core::serialize_profile_json(app.session.document().profile()),
         "Position on window begins with the task, canonical key, selected panel and JSON");
  fixture.edit.deliver(app, {.kind = Event::Kind::awaiting, .panel_id = {}, .text = {}});
  expect(!drag->active() && !drag->value(), "Direct Drag is inactive while awaiting");
  fixture.started();
  expect(drag->active() && !drag->value(), "started enables Direct Drag but leaves it off");
  Access::acknowledge_direct_drag_notice(app);
  drag->value(1);
  drag->do_callback();
  drag->value(0);
  drag->do_callback();
  expect(fixture.edit.direct_drag == std::vector<bool>{true, false}, "Direct Drag sends both toggle values");
  drag->value(1);
  drag->do_callback();

  auto &preview = *Access::preview(app);
  const auto view = Access::preview_panel(app, fixture.panel());
  const auto frame = editor::placement_frame(preview.x() + 5, preview.y() + 5,
      preview.w() - 10, preview.h() - 10, fixture.task.width, fixture.task.height);
  const auto geometry = core::panel_geometry(view.panel, view.presentation.items.size());
  const auto origin = editor::placed_origin(frame, view.panel.anchor_x, view.panel.anchor_y,
      static_cast<int>(geometry.size.width * frame.scale),
      static_cast<int>(geometry.size.height * frame.scale));
  const int x = frame.x + frame.width / 3, y = frame.y + frame.height / 2;
  const auto moved = editor::anchor_at(frame, x, y);
  const auto panel_id = fixture.panel().id;
  if (!expect(preview_event(preview, FL_PUSH, origin.x + 1, origin.y + 1) &&
                  preview_event(preview, FL_DRAG, x, y) && preview_event(preview, FL_RELEASE, x, y),
              "the preview handles the real drag callbacks")) return;
  expect(fixture.edit.anchors == std::vector<AnchorSend>{{panel_id, moved.x, moved.y, false},
                                                       {panel_id, moved.x, moved.y, true}},
         "drag sends a non-final anchor then a final anchor at release");
  expect(fixture.edit.profiles.size() == 1 && fixture.edit.profiles.back() ==
             core::serialize_profile_json(app.session.document().profile()),
         "the accepted drag also publishes the live profile");
  Access::position_button(app)->do_callback();
  expect(!app.placement_active() && !drag->active() && !drag->value() &&
             fixture.edit.end == std::vector<bool>{true},
         "manual end notifies LayerD and turns off and disables Direct Drag");
}

void incoming_anchor_and_accepted_edit() {
  Fixture fixture;
  if (!fixture.build()) return;
  auto &app = fixture.app();
  fixture.start();
  fixture.started();
  const auto original = fixture.panel();
  const auto generation = app.session.edit_generation();
  fixture.edit.deliver(app, {.kind = Event::Kind::anchor, .panel_id = original.id,
                            .x = .2, .y = .3, .text = {}});
  const auto preview = Access::preview_panel(app, fixture.panel());
  expect(preview.panel.anchor_x == .2 && preview.panel.anchor_y == .3 &&
             fixture.panel().anchor_x == original.anchor_x && fixture.panel().anchor_y == original.anchor_y &&
             app.session.edit_generation() == generation && !app.session.can_undo() &&
             fixture.edit.anchors.empty() && fixture.edit.profiles.empty(),
         "non-final anchor only changes the painted preview");
  fixture.edit.deliver(app, {.kind = Event::Kind::anchor, .panel_id = original.id,
                            .x = .4, .y = .6, .final = true, .text = {}});
  expect(fixture.panel().anchor_x == .4 && fixture.panel().anchor_y == .6 &&
             app.session.edit_generation() == generation + 1 && fixture.edit.anchors.empty(),
         "final anchor commits once without echoing an anchor");
  app.command(editor::EditorCommand::undo);
  expect(!app.session.can_undo() && app.session.can_redo() &&
             fixture.panel().anchor_x == original.anchor_x && fixture.panel().anchor_y == original.anchor_y &&
             Access::preview_panel(app, fixture.panel()).panel.anchor_x == original.anchor_x,
         "one undo restores the document and preview with no stale temporary anchor");
  Access::tabs(app)->value(Access::panel_tab(app));
  app.tab_changed();
  fixture.edit.profiles.clear();
  auto *const name = Access::panel_name(app);
  name->value("Renamed panel");
  name->do_callback();
  expect(fixture.panel().name == "Renamed panel" && fixture.edit.profiles.size() == 1 &&
             fixture.edit.profiles.back() == core::serialize_profile_json(app.session.document().profile()),
         "accepted field edit publishes the latest live JSON");
}

void end_reasons_and_expiry() {
  Fixture fixture;
  if (!fixture.build()) return;
  struct EndCase { Event event; std::string_view text; editor::StatusKind status; };
  using Reason = Event::EndReason;
  using Kind = editor::StatusKind;
  const auto ended = [](Reason reason, std::string text = {}) -> Event {
    return {.reason = reason, .panel_id = {}, .text = std::move(text)};
  };
  const std::array<EndCase, 7> cases{{
      {ended(Reason::connection_lost), "Live editor connection lost; normal overlays restored.", Kind::warning},
      {ended(Reason::daemon_error, "rejected"), "LayerD ended editing: rejected", Kind::failure},
      {ended(Reason::daemon_ended), "LayerD ended the on-window edit session.", Kind::information},
      {ended(Reason::daemon_ended, "finished"), "LayerD ended editing: finished", Kind::information},
      {ended(Reason::target_closed), "Edit target closed or minimized.", Kind::information},
      {ended(Reason::unavailable, "missing pipe"), "On-window editing is unavailable: missing pipe", Kind::failure},
      {{.kind = Event::Kind::expired, .panel_id = {}, .text = {}},
          "Live editing is unavailable: LayerD did not expose the live-edit "
          "service. Exit the older LayerD from its tray icon, then try Position on window again.", Kind::failure},
  }};
  for (const auto &[event, text, status] : cases) {
    fixture.start();
    if (event.kind != Event::Kind::expired) fixture.started();
    auto *const drag = Access::direct_drag(fixture.app());
    if (drag->active()) drag->value(1);
    fixture.edit.deliver(fixture.app(), event);
    expect(!fixture.app().placement_active() && !drag->active() && !drag->value() &&
               std::string_view(Access::status(fixture.app())->value()) == text &&
               Access::status_kind(fixture.app()) == status && !fixture.edit.end.empty() &&
               !fixture.edit.end.back(), text);
  }
  expect(fixture.edit.end == std::vector<bool>(cases.size(), false),
         "peer-ended and expired sessions are detached once without notifying LayerD");
}

void ended_during_direct_drag_confirmation() {
  Fixture fixture;
  if (!fixture.build()) return;
  auto &app = fixture.app();
  fixture.start();
  fixture.started();
  auto *drag = Access::direct_drag(app);
  if (!expect(drag && drag->active(), "Direct drag is available before the prompt")) return;
  Access::set_direct_drag_confirmation(app, [&] {
    fixture.edit.deliver(app, {.kind = Event::Kind::ended,
                               .reason = Event::EndReason::connection_lost,
                               .panel_id = {}, .text = {}});
    return 1;
  });
  drag->value(1);
  drag->do_callback();
  expect(!app.placement_active() && !drag->active() && !drag->value() &&
             fixture.edit.direct_drag.empty(),
         "Continue cannot enable direct drag in an ended session");
}

void closing_main_window_closes_tool_windows_and_edit() {
  Fixture fixture;
  if (!fixture.build()) return;
  auto &app = fixture.app();
  app.show();
  fixture.start();
  fixture.started();
  app.command(editor::EditorCommand::diagnostics);
  app.command(editor::EditorCommand::tasks);
  auto *diagnostics = Access::diagnostics(app);
  auto *tasks = Access::tasks_window(app);
  if (!expect(diagnostics && diagnostics->shown() && tasks && tasks->shown(),
              "both tool windows are open")) return;
  app.main_window()->do_callback();
  expect(!diagnostics->shown() && !tasks->shown() && !app.shown() &&
             !Fl::first_window() && fixture.edit.end == std::vector<bool>{true},
         "closing the main window closes both tools and notifies LayerD once");
}

void switching_or_closing_profile_ends_edit() {
  Fixture fixture;
  if (!fixture.build()) return;
  auto &app = fixture.app();
  fixture.start();
  fixture.started();
  auto *const profiles = Access::profiles(app);
  profiles->value(2);
  app.selection_changed(profiles);
  expect(!app.placement_active() && app.session.document().profile().id == "position-next" &&
             fixture.edit.end == std::vector<bool>{true}, "switching profiles notifies LayerD");
  fixture.start();
  fixture.started();
  app.session.close_document();
  app.refresh();
  expect(!app.placement_active() && fixture.edit.end == std::vector<bool>{true, true},
         "closing the profile notifies LayerD");
}
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(begin_toggle_and_preview_drag),
    LAYERC_TEST_CASE(incoming_anchor_and_accepted_edit),
    LAYERC_TEST_CASE(end_reasons_and_expiry),
    LAYERC_TEST_CASE(ended_during_direct_drag_confirmation),
    LAYERC_TEST_CASE(closing_main_window_closes_tool_windows_and_edit),
    LAYERC_TEST_CASE(switching_or_closing_profile_ends_edit))
