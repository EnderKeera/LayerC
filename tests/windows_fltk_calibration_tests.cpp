#include "calibration_run_internal.hpp"
#include "editor_session.hpp"
#include "platform/win32/calibration_service_internal.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/platform/window_query.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

namespace core = layerc::core;
namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;
using layerc::test::ScopedEnvironment;
using layerc::test::TempDirectory;

constexpr auto fake_mode_key = "LAYERC_TEST_FAKE_CALIBRATOR";
constexpr auto timer_event_key = "LAYERC_TEST_CALIBRATION_TIMER";
constexpr auto launch_event_key = "LAYERC_TEST_CALIBRATION_LAUNCH";

std::wstring environment(const wchar_t *name) {
  const DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
  if (capacity == 0) return {};
  std::wstring value(capacity, L'\0');
  const auto count = GetEnvironmentVariableW(name, value.data(), capacity);
  if (count == 0 || count >= capacity) return {};
  value.resize(count);
  return value;
}

std::filesystem::path fake_image() {
  return platform::sibling_image_path(L"layerc_windows_fltk_calibration_tests.exe");
}

std::optional<HANDLE> pipe_argument(const std::string_view value) {
  std::uintptr_t handle{};
  const auto end = value.data() + value.size();
  const auto parsed = std::from_chars(value.data(), end, handle);
  if (parsed.ec != std::errc{} || parsed.ptr != end || handle == 0) return std::nullopt;
  return reinterpret_cast<HANDLE>(handle);
}

core::VisualProfile calibration_visual(const core::GameProfile &profile,
                                       const std::string &cooldown) {
  core::VisualProfile visual;
  visual.profile_id = profile.id;
  core::PixelRule rule;
  rule.id = "ready";
  rule.name = "Ready";
  visual.observations.emplace_back(std::move(rule));
  visual.triggers.push_back({.cooldown_id = cooldown, .observation_id = "ready"});
  return visual;
}

int fake_calibrator(HANDLE request, HANDLE response) {
  const platform::OwnedHandle read(request), write(response);
  if (const auto name = environment(L"LAYERC_TEST_CALIBRATION_LAUNCH"); !name.empty()) {
    const platform::OwnedHandle launched(OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
    if (!launched.valid() || SetEvent(launched.get()) == FALSE) return 2;
  }
  const auto mode = environment(L"LAYERC_TEST_FAKE_CALIBRATOR");
  if (mode == L"hang") {
    Sleep(INFINITE);
    return 2;
  }
  const auto framed = platform::read_framed_pipe(read.get(), core::max_calibration_packet_bytes);
  if (!framed) return 2;
  const auto decoded = core::decode_calibration_request(framed.payload);
  const auto *input = std::get_if<core::CalibrationRequest>(&decoded);
  if (!input || IsWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(input->target_window))) == FALSE)
    return 2;
  const auto parsed = core::parse_profile_json(input->profile_json);
  const auto *profile = std::get_if<core::GameProfile>(&parsed);
  if (!profile) return 2;

  core::CalibrationResponse result;
  if (mode == L"applied") {
    if (const auto name = environment(L"LAYERC_TEST_CALIBRATION_TIMER"); !name.empty()) {
      const platform::OwnedHandle timer(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
      if (!timer.valid() || WaitForSingleObject(timer.get(), 4'000) != WAIT_OBJECT_0) return 2;
    }
    result.applied = true;
    result.visual_json = core::serialize_visual_profile_json(
        calibration_visual(*profile, input->cooldown_id));
  } else if (mode == L"invalid") {
    result.applied = true;
    result.visual_json = "{}";
  } else if (mode != L"cancelled") {
    return 2;
  }
  std::string error;
  return platform::write_framed_pipe(write.get(), core::encode_calibration_response(result),
                                    core::max_calibration_packet_bytes, error) ? 0 : 2;
}

class ChildEvent {
public:
  ChildEvent(const wchar_t *suffix, const char *key)
      : name_(L"Local\\LayerC.FltkCalibrationTests." + std::to_wstring(GetCurrentProcessId()) + suffix),
        handle_(CreateEventW(nullptr, TRUE, FALSE, name_.c_str())),
        setting_(key, std::filesystem::path(name_)) {}

  [[nodiscard]] bool valid() const { return handle_.valid() && setting_.applied(); }
  [[nodiscard]] HANDLE get() const { return handle_.get(); }

private:
  std::wstring name_;
  platform::OwnedHandle handle_;
  ScopedEnvironment setting_;
};

struct TimerProbe {
  HWND owner{};
  HANDLE ready{};
  unsigned ticks{};
  bool owner_disabled{};
};

void probe_owner(void *value) {
  auto &probe = *static_cast<TimerProbe *>(value);
  ++probe.ticks;
  probe.owner_disabled = IsWindow(probe.owner) != FALSE && IsWindowEnabled(probe.owner) == FALSE;
  (void)SetEvent(probe.ready);
}

void hide_owner(void *value) { static_cast<Fl_Window *>(value)->hide(); }

core::GameProfile fixture_profile() {
  auto profile = editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  return profile;
}

core::CalibrationRequest request_for(HWND window, const core::GameProfile &profile) {
  return {.target_window = reinterpret_cast<std::uintptr_t>(window),
          .cooldown_id = profile.cooldowns.front().id,
          .profile_json = core::serialize_profile_json(profile), .visual_json = {}};
}

void fltk_pump_runs_timeouts_and_restores_owner() {
  const ScopedEnvironment mode(fake_mode_key, std::filesystem::path(L"applied"));
  const ScopedEnvironment no_launch_probe(launch_event_key, std::nullopt);
  const ChildEvent ready(L".timer", timer_event_key);
  if (!expect(mode.applied() && no_launch_probe.applied() && ready.valid(),
              "set up the fake helper and the timer rendezvous")) return;
  Fl_Window owner(240, 100);
  owner.end();
  owner.show();
  const HWND window = fl_xid(&owner);
  if (!expect(window && IsWindowEnabled(window) != FALSE, "the owner starts enabled")) return;

  const auto profile = fixture_profile();
  const auto request = request_for(window, profile);
  TimerProbe probe{.owner = window, .ready = ready.get()};
  Fl::add_timeout(0.01, probe_owner, &probe);
  const auto result = editor::detail::run_calibration_session_with_image(
      window, request, fake_image(), editor::detail::fltk_calibration_pump(owner));
  Fl::remove_timeout(probe_owner, &probe);
  expect(probe.ticks > 0 && probe.owner_disabled, "an FLTK timeout runs while the owner is disabled");
  expect(IsWindowEnabled(window) != FALSE, "the owner is re-enabled after calibration");
  const auto *response = std::get_if<core::CalibrationResponse>(&result);
  if (!expect(response && response->applied && response->error.empty(),
              "the fake helper replies with an applied result")) return;
  const auto parsed = core::parse_visual_profile_json(response->visual_json, profile);
  const auto *visual = std::get_if<core::VisualProfile>(&parsed);
  expect(visual && *visual == calibration_visual(profile, request.cooldown_id),
         "the applied visual result round-trips through the private channel");
}

void hiding_owner_terminates_helper_without_quit() {
  const ScopedEnvironment mode(fake_mode_key, std::filesystem::path(L"hang"));
  const ScopedEnvironment no_launch_probe(launch_event_key, std::nullopt);
  if (!expect(mode.applied() && no_launch_probe.applied(), "set up the hanging fake helper")) return;
  Fl_Window owner(240, 100);
  owner.end();
  owner.show();
  const HWND window = fl_xid(&owner);
  if (!expect(window != nullptr, "the disposable owner exists")) return;

  platform::OwnedHandle child;
  bool wait_started = false;
  const auto pump = editor::detail::fltk_calibration_pump(owner);
  const auto start = std::chrono::steady_clock::now();
  const auto result = editor::detail::run_calibration_session_with_image(
      window, request_for(window, fixture_profile()), fake_image(),
      [&](HANDLE process, DWORD milliseconds) {
        if (!wait_started) {
          wait_started = true;
          const HANDLE self = GetCurrentProcess();
          HANDLE duplicate{};
          if (DuplicateHandle(self, process, self, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            child.reset(duplicate);
          Fl::add_timeout(0.01, hide_owner, &owner);
        }
        return pump(process, milliseconds);
      });
  Fl::remove_timeout(hide_owner, &owner);
  const auto *failure = std::get_if<editor::CalibrationFailure>(&result);
  expect(!owner.shown() && failure && failure->kind == editor::CalibrationFailureKind::cancelled,
         "hiding the FLTK owner produces explicit session cancellation");
  expect(std::chrono::steady_clock::now() - start < std::chrono::seconds(4),
         "owner cancellation terminates and joins the helper promptly");
  MSG quit{};
  expect(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) == FALSE,
         "owner cancellation does not post WM_QUIT");
  DWORD exit_code{};
  expect(child.valid() && WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0 &&
             GetExitCodeProcess(child.get(), &exit_code) != FALSE && exit_code == ERROR_CANCELLED,
         "the helper has exited with the cancellation code before return");
}

void failure_text_matches_win32() {
  using Kind = editor::CalibrationFailureKind;
  constexpr std::string_view detail = "The original detail.";
  const struct {
    Kind kind;
    std::string_view title;
    std::string_view message;
  } cases[] = {
      {Kind::missing_image, "Calibration unavailable", "layercal.exe is missing beside LayerC Editor."},
      {Kind::channel, "Calibration unavailable", "Could not create the private calibration channel or restrict its handle inheritance."},
      {Kind::launch, "Calibration unavailable", "Could not start layercal.exe inside its bounded Job."},
      {Kind::monitor, "Calibration unavailable", "Could not monitor the calibration response."},
      {Kind::request_timeout, "Calibration channel timed out", "LayerC Calibration did not begin reading its bounded request within five seconds and was stopped. The editor remained responsive and no calibration changes were applied."},
      {Kind::session_timeout, "Calibration timed out", "LayerC Calibration exceeded the 30-minute safety limit and was stopped. No calibration changes were applied."},
      {Kind::write, "Calibration channel failed", detail},
      {Kind::read, "Calibration did not return a result", detail},
      {Kind::invalid_response, "Invalid calibration response", detail},
      {Kind::failed_response, "Calibration failed", detail},
  };
  for (const auto &item : cases) {
    const auto text = editor::calibration_failure_text({item.kind, std::string(detail)});
    expect(text && text->title == item.title && text->message == item.message, item.title);
  }
  expect(!editor::calibration_failure_text({Kind::cancelled, std::string(detail)}),
         "a cancelled session has no failure dialog text");
}

class SettingsFixture {
public:
  SettingsFixture()
      : key_(L"Software\\LayerC.FltkCalibrationTests\\" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64())),
        settings_("LAYERC_SETTINGS_KEY", std::filesystem::path(key_)) {}
  ~SettingsFixture() { (void)RegDeleteTreeW(HKEY_CURRENT_USER, key_.c_str()); }

  [[nodiscard]] bool isolated() const {
    return settings_.applied() && environment(L"LAYERC_SETTINGS_KEY") == key_;
  }

private:
  std::wstring key_;
  ScopedEnvironment settings_;
};

void platform_service_accepts_and_reports() {
  const SettingsFixture settings;
  if (!expect(settings.isolated(), "the settings override is isolated")) return;
  const auto notice = platform::write_bool_setting(platform::BoolSetting::visual_capture_notice, true);
  if (!expect(static_cast<bool>(notice), "acknowledge capture in the disposable settings key")) return;
  const TempDirectory temp("windows-fltk-calibration");
  const ScopedEnvironment store("LAYERC_PROFILE_DIR", temp.path());
  const ScopedEnvironment no_timer_probe(timer_event_key, std::nullopt);
  const ChildEvent launched(L".launched", launch_event_key);
  if (!expect(store.applied() && no_timer_probe.applied() && launched.valid(),
              "set up the disposable profile store and helper probe")) return;
  const auto profile = fixture_profile();
  const auto path = temp.path() / (profile.id + ".json");
  if (!expect(platform::write_profile_file(path, core::serialize_profile_json(profile)).ok(),
              "write the disposable profile")) return;
  editor::EditorSession session(temp.path());
  if (!expect(!session.open_path(path), "open the disposable profile")) return;
  const auto &cooldown = profile.cooldowns.front().id;

  Fl_Window target(240, 100);
  target.end();
  target.show();
  Fl_Window owner(240, 100);
  owner.end();
  owner.show();
  const HWND window = fl_xid(&target);
  if (!expect(window != nullptr && fl_xid(&owner) != nullptr, "show the test target and owner")) return;

  editor::TaskCandidate candidate;
  candidate.window = reinterpret_cast<std::uintptr_t>(window);
  std::optional<editor::TaskCandidate> resolved = candidate;
  std::optional<std::pair<editor::StatusKind, std::string>> report;
  const editor::CalibrationHost host{
      .report = [&](editor::StatusKind status, const std::string &text) { report.emplace(status, text); },
      .resolve_target = [&] { return resolved; },
      .owner = reinterpret_cast<std::uintptr_t>(&owner),
  };
  unsigned accept_calls{};
  const editor::CalibrationAccept accept = [&](const editor::CalibrationDraft &draft) {
    ++accept_calls;
    expect(draft.images.empty(), "external results never carry owned asset bytes");
    return session.prepare_calibration(cooldown, draft.visual, draft.images,
                                       [] { return editor::SharedObservationChoice::cancel; });
  };
  const auto run = [&](const wchar_t *mode) {
    const ScopedEnvironment fake(fake_mode_key, std::filesystem::path(mode));
    expect(fake.applied(), "set the fake helper response mode");
    report.reset();
    (void)ResetEvent(launched.get());
    return editor::detail::run_platform_calibration_with_image(host, session.document(), cooldown,
                                                               accept, fake_image());
  };
  const auto reported = [](const editor::CalibrationRunResult &result) {
    const auto *end = std::get_if<editor::CalibrationEnd>(&result);
    return end && *end == editor::CalibrationEnd::reported;
  };

  auto applied = run(L"applied");
  auto *accepted = std::get_if<editor::AcceptedCalibration>(&applied);
  if (!expect(accepted && !report && WaitForSingleObject(launched.get(), 0) == WAIT_OBJECT_0,
              "the service launches the helper and returns a typed acceptance")) return;
  expect(!session.apply_calibration(std::move(*accepted)) && session.document().visual() &&
             session.document().visual()->triggers.size() == 1 &&
             session.document().visual()->triggers.front().cooldown_id == cooldown,
         "the editor session accepts and installs the requested cooldown assignment");

  auto cancelled = run(L"cancelled");
  expect(reported(cancelled) && report && report->first == editor::StatusKind::information &&
             report->second == "Visual calibration cancelled; no changes were made.",
         "the helper's unapplied result is reported as cancellation information");
  auto invalid = run(L"invalid");
  expect(reported(invalid) && report && report->first == editor::StatusKind::failure &&
             report->second == "LayerC Calibration returned an invalid result. No profile data was changed.",
         "a protocol-valid but invalid visual result is reported as a failure");

  resolved.reset();
  auto no_target = run(L"applied");
  expect(reported(no_target) && !report && WaitForSingleObject(launched.get(), 0) == WAIT_TIMEOUT,
         "a resolver that already reported no match does not start the fake helper");
  expect(accept_calls == 1, "only the applied valid result reaches acceptance");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 1) {
    if (argc != 5 || std::string_view(argv[1]) != "--request-pipe" ||
        std::string_view(argv[3]) != "--response-pipe") return 2;
    const auto request = pipe_argument(argv[2]);
    const auto response = pipe_argument(argv[4]);
    return request && response ? fake_calibrator(*request, *response) : 2;
  }
  return layerc::test::run({
      LAYERC_TEST_CASE(fltk_pump_runs_timeouts_and_restores_owner),
      LAYERC_TEST_CASE(hiding_owner_terminates_helper_without_quit),
      LAYERC_TEST_CASE(failure_text_matches_win32),
      LAYERC_TEST_CASE(platform_service_accepts_and_reports),
  });
}
