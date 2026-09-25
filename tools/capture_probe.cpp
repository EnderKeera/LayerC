#include "layerc/capture/window_capture.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/text.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

namespace {

std::optional<std::uint64_t> unsigned_argument(const int argc, wchar_t **argv,
                                               const std::wstring_view name) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (argv[index] != name) continue;
    if (const auto value =
            layerc::platform::parse_integer<std::uint64_t>(argv[index + 1]))
      return value;
  }
  return std::nullopt;
}

layerc::core::CaptureQuality quality_argument(const int argc,
                                              wchar_t **argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) != L"--quality") continue;
    const std::wstring_view value(argv[index + 1]);
    if (value == L"low") return layerc::core::CaptureQuality::low;
    if (value == L"exact") return layerc::core::CaptureQuality::exact;
  }
  return layerc::core::CaptureQuality::balanced;
}

const char *backend_name(const layerc::core::CaptureBackendKind backend) {
  switch (backend) {
  case layerc::core::CaptureBackendKind::windows_graphics_capture:
    return "wgc";
  case layerc::core::CaptureBackendKind::desktop_duplication:
    return "desktopDuplication";
  default:
    return "none";
  }
}

} // namespace

int wmain(const int argc, wchar_t **argv) {
  const auto raw_window = unsigned_argument(argc, argv, L"--hwnd");
  const auto attempts = unsigned_argument(argc, argv, L"--attempts").value_or(180);
  if (!raw_window || *raw_window == 0 || attempts == 0) {
    std::cerr << "usage: layerc_capture_probe --hwnd <decimal> "
                 "[--quality low|balanced|exact] [--attempts 180]\n";
    return 2;
  }
  const auto window = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(*raw_window));
  if (!IsWindow(window)) {
    std::cerr << "capture target is not a window\n";
    return 2;
  }
  const auto quality = quality_argument(argc, argv);
  layerc::capture::WindowCaptureSession capture(
      window, {.allow_desktop_duplication = true, .quality = quality});
  (void)capture.start();
  layerc::capture::CapturePollResult latest;
  for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
    const bool foreground =
        GetAncestor(GetForegroundWindow(), GA_ROOT) == window;
    latest =
        capture.poll(layerc::core::MonotonicClock::now(), foreground).result;
    if (latest.availability ==
            layerc::core::CaptureAvailability::available &&
        latest.frame)
      break;
    if (latest.availability ==
            layerc::core::CaptureAvailability::protected_or_black ||
        latest.availability ==
            layerc::core::CaptureAvailability::outside_output)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  const auto gpu = capture.gpu_description();
  capture.stop();
  std::string output = "{\"backend\":\"";
  output += backend_name(latest.backend);
  output += "\",\"availability\":\"";
  output += layerc::core::enum_wire_name(layerc::core::capture_availability_wire,
                                        latest.availability)
                .value_or("inactive");
  output += "\",\"gpu\":";
  layerc::core::json::append_quoted(output, gpu);
  output += ",\"width\":";
  layerc::core::json::append_unsigned(output,
                                      latest.frame ? latest.frame->width : 0);
  output += ",\"height\":";
  layerc::core::json::append_unsigned(output,
                                      latest.frame ? latest.frame->height : 0);
  output += ",\"diagnostic\":";
  layerc::core::json::append_quoted(output, latest.diagnostic);
  output += "}\n";
  std::cout << output;
  return latest.availability == layerc::core::CaptureAvailability::available &&
                 latest.frame && !gpu.empty()
             ? 0
             : 3;
}
