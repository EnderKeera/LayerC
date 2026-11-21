#include "calibration_run_internal.hpp"
#include "calibration_session.hpp"
#include "editor_util.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/window_query.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

#include <windows.h>

namespace {
using layerc::test::expect;

struct OwnerState {
  int quit_code{};
  bool destroy_on_timer{};
  bool destroyed{};
  unsigned enabled_notifications{};
  unsigned activations{};
};

LRESULT CALLBACK owner_proc(const HWND window, const UINT message,
                            const WPARAM wparam, const LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto *state = reinterpret_cast<OwnerState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state != nullptr) {
    if (message == WM_ENABLE && wparam != FALSE)
      ++state->enabled_notifications;
    if (message == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE)
      ++state->activations;
    if (message == WM_TIMER) {
      KillTimer(window, 1);
      if (state->destroy_on_timer)
        DestroyWindow(window);
      else
        PostQuitMessage(state->quit_code);
      return 0;
    }
    if (message == WM_DESTROY) {
      state->destroyed = true;
      if (state->destroy_on_timer)
        PostQuitMessage(state->quit_code);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

class OwnerFixture {
public:
  OwnerFixture(OwnerState &state, const bool enabled) {
    constexpr auto name = L"LayerC.CalibrationShutdownFixture";
    static const ATOM registered = [] {
      WNDCLASSW window_class{};
      window_class.lpfnWndProc = owner_proc;
      window_class.hInstance = GetModuleHandleW(nullptr);
      window_class.lpszClassName = L"LayerC.CalibrationShutdownFixture";
      return RegisterClassW(&window_class);
    }();
    if (registered == 0)
      return;
    window_ = CreateWindowExW(0, name, L"Disposable calibration owner",
                             WS_POPUP | (enabled ? 0U : WS_DISABLED),
                             0, 0, 100, 100, nullptr, nullptr,
                             GetModuleHandleW(nullptr), &state);
  }
  ~OwnerFixture() {
    if (IsWindow(window_) != FALSE) {
      KillTimer(window_, 1);
      DestroyWindow(window_);
    }
  }
  OwnerFixture(const OwnerFixture &) = delete;
  OwnerFixture &operator=(const OwnerFixture &) = delete;
  HWND get() const { return window_; }

private:
  HWND window_{};
};

void exercise_quit(const bool initially_enabled, const bool destroy_owner,
                   const int quit_code) {
  OwnerState state{.quit_code = quit_code, .destroy_on_timer = destroy_owner};
  OwnerFixture owner(state, initially_enabled);
  if (!expect(owner.get() != nullptr, "the disposable owner must exist"))
    return;
  const auto enabled_before = state.enabled_notifications;
  const auto activations_before = state.activations;

  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  // Nested-document canonicalization discards padding. Populate real valid
  // cooldown data so the encoded frame still exceeds the pipe buffer.
  const auto seed = profile.cooldowns.front();
  for (unsigned index{}; index < 100U; ++index) {
    auto definition = seed;
    definition.id = "pipe-padding-" + std::to_string(index);
    definition.name = std::string(48U, 'p');
    profile.cooldowns.push_back(std::move(definition));
  }
  layerc::core::CalibrationRequest request{
      .target_window = reinterpret_cast<std::uintptr_t>(owner.get()),
      .cooldown_id = profile.cooldowns.front().id,
      // The fixture never reads: shutdown must unblock owned synchronous I/O,
      // not leave a detached worker or merely close its handles.
      .profile_json = layerc::core::serialize_profile_json(profile),
      .visual_json = {},
  };
  const auto parsed = layerc::core::parse_profile_json(request.profile_json);
  if (!expect(std::get_if<layerc::core::ProfileError>(&parsed) == nullptr,
              "the padded disposable base profile must remain parser-valid"))
    return;
  const auto decoded = layerc::core::decode_calibration_request(
      layerc::core::encode_calibration_request(request));
  if (!expect(std::get_if<layerc::core::CalibrationRequest>(&decoded) != nullptr,
              "the disposable calibration request must be protocol-valid"))
    return;
  if (!expect(SetTimer(owner.get(), 1, 30, nullptr) != 0,
              "the owner must post quit while the nested wait is active"))
    return;

  const auto start = std::chrono::steady_clock::now();
  const auto result = layerc::editor::detail::run_calibration_session_with_image(
      owner.get(), request,
      layerc::platform::sibling_image_path(L"layerc_editor_tests.exe"));
  expect(std::chrono::steady_clock::now() - start < std::chrono::seconds(4),
         "quit must promptly terminate the Job and join blocked I/O");
  const auto *failure = std::get_if<layerc::editor::CalibrationFailure>(&result);
  const bool cancelled = failure != nullptr &&
      failure->kind == layerc::editor::CalibrationFailureKind::cancelled;
  expect(cancelled, "thread quit must be an explicit calibration cancellation");
  if (cancelled)
    layerc::editor::show_calibration_failure(owner.get(), *failure);

  MSG quit{};
  const bool found = PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
  expect(found && static_cast<int>(quit.wParam) == quit_code,
         "the outer loop must receive the original quit code after I/O joins");
  expect(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) == FALSE,
         "the session must not post duplicate quit messages");
  expect(state.activations == activations_before,
         "a quitting session must never reactivate its owner");
  if (destroy_owner) {
    expect(state.destroyed && IsWindow(owner.get()) == FALSE,
           "owner destruction must be dispatched before quit is preserved");
    expect(state.enabled_notifications == enabled_before,
           "a destroyed owner must never be reenabled");
  } else {
    expect(IsWindow(owner.get()) != FALSE &&
               (IsWindowEnabled(owner.get()) != FALSE) == initially_enabled,
           "the surviving owner must retain its original enabled state");
    expect(state.enabled_notifications == enabled_before +
               (initially_enabled ? 1U : 0U),
           "restoration must not enable an owner that was already disabled");
  }
}

} // namespace

void editor_calibration_shutdown_contracts() {
  exercise_quit(true, false, 37);
}

void editor_calibration_disabled_owner_contracts() {
  exercise_quit(false, false, 0);
}

void editor_calibration_destroyed_owner_contracts() {
  exercise_quit(true, true, -7);
}
