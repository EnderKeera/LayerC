#include "calibration_window.hpp"

#include "layerc/core/calibration.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_handle.hpp"

#include <optional>
#include <string_view>
#include <variant>

#include <shellapi.h>
#include <windows.h>

namespace {

std::optional<HANDLE> handle_argument(const int argc, wchar_t **argv,
                                      const std::wstring_view name) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (argv[index] != name) continue;
    const auto value =
        layerc::platform::parse_integer<std::uintptr_t>(argv[index + 1]);
    if (!value || *value == 0) return std::nullopt;
    const auto handle = reinterpret_cast<HANDLE>(*value);
    DWORD flags{};
    if (GetHandleInformation(handle, &flags) == FALSE) return std::nullopt;
    return handle;
  }
  return std::nullopt;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int argc{};
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return 2;
  const auto request_pipe = handle_argument(argc, argv, L"--request-pipe");
  const auto response_pipe = handle_argument(argc, argv, L"--response-pipe");
  LocalFree(argv);
  if (!request_pipe.has_value() || !response_pipe.has_value()) return 2;
  layerc::platform::OwnedHandle request(*request_pipe);
  const layerc::platform::OwnedHandle response_handle(*response_pipe);

  const auto framed = layerc::platform::read_framed_pipe(
      request.get(), layerc::core::max_calibration_packet_bytes);
  request.reset();
  layerc::core::CalibrationResponse response;
  int exit_code{};
  if (!framed) {
    response.error = framed.error;
    exit_code = 2;
  } else {
    auto decoded = layerc::core::decode_calibration_request(framed.payload);
    if (const auto *error =
            std::get_if<layerc::core::CalibrationProtocolError>(&decoded)) {
      response.error = error->message;
      exit_code = 2;
    } else {
      layerc::calibration::CalibrationWindow window(
          std::get<layerc::core::CalibrationRequest>(std::move(decoded)));
      exit_code = window.run(instance, show_command);
      response = window.response();
    }
  }

  std::string write_error;
  const auto encoded = layerc::core::encode_calibration_response(response);
  if (!layerc::platform::write_framed_pipe(
          response_handle.get(), encoded,
          layerc::core::max_calibration_packet_bytes, write_error))
    exit_code = 2;
  return exit_code;
}
