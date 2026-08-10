#include "layerc/capture/window_capture.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
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
    wchar_t *end{};
    const auto value = wcstoull(argv[index + 1], &end, 10);
    if (end != argv[index + 1] && *end == L'\0') return value;
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

std::string escaped(const std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char character : value) {
    if (character == '"' || character == '\\') output.push_back('\\');
    if (static_cast<unsigned char>(character) >= 0x20U)
      output.push_back(character);
  }
  return output;
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

const char *availability_name(
    const layerc::core::CaptureAvailability availability) {
  switch (availability) {
  case layerc::core::CaptureAvailability::starting: return "starting";
  case layerc::core::CaptureAvailability::available: return "available";
  case layerc::core::CaptureAvailability::unavailable: return "unavailable";
  case layerc::core::CaptureAvailability::protected_or_black:
    return "protectedOrBlack";
  case layerc::core::CaptureAvailability::outside_output:
    return "outsideOutput";
  default: return "inactive";
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
    latest = capture.poll(layerc::core::MonotonicClock::now(), foreground);
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
  std::cout << "{\"backend\":\"" << backend_name(latest.backend)
            << "\",\"availability\":\""
            << availability_name(latest.availability) << "\",\"gpu\":\""
            << escaped(gpu) << "\",\"width\":"
            << (latest.frame ? latest.frame->width : 0)
            << ",\"height\":"
            << (latest.frame ? latest.frame->height : 0)
            << ",\"diagnostic\":\"" << escaped(latest.diagnostic)
            << "\"}\n";
  return latest.availability == layerc::core::CaptureAvailability::available &&
                 latest.frame && !gpu.empty()
             ? 0
             : 3;
}
