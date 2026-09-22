#include "layerc/capture/window_capture.hpp"
#include "test_support.hpp"

namespace {

void capture_static_window_snapshot_case() {
  const HWND snapshot_window = CreateWindowExW(
      0, L"STATIC", L"LayerC snapshot integration test", WS_POPUP, 0, 0, 32,
      16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!layerc::test::expect(
          snapshot_window != nullptr,
          "interactive snapshot window should be created"))
    return;
  const auto snapshot = layerc::capture::capture_static_window_snapshot(
      snapshot_window, layerc::core::CaptureQuality::exact,
      layerc::core::TimePoint{});
  DestroyWindow(snapshot_window);
  layerc::test::expect(
      snapshot && snapshot->width == 32 && snapshot->height == 16 &&
          snapshot->pixels.size() == 32U * 16U,
      "calibration snapshot should return bounded RGB pixels");
}

} // namespace

int main() {
  return layerc::test::run({{"capture_static_window_snapshot_case",
                             capture_static_window_snapshot_case}});
}
