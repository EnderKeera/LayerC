// Deterministic FLTK autosave tests: disposable stores and private sockets.
#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "linux_editor_test_access.hpp"
#include "test_support.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/json.hpp"
#include "layerc/platform/profile_store.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Window.H>
#include <chrono>
#include <filesystem>
#include <sys/stat.h>
namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using layerc::test::expect;
class Fixture {
public:
  Fixture() : temp_("layerc-autosave"),
      home_("HOME", temp_.path()), config_("XDG_CONFIG_HOME", temp_.path() / "config"),
      data_("XDG_DATA_HOME", temp_.path() / "data"),
      runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime"),
      bus_("DBUS_SESSION_BUS_ADDRESS", std::nullopt),
      accessibility_("AT_SPI_BUS_ADDRESS", std::nullopt),
      backend_("FLTK_BACKEND", std::filesystem::path("x11")),
      override_("LAYERC_PROFILE_DIR", std::nullopt) {
    layerc::test::require_x11();
    std::filesystem::create_directories(temp_.path() / "runtime");
    expect(::chmod((temp_.path() / "runtime").c_str(), 0700) == 0, "private control runtime");
    path = platform::profile_directory() / "fixture.json";
  }
  void store(bool legacy = false) {
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    auto bytes = core::serialize_profile_json(profile);
    if (legacy) {
      const auto tree = core::json::parse(bytes);
      auto object = std::get<core::json::Value>(tree).as_object();
      object["schemaVersion"] = core::json::Value(std::int64_t{3});
      object.erase("cooldownGroups"); // this field is legal only from schema 4
      bytes.clear();
      core::json::append_value(bytes, core::json::Value(std::move(object)), 2);
      const auto parsed = core::parse_profile_json(bytes);
      const auto *error = std::get_if<core::ProfileError>(&parsed);
      expect(!error, error ? error->describe() : "legacy fixture is valid");
    }
    expect(platform::write_profile_file(path, bytes).ok(), "store a disposable fixture");
  }
  std::filesystem::path path;
private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, bus_, accessibility_, backend_, override_;
};
void wait(double seconds) {
  const auto until = core::MonotonicClock::now() + std::chrono::duration_cast<core::MonotonicClock::duration>(
      std::chrono::duration<double>(seconds));
  while (core::MonotonicClock::now() < until) Fl::wait(.01);
}
void show(editor::EditorApp &app) { app.build(); editor::EditorAppTestAccess::window(app).show(); Fl::check(); }
void name(editor::EditorApp &app, const char *value) { editor::EditorAppTestAccess::name(app)->value(value); app.field_changed(editor::EditorAppTestAccess::name(app)); }
std::string disk_name(const std::filesystem::path &path) {
  const auto bytes = layerc::test::read_file(path);
  if (!bytes) return {};
  const auto result = core::parse_profile_json(*bytes);
  const auto *profile = std::get_if<core::GameProfile>(&result);
  return profile ? profile->name : std::string{};
}
void debounce_undo_and_offline_save() {
  Fixture fixture; fixture.store(); editor::EditorApp app; show(app);
  const auto original = disk_name(fixture.path);
  name(app, "First"); wait(.20);
  expect(disk_name(fixture.path) == original && app.session.has_unsaved_changes(), "debounce retains partial text");
  name(app, "Second"); wait(.20);
  expect(disk_name(fixture.path) == original, "typing restarts debounce");
  wait(.35);
  expect(disk_name(fixture.path) == "Second" && !app.session.has_unsaved_changes() &&
             std::string(editor::EditorAppTestAccess::status(app)->value()).starts_with("Autosaved") &&
             std::string(editor::EditorAppTestAccess::status(app)->value()).find("LayerD is not running") != std::string::npos,
         "text autosaves durably with visible offline-daemon status");
  app.command(editor::EditorCommand::undo); wait(.08);
  expect(disk_name(fixture.path) == original && !app.session.has_unsaved_changes(), "Undo immediately autosaves coalesced history");
  app.command(editor::EditorCommand::redo); wait(.08);
  expect(disk_name(fixture.path) == "Second", "Redo autosaves the restored entry");
  editor::EditorAppTestAccess::window(app).hide();
}
void draft_first_save_then_autosave() {
  Fixture fixture; editor::EditorApp app; show(app);
  app.command(editor::EditorCommand::new_profile);
  editor::EditorAppTestAccess::executable(app)->value("draft.exe"); app.field_changed(editor::EditorAppTestAccess::executable(app));
  name(app, "Unsaved new profile"); wait(.55);
  expect(app.session.is_draft() && app.session.profiles().profiles.empty(), "typing never creates a new profile");
  expect(app.save(), "first explicit Save creates draft");
  const auto path = app.session.document().path();
  name(app, "Created profile edit"); wait(.55);
  expect(!app.session.is_draft() && disk_name(path) == "Created profile edit", "existing profile then autosaves");
  editor::EditorAppTestAccess::window(app).hide();
}
void invalid_buffer_then_recovery() {
  Fixture fixture; fixture.store(); editor::EditorApp app; show(app);
  const auto before = layerc::test::read_file(fixture.path);
  name(app, "Accepted"); name(app, ""); wait(.55);
  expect(layerc::test::read_file(fixture.path) == before && app.session.has_unsaved_changes() &&
             std::string(editor::EditorAppTestAccess::name(app)->value()).empty() && std::string(editor::EditorAppTestAccess::status(app)->value()).starts_with("Not autosaved"),
         "invalid buffer blocks publication without losing it or earlier accepted edits");
  name(app, "Recovered"); wait(.55);
  expect(disk_name(fixture.path) == "Recovered" && !app.session.has_unsaved_changes(), "corrected buffer retries autosave");
  editor::EditorAppTestAccess::window(app).hide();
}
void write_failure_then_recovery() {
  Fixture fixture; fixture.store(); editor::EditorApp app; show(app);
  const auto before = layerc::test::read_file(fixture.path);
  const auto blocked = platform::recovery_backup_path(fixture.path);
  std::filesystem::remove(blocked); std::filesystem::create_directory(blocked);
  name(app, "Cannot write"); wait(.55);
  expect(layerc::test::read_file(fixture.path) == before && app.session.has_unsaved_changes() &&
             std::string(editor::EditorAppTestAccess::status(app)->value()).starts_with("Not autosaved"),
         "planning failure retains draft and original durable bytes");
  wait(.20); expect(layerc::test::read_file(fixture.path) == before, "failed save has no retry spin");
  std::filesystem::remove(blocked);
  name(app, "Write recovered"); wait(.55);
  expect(disk_name(fixture.path) == "Write recovered" && !app.session.has_unsaved_changes(), "next edit retries after fault removal");
  editor::EditorAppTestAccess::window(app).hide();
}
void upgrade_never_prompts_automatically() {
  Fixture fixture; fixture.store(true); editor::EditorApp app; show(app);
  const auto before = layerc::test::read_file(fixture.path);
  expect(app.session.document().legacy_base_schema(), "legacy fixture is recognized");
  name(app, "Legacy edit"); wait(.55);
  expect(!Fl::modal() && layerc::test::read_file(fixture.path) == before && app.session.has_unsaved_changes() &&
             std::string(editor::EditorAppTestAccess::status(app)->value()).starts_with("Autosave paused"),
         "autosave never prompts or silently upgrades");
  expect(app.session.save([] { return true; }).saved(), "explicit consent upgrades");
  app.refresh(); name(app, "After upgrade"); wait(.55);
  expect(disk_name(fixture.path) == "After upgrade" && !app.session.has_unsaved_changes(), "autosave resumes after upgrade");
  editor::EditorAppTestAccess::window(app).hide();
}
void modal_suspends_commit_and_close_flushes() {
  Fixture fixture; fixture.store(); editor::EditorApp app; show(app);
  const auto before = layerc::test::read_file(fixture.path);
  name(app, "Inside modal");
  Fl_Window modal(120, 80); modal.end(); modal.set_modal(); modal.show(); wait(.55);
  expect(layerc::test::read_file(fixture.path) == before && app.session.has_unsaved_changes(), "no autosave underneath modal");
  modal.hide(); wait(.20);
  expect(disk_name(fixture.path) == "Inside modal", "autosave resumes after modal close");
  name(app, "Close flush"); app.command(editor::EditorCommand::close);
  expect(!editor::EditorAppTestAccess::window(app).shown() && disk_name(fixture.path) == "Close flush" && !app.session.has_unsaved_changes(),
         "close flushes accepted edits and stops owned services");
  const auto closed = layerc::test::read_file(fixture.path); wait(.55);
  expect(layerc::test::read_file(fixture.path) == closed, "no orphaned timer writes after close");
}
} // namespace


// A refused manual Save reports one prefix, keeps the buffer, history and the
// durable bytes, and does not change its text between repeated attempts.
void refused_save_and_presentation_only_report() {
  Fixture fixture; fixture.store(); editor::EditorApp app; show(app);
  const auto durable = layerc::test::read_file(fixture.path);
  name(app, "Accepted rename"); wait(.20);
  expect(app.session.can_undo(), "the accepted rename is one history step");
  const auto steps = app.session.edit_generation();
  name(app, "");
  const auto first = app.save();
  const std::string first_text = editor::EditorAppTestAccess::status(app)->value();
  expect(!first && first_text.starts_with("Not saved. Invalid input: ") &&
             first_text.find("Not saved") == first_text.rfind("Not saved"),
         "a refused Save reports one prefix and the field error: " + first_text);
  const auto second = app.save();
  const std::string second_text = editor::EditorAppTestAccess::status(app)->value();
  expect(!second && second_text == first_text && std::string(editor::EditorAppTestAccess::name(app)->value()).empty(),
         "a repeated refused Save keeps the invalid buffer and the same text");
  expect(app.session.edit_generation() == steps &&
             layerc::test::read_file(fixture.path) == durable,
         "a refused Save changes neither history nor durable bytes");
  editor::EditorAppTestAccess::window(app).hide();

  Fixture report_fixture; report_fixture.store();
  editor::EditorApp quiet; show(quiet);
  const auto quiet_durable = layerc::test::read_file(report_fixture.path);
  const auto sequence = editor::EditorAppTestAccess::live_sequence(quiet);
  // Presentation is never an autosave signal: with the listener cleared the
  // session edit cannot schedule, and report() must not schedule either.
  quiet.session.set_change_listener({});
  expect(!quiet.session.edit([](auto &profile) { profile.name = "Listenerless"; }) &&
             quiet.session.has_unsaved_changes(),
         "an accepted edit without the listener still dirties the document");
  expect(!editor::EditorAppTestAccess::autosave_pending(quiet),
         "a session edit cannot schedule autosave once the listener is cleared");
  quiet.report(editor::StatusKind::information, "x");
  quiet.report(editor::StatusKind::information, "x");
  auto refused = quiet.session.export_json(
      report_fixture.path.parent_path() / "saved-active.json",
      quiet.session.profiles().profiles.size());
  quiet.report(refused.kind, refused.text);
  expect(!refused.ok && editor::EditorAppTestAccess::status_kind(quiet) ==
                           editor::StatusKind::failure &&
             std::string(editor::EditorAppTestAccess::status(quiet)->value()) == refused.text,
         "a saved-profile refusal reaches the status renderer as failure");
  refused.text = "Profile saved. /tmp/loaded/active.json";
  quiet.report(refused.kind, refused.text);
  expect(editor::EditorAppTestAccess::status_kind(quiet) == editor::StatusKind::failure,
         "rewording the refusal cannot turn its displayed glyph into success");
  expect(!editor::EditorAppTestAccess::autosave_pending(quiet) &&
             editor::EditorAppTestAccess::live_sequence(quiet) == sequence &&
             layerc::test::read_file(report_fixture.path) == quiet_durable,
         "report() is presentational: no autosave timeout, no live publish, no write");
  editor::EditorAppTestAccess::window(quiet).hide();

  Fixture listener_fixture; listener_fixture.store();
  editor::EditorApp app_with_listener; show(app_with_listener);
  expect(!app_with_listener.session.edit(
             [](auto &profile) { profile.name = "Listener schedules"; }) &&
             editor::EditorAppTestAccess::autosave_pending(app_with_listener),
         "the production listener schedules autosave for an accepted edit");
  editor::EditorAppTestAccess::window(app_with_listener).hide();
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(debounce_undo_and_offline_save),
                 LAYERC_TEST_CASE(draft_first_save_then_autosave),
                 LAYERC_TEST_CASE(invalid_buffer_then_recovery),
                 LAYERC_TEST_CASE(write_failure_then_recovery),
                 LAYERC_TEST_CASE(upgrade_never_prompts_automatically),
                 LAYERC_TEST_CASE(modal_suspends_commit_and_close_flushes),
                 LAYERC_TEST_CASE(refused_save_and_presentation_only_report))
