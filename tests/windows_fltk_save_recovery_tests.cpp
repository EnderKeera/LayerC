#include "editor_app.hpp"
#include "editor_test_environment.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "linux_editor_test_access.hpp"
#include "platform/win32/startup.hpp"
#include "profile_edit.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>

namespace {
namespace editor = layerc::editor;
namespace platform = layerc::platform;
namespace test = layerc::test;
using Access = editor::EditorAppTestAccess;
using test::expect;

auto seed_profile() {
  auto profile = editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  return layerc::core::serialize_profile_json(profile);
}
} // namespace

void windows_locked_target_save_and_retry() {
  test::EditorTestEnvironment environment("layerc-save-retry");
  const auto path = platform::profile_directory() / "fixture.json";
  const auto before = seed_profile();
  if (!expect(platform::write_profile_file(path, before).ok(), "seed the private profile")) return;

  editor::EditorApp app;
  app.build();
  auto &window = Access::window(app);
  window.show();
  Fl::check();
  if (!expect(app.session.has_document(), "open the disposable profile")) return;

  auto *name = Access::name(app);
  name->value("Retry after refusal");
  app.field_changed(name);
  {
    platform::OwnedHandle lock(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!expect(lock.valid(), "open the target without allowing replacement")) return;
    expect(!app.save() && app.session.document().dirty() &&
               test::read_file(path) == before &&
               std::string_view(Access::status(app)->value()) ==
                   "Not saved. Profile write failed: profile store I/O failed.",
           "locked Save retains the exact status, dirty document and original bytes");
  }
  expect(app.save() && !app.session.has_unsaved_changes() &&
             test::read_file(path).value_or("").find("Retry after refusal") != std::string::npos &&
             std::string_view(Access::status(app)->value()) ==
                 "Saved. LayerD is not running; the change applies when it starts.",
         "retry writes the profile and reports the isolated daemon absent");
  window.hide();
}

void windows_interrupted_import_recovery() {
  test::EditorTestEnvironment environment("layerc-import-recovery");
  const auto directory = platform::profile_directory();
  const auto path = directory / "fixture.json";
  const auto before = seed_profile();
  if (!expect(platform::write_profile_file(path, before).ok(), "seed the private profile")) return;

  const auto stage = directory / "_transactions" / "interrupted-import";
  if (!expect(test::write_file(stage / "backup-0", before), "simulate interruption before journal publication"))
    return;
  expect(!editor::recover_startup_profiles(directory) && !std::filesystem::exists(stage) &&
             test::read_file(path) == before,
         "startup removes the unpublished import without changing the saved profile");

  const auto bad = directory / "_transactions" / "not-a-transaction";
  if (!expect(test::write_file(bad, "preserve"), "seed a blocked transaction directory")) return;
  expect(editor::recover_startup_profiles(directory) ==
             "LayerE could not recover an interrupted profile import (profile recovery is blocked). "
             "Resolve the _transactions directory before editing profiles." &&
             test::read_file(bad) == "preserve" && test::read_file(path) == before,
         "startup reports the exact recovery failure and preserves the evidence");
}
