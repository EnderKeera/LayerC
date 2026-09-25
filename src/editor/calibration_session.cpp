#include "calibration_session.hpp"
#include "editor_text.hpp"
#include "editor_util.hpp"

#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/window_query.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>

namespace layerc::editor {
namespace {

using layerc::platform::OwnedHandle;

struct WaitResult {
  bool request_timed_out{};
  bool request_failed{};
  bool session_timed_out{};
};

// Keep the disabled parent responsive to paint and task messages while the
// calibration window is open. Both deadlines use the monotonic tick counter.
WaitResult wait_modally(const HWND owner, const HANDLE process,
                        const HANDLE request_finished,
                        const std::atomic_bool &write_succeeded) {
  EnableWindow(owner, FALSE);
  constexpr ULONGLONG request_timeout_ms = 5'000ULL;
  constexpr ULONGLONG calibration_timeout_ms = 30ULL * 60ULL * 1'000ULL;
  const ULONGLONG request_deadline = GetTickCount64() + request_timeout_ms;
  const ULONGLONG calibration_deadline = GetTickCount64() + calibration_timeout_ms;
  WaitResult result;
  while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
    if (MsgWaitForMultipleObjects(1, &process, FALSE, 50, QS_ALLINPUT) ==
        WAIT_OBJECT_0 + 1) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    const ULONGLONG now = GetTickCount64();
    const DWORD request_state = WaitForSingleObject(request_finished, 0);
    if (request_state == WAIT_OBJECT_0 &&
        !write_succeeded.load(std::memory_order_acquire))
      result.request_failed = true;
    else if (request_state != WAIT_OBJECT_0 && now >= request_deadline)
      result.request_timed_out = true;
    else if (now >= calibration_deadline)
      result.session_timed_out = true;
    if (result.request_failed || result.request_timed_out ||
        result.session_timed_out)
      break;
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  return result;
}

} // namespace

std::variant<layerc::core::CalibrationResponse, CalibrationFailure>
run_calibration_session(const HWND owner,
                        const layerc::core::CalibrationRequest &request) {
  const auto child = layerc::platform::sibling_image_path(L"layercal.exe");
  if (child.empty() || !std::filesystem::exists(child))
    return CalibrationFailure{CalibrationFailureKind::missing_image, {}};

  SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                               .lpSecurityDescriptor = nullptr,
                               .bInheritHandle = TRUE};
  HANDLE raw_request_read{}, raw_request_write{};
  HANDLE raw_response_read{}, raw_response_write{};
  if (CreatePipe(&raw_request_read, &raw_request_write, &security, 0) == FALSE)
    return CalibrationFailure{CalibrationFailureKind::channel, {}};
  OwnedHandle request_read(raw_request_read), request_write(raw_request_write);
  if (CreatePipe(&raw_response_read, &raw_response_write, &security, 0) == FALSE)
    return CalibrationFailure{CalibrationFailureKind::channel, {}};
  OwnedHandle response_read(raw_response_read), response_write(raw_response_write);
  if (SetHandleInformation(request_write.get(), HANDLE_FLAG_INHERIT, 0) == FALSE ||
      SetHandleInformation(response_read.get(), HANDLE_FLAG_INHERIT, 0) == FALSE)
    return CalibrationFailure{CalibrationFailureKind::channel, {}};

  const std::array<HANDLE, 2> inherited{request_read.get(), response_write.get()};
  layerc::platform::HelperLaunchOptions options;
  options.inherited_handles = inherited;
  options.creation_flags = CREATE_UNICODE_ENVIRONMENT;
  const auto command = layerc::platform::quote_command_argument(child.wstring()) +
                       L" --request-pipe " +
                       std::to_wstring(reinterpret_cast<std::uintptr_t>(request_read.get())) +
                       L" --response-pipe " +
                       std::to_wstring(reinterpret_cast<std::uintptr_t>(response_write.get()));
  auto launched = layerc::platform::launch_bounded_helper(child, command, options);
  if (const auto *failure =
          std::get_if<layerc::platform::HelperLaunchError>(&launched)) {
    return CalibrationFailure{failure->step == layerc::platform::HelperLaunchStep::attributes
                                  ? CalibrationFailureKind::channel
                                  : CalibrationFailureKind::launch, {}};
  }
  auto &helper = std::get<layerc::platform::BoundedHelper>(launched);
  request_read.reset();
  response_write.reset();

  OwnedHandle request_finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  OwnedHandle io_finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!request_finished.valid() || !io_finished.valid()) {
    helper.terminate(ERROR_NOT_ENOUGH_MEMORY);
    return CalibrationFailure{CalibrationFailureKind::monitor, {}};
  }
  const auto payload = layerc::core::encode_calibration_request(request);
  layerc::platform::FramedPipeRead response;
  std::atomic_bool write_succeeded{};
  std::string write_error;
  std::thread io([&] {
    const bool wrote = layerc::platform::write_framed_pipe(
        request_write.get(), payload, layerc::core::max_calibration_packet_bytes,
        write_error);
    write_succeeded.store(wrote, std::memory_order_release);
    SetEvent(request_finished.get());
    if (wrote)
      response = layerc::platform::read_framed_pipe(
          response_read.get(), layerc::core::max_calibration_packet_bytes);
    SetEvent(io_finished.get());
  });

  const auto wait = wait_modally(owner, helper.process.get(), request_finished.get(),
                                 write_succeeded);
  if (wait.request_failed || wait.request_timed_out || wait.session_timed_out)
    helper.terminate(WAIT_TIMEOUT);
  if (WaitForSingleObject(io_finished.get(), 2'000) != WAIT_OBJECT_0) {
    (void)CancelSynchronousIo(reinterpret_cast<HANDLE>(io.native_handle()));
    (void)WaitForSingleObject(io_finished.get(), 2'000);
  }
  request_write.reset();
  response_read.reset();
  io.join();

  if (wait.request_timed_out)
    return CalibrationFailure{CalibrationFailureKind::request_timeout, {}};
  if (wait.session_timed_out)
    return CalibrationFailure{CalibrationFailureKind::session_timeout, {}};
  if (wait.request_failed || !write_succeeded.load(std::memory_order_acquire))
    return CalibrationFailure{CalibrationFailureKind::write, std::move(write_error)};
  if (!response)
    return CalibrationFailure{CalibrationFailureKind::read, std::move(response.error)};
  auto decoded = layerc::core::decode_calibration_response(response.payload);
  if (const auto *invalid =
          std::get_if<layerc::core::CalibrationProtocolError>(&decoded))
    return CalibrationFailure{CalibrationFailureKind::invalid_response,
                              invalid->message};
  auto result = std::get<layerc::core::CalibrationResponse>(std::move(decoded));
  if (!result.error.empty())
    return CalibrationFailure{CalibrationFailureKind::failed_response,
                              std::move(result.error)};
  return result;
}

void show_calibration_failure(const HWND owner, const CalibrationFailure &failure) {
  const wchar_t *title = L"Calibration unavailable";
  std::wstring message;
  switch (failure.kind) {
  case CalibrationFailureKind::missing_image:
    message = L"layercal.exe is missing beside LayerC Editor.";
    break;
  case CalibrationFailureKind::channel:
    message = L"Could not create the private calibration channel or restrict its handle inheritance.";
    break;
  case CalibrationFailureKind::launch:
    message = L"Could not start layercal.exe inside its bounded Job.";
    break;
  case CalibrationFailureKind::monitor:
    message = L"Could not monitor the calibration response.";
    break;
  case CalibrationFailureKind::request_timeout:
    title = L"Calibration channel timed out";
    message = L"LayerC Calibration did not begin reading its bounded request within five seconds and was stopped. The editor remained responsive and no calibration changes were applied.";
    break;
  case CalibrationFailureKind::session_timeout:
    title = L"Calibration timed out";
    message = L"LayerC Calibration exceeded the 30-minute safety limit and was stopped. No calibration changes were applied.";
    break;
  case CalibrationFailureKind::write:
    title = L"Calibration channel failed";
    message = to_wide(failure.detail);
    break;
  case CalibrationFailureKind::read:
    title = L"Calibration did not return a result";
    message = to_wide(failure.detail);
    break;
  case CalibrationFailureKind::invalid_response:
    title = L"Invalid calibration response";
    message = to_wide(failure.detail);
    break;
  case CalibrationFailureKind::failed_response:
    title = L"Calibration failed";
    message = to_wide(failure.detail);
    break;
  }
  MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

} // namespace layerc::editor
