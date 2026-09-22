#pragma once

#include "layerc/capture/capture.hpp"

#include <memory>
#include <span>
#include <string>

#include <windows.h>

namespace layerc::capture {

struct CaptureOptions {
  bool allow_desktop_duplication{true};
  core::CaptureQuality quality{core::CaptureQuality::balanced};
};

// Hardware-only window capture. Construction is cheap; D3D11 and WinRT are
// initialized lazily by start(). If WGC cannot start, the implementation can
// fall back to Desktop Duplication when allowed.
class WindowCaptureSession {
public:
  WindowCaptureSession(HWND window, CaptureOptions options = {});
  ~WindowCaptureSession();
  WindowCaptureSession(WindowCaptureSession &&) noexcept;
  WindowCaptureSession &operator=(WindowCaptureSession &&) noexcept;
  WindowCaptureSession(const WindowCaptureSession &) = delete;
  WindowCaptureSession &operator=(const WindowCaptureSession &) = delete;

  [[nodiscard]] bool start();
  void stop();
  [[nodiscard]] CapturePollResult poll(core::TimePoint now,
                                       bool target_is_foreground);
  void set_excluded_screen_rects(std::span<const RECT> rectangles);
  void set_quality(core::CaptureQuality quality);

  [[nodiscard]] core::CaptureBackendKind backend() const noexcept;
  [[nodiscard]] std::string gpu_description() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] CapturePoll poll_window_capture(
    core::WindowId window, WindowCaptureSession &session,
    core::TimePoint now, bool target_is_foreground);

// One-shot screen snapshot used only when calibration's Desktop Duplication
// fallback cannot obtain an initial frame from an otherwise static desktop.
[[nodiscard]] std::shared_ptr<CaptureFrame> capture_static_window_snapshot(
    HWND window, core::CaptureQuality quality, core::TimePoint captured_at);

} // namespace layerc::capture
