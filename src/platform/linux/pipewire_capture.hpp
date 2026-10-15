#pragma once

#include "event_loop.hpp"
#include "portal_screencast.hpp"
#include "pipewire_transport.h"

#include "layerc/runtime/runtime_services.hpp"

#include <memory>
#include <span>

namespace layerc::platform {

// Owns one portal-authorized stream. All native dispatch and conversion stay
// on the daemon event loop; the C transport also disables PipeWire data loops.
class PipeWireCapture {
public:
  PipeWireCapture(EventLoop &loop, core::WindowId window, core::CaptureQuality quality)
      : loop_(&loop), window_(window), quality_(quality) {}
  ~PipeWireCapture();
  PipeWireCapture(const PipeWireCapture &) = delete;
  PipeWireCapture &operator=(const PipeWireCapture &) = delete;

  [[nodiscard]] bool start(PortalRemote remote);
  void stop() noexcept;
  void dispatch(std::span<const int> ready);
  [[nodiscard]] capture::CapturePoll poll(core::TimePoint now);
  void set_quality(core::CaptureQuality quality) noexcept;
  void set_window(LogicalRect window) noexcept;
  void set_sample_rate(std::uint8_t rate) noexcept;
  [[nodiscard]] const std::string &reason() const noexcept { return reason_; }
  [[nodiscard]] bool active() const noexcept { return native_ != nullptr; }
  [[nodiscard]] runtime::RuntimeResourceStats resources() const noexcept;

private:
  static void on_error(void *userdata, const char *reason) noexcept;
  static void on_frame(void *userdata, const layerc_pw_frame *frame) noexcept;
  void accept_frame(const layerc_pw_frame &frame);
  void release_native() noexcept;

  EventLoop *loop_;
  core::WindowId window_;
  core::CaptureQuality quality_;
  layerc_pw_capture *native_{};
  int fd_{-1};
  LogicalRect window_geometry_{};
  std::optional<LogicalRect> monitor_geometry_;
  bool monitor_{}, fatal_{};
  std::uint8_t sample_rate_{20};
  core::TimePoint next_frame_{};
  std::string reason_;
  std::shared_ptr<const capture::CaptureFrame> frame_;
  capture::CaptureHealthTracker health_;
};

} // namespace layerc::platform
