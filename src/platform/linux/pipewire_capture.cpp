#include "pipewire_capture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace layerc::platform {

PipeWireCapture::~PipeWireCapture() { stop(); }

bool PipeWireCapture::start(PortalRemote remote) {
  stop();
  monitor_ = remote.monitor;
  monitor_geometry_ = remote.geometry;
  const char *reason{};
  native_ = layerc_pw_capture_new(std::exchange(remote.fd, -1), remote.node,
                                  this, on_error, on_frame, &reason);
  if (!native_) {
    reason_ = reason ? reason : "pipewire_start_failed";
    return false;
  }
  fd_ = layerc_pw_capture_fd(native_);
  if (fd_ < 0 || fatal_) {
    release_native();
    if (reason_.empty()) reason_ = "pipewire_start_failed";
    return false;
  }
  loop_->register_fd(fd_);
  return true;
}

void PipeWireCapture::release_native() noexcept {
  if (fd_ >= 0) loop_->unregister_fd(fd_);
  fd_ = -1;
  layerc_pw_capture_destroy(std::exchange(native_, nullptr));
  frame_.reset();
  health_.reset();
}

void PipeWireCapture::stop() noexcept {
  release_native();
  monitor_geometry_.reset();
  monitor_ = fatal_ = false;
  next_frame_ = {};
  reason_.clear();
}

void PipeWireCapture::set_quality(const core::CaptureQuality quality) noexcept {
  if (quality_ == quality) return;
  quality_ = quality;
  frame_.reset();
  health_.reset();
  next_frame_ = {};
}
void PipeWireCapture::set_window(const LogicalRect window) noexcept {
  if (window_geometry_.x == window.x && window_geometry_.y == window.y &&
      window_geometry_.width == window.width && window_geometry_.height == window.height) return;
  window_geometry_ = window;
  if (monitor_) {
    frame_.reset();
    health_.reset();
    next_frame_ = {};
  }
}
void PipeWireCapture::set_sample_rate(const std::uint8_t rate) noexcept {
  const auto next = std::clamp<std::uint8_t>(rate, 1, 60);
  if (next != sample_rate_) { sample_rate_ = next; next_frame_ = {}; }
}

void PipeWireCapture::on_error(void *userdata, const char *reason) noexcept {
  auto &self = *static_cast<PipeWireCapture *>(userdata);
  self.fatal_ = true;
  self.frame_.reset();
  self.health_.reset();
  try { self.reason_ = reason ? reason : "pipewire_connection_lost"; } catch (...) {}
}

void PipeWireCapture::on_frame(void *userdata, const layerc_pw_frame *frame) noexcept {
  auto &self = *static_cast<PipeWireCapture *>(userdata);
  // Allocation/evaluation exceptions must not cross a C library callback.
  try { if (frame && !self.fatal_) self.accept_frame(*frame); }
  catch (...) { on_error(userdata, "pipewire_frame_failed"); }
}

void PipeWireCapture::accept_frame(const layerc_pw_frame &frame) {
  const auto now = core::MonotonicClock::now(); // fixed before conversion/evaluation
  if (next_frame_ != core::TimePoint{} && now < next_frame_) return;
  std::optional<capture::PixelBounds> crop;
  std::optional<WindowCanvas> canvas;
  if (monitor_) {
    if (!monitor_geometry_) {
      on_error(this, "portal_monitor_geometry_unavailable");
      return;
    }
    crop = monitor_crop(window_geometry_, *monitor_geometry_, frame.width, frame.height);
    // A clipped monitor stream supplies only part of the window; place it in a
    // whole-window canvas so rule coordinates keep their meaning.
    if (crop)
      canvas = window_canvas(window_geometry_, *monitor_geometry_, frame.width, frame.height);
    if (!crop || !canvas) {
      frame_.reset();
      health_.reset();
      reason_ = "outside_output";
      return;
    }
  }
  constexpr std::array formats{VideoPixelFormat::bgrx, VideoPixelFormat::bgra,
                               VideoPixelFormat::rgbx, VideoPixelFormat::rgba};
  if (!frame.format || frame.format > formats.size()) {
    on_error(this, "pipewire_format_unsupported");
    return;
  }
  const RawVideoFrame raw{{static_cast<const std::byte *>(frame.data), frame.size},
                          frame.width, frame.height, frame.stride, formats[frame.format - 1]};
  frame_ = convert_video_frame(raw, quality_, crop, now, reason_, canvas);
  if (!frame_) {
    on_error(this, "pipewire_buffer_invalid");
    return;
  }
  next_frame_ = now + std::chrono::milliseconds(1'000 / sample_rate_);
  reason_.clear();
}

void PipeWireCapture::dispatch(const std::span<const int> ready) {
  if (fd_ < 0 || std::ranges::find(ready, fd_) == ready.end()) return;
  if (layerc_pw_capture_iterate(native_) < 0) on_error(this, "pipewire_dispatch_failed");
  if (fatal_) release_native(); // never destroy a transport inside its callback
}

capture::CapturePoll PipeWireCapture::poll(const core::TimePoint now) {
  const bool stale = frame_ && now - frame_->captured_at > std::chrono::seconds(2);
  const bool available = frame_ && reason_.empty() && !stale;
  auto result = health_.observe({
      available ? core::CaptureAvailability::available : core::CaptureAvailability::unavailable,
      core::CaptureBackendKind::pipewire_portal,
      stale ? "stale_frames" : reason_.empty() && !frame_ ? "portal_pending" : reason_,
      available ? frame_ : nullptr});
  return capture::normalize_capture_poll(window_, std::move(result));
}

runtime::RuntimeResourceStats PipeWireCapture::resources() const noexcept {
  runtime::RuntimeResourceStats result;
  if (native_) {
    result.objects = 4; // loop, context, core, stream
    result.handles = 2; // registered loop fd + portal-owned remote
    result.capture_sessions = 1;
  }
  if (frame_) result.cache_bytes = frame_->pixels.size() * sizeof(core::RgbPixel);
  return result; // no threads, background timer, or queued frame ownership
}

} // namespace layerc::platform
