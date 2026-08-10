#include "layerc/capture/window_capture.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  const HWND snapshot_window = CreateWindowExW(
      0, L"STATIC", L"LayerC snapshot integration test", WS_POPUP, 0, 0, 32,
      16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (snapshot_window == nullptr) {
    std::cerr << "FAIL: interactive snapshot window could not be created\n";
    return EXIT_FAILURE;
  }
  const auto snapshot = layerc::capture::capture_static_window_snapshot(
      snapshot_window, layerc::core::CaptureQuality::exact,
      layerc::core::TimePoint{});
  DestroyWindow(snapshot_window);
  if (!snapshot || snapshot->width != 32 || snapshot->height != 16 ||
      snapshot->pixels.size() != 32U * 16U) {
    std::cerr << "FAIL: calibration snapshot did not return bounded RGB pixels\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
