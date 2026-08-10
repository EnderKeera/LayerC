#pragma once

#include "layerc/capture/window_capture.hpp"

#include <memory>
#include <span>
#include <string>

namespace layerc::capture {

class CaptureBackend {
public:
  virtual ~CaptureBackend() = default;
  [[nodiscard]] virtual bool start() = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual CapturePollResult poll(core::TimePoint now,
                                                bool foreground) = 0;
  virtual void set_excluded_screen_rects(std::span<const RECT> rectangles) = 0;
  virtual void set_quality(core::CaptureQuality quality) = 0;
  [[nodiscard]] virtual core::CaptureBackendKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::string gpu_description() const = 0;
  [[nodiscard]] virtual std::string last_error() const = 0;
};

[[nodiscard]] bool capture_window_bounds(HWND window, RECT &bounds) noexcept;
[[nodiscard]] std::unique_ptr<CaptureBackend> make_wgc_backend(HWND window);
[[nodiscard]] std::unique_ptr<CaptureBackend>
make_desktop_duplication_backend(HWND window);

} // namespace layerc::capture
