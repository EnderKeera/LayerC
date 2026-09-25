#pragma once

#include "x11_display.hpp"

#include "layerc/capture/capture.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace layerc::platform {

// Concrete capture session for one X11 window. The display must outlive the
// session; native X11 resources stay confined to the Linux adapter.
class X11CaptureSession {
public:
  X11CaptureSession(const X11Display &display, core::WindowId window,
                    core::CaptureQuality quality =
                        core::CaptureQuality::balanced) noexcept;
  ~X11CaptureSession();
  X11CaptureSession(const X11CaptureSession &) = delete;
  X11CaptureSession &operator=(const X11CaptureSession &) = delete;

  [[nodiscard]] bool start();
  void stop() noexcept;
  [[nodiscard]] capture::CapturePoll poll(core::TimePoint now);
  void set_quality(core::CaptureQuality quality) noexcept;

  [[nodiscard]] core::CaptureBackendKind backend() const noexcept {
    return backend_;
  }
  [[nodiscard]] runtime::RuntimeResourceStats resources() const noexcept;

private:
  void release_named_pixmap() noexcept;
  void release_damage() noexcept;
  void refresh_named_pixmap();
  void start_damage();
  [[nodiscard]] bool collect_damage();
  [[nodiscard]] std::shared_ptr<const capture::CaptureFrame>
  capture_frame(core::TimePoint now, std::string &failure);
  [[nodiscard]] capture::CapturePoll current_result();
  [[nodiscard]] capture::CapturePoll unavailable(std::string diagnostic,
                                                 core::CaptureBackendKind backend)
      const;
  [[nodiscard]] std::string warning() const;

  const X11Display *display_{};
  core::WindowId window_{};
  core::CaptureQuality quality_{core::CaptureQuality::balanced};
  bool started_{};
  bool dirty_{};
  std::uint64_t pixmap_{};
  std::uint64_t damage_{};
  std::uint64_t damage_region_{};
  int damage_event_base_{};
  std::uint32_t source_width_{};
  std::uint32_t source_height_{};
  core::CaptureBackendKind backend_{core::CaptureBackendKind::none};
  std::shared_ptr<const capture::CaptureFrame> frame_;
  capture::CaptureHealthTracker health_;
  core::TimePoint next_capture_at_{};
  std::string fallback_reason_;
  std::string damage_reason_;
  std::string error_;
};

} // namespace layerc::platform
