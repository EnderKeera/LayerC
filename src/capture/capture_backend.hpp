#pragma once

#include "layerc/capture/window_capture.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace layerc::capture {

class CaptureBackend {
public:
  virtual ~CaptureBackend() = default;
  [[nodiscard]] virtual bool start() = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual CapturePollResult poll(core::TimePoint now,
                                                bool foreground) = 0;
  // Backends that capture an application surface directly may ignore desktop
  // exclusion rectangles; desktop-compositing backends must mark them unavailable.
  virtual void set_excluded_screen_rects(std::span<const RECT> rectangles) = 0;
  virtual void set_quality(core::CaptureQuality quality) = 0;
  [[nodiscard]] virtual core::CaptureBackendKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::string gpu_description() const = 0;
  [[nodiscard]] virtual std::string last_error() const = 0;
};

// Frame, quality, error, and started state shared by the Windows backends,
// so both build poll results one way. The owning backend keeps its device
// resources and any backend-specific diagnostic text.
class BackendFrameSlot {
public:
  explicit BackendFrameSlot(const core::CaptureBackendKind kind) noexcept
      : kind_(kind) {}

  [[nodiscard]] bool started() const noexcept { return started_; }
  void set_started(const bool started) noexcept { started_ = started; }

  [[nodiscard]] core::CaptureQuality quality() const noexcept {
    return quality_;
  }
  // A new quality invalidates the cached frame.
  void set_quality(const core::CaptureQuality quality) {
    if (quality_ == quality)
      return;
    quality_ = quality;
    frame_.reset();
  }

  void publish(std::shared_ptr<CaptureFrame> frame) noexcept {
    frame_ = std::move(frame);
  }
  void clear_frame() noexcept { frame_.reset(); }

  [[nodiscard]] const std::string &last_error() const noexcept {
    return error_;
  }
  void set_error(std::string error) { error_ = std::move(error); }
  void clear_error() noexcept { error_.clear(); }

  [[nodiscard]] CapturePollResult unavailable(std::string diagnostic) const {
    return {.availability = core::CaptureAvailability::unavailable,
            .backend = kind_,
            .diagnostic = std::move(diagnostic),
            .frame = {}};
  }
  // The cached frame when one exists, otherwise still starting.
  [[nodiscard]] CapturePollResult current(std::string diagnostic = {}) const {
    return {.availability = frame_ ? core::CaptureAvailability::available
                                   : core::CaptureAvailability::starting,
            .backend = kind_,
            .diagnostic = std::move(diagnostic),
            .frame = frame_};
  }

private:
  core::CaptureBackendKind kind_;
  std::shared_ptr<CaptureFrame> frame_;
  std::string error_;
  core::CaptureQuality quality_{core::CaptureQuality::balanced};
  bool started_{};
};

[[nodiscard]] bool capture_window_bounds(HWND window, RECT &bounds) noexcept;
[[nodiscard]] std::unique_ptr<CaptureBackend> make_wgc_backend(HWND window);
[[nodiscard]] std::unique_ptr<CaptureBackend>
make_desktop_duplication_backend(HWND window);

} // namespace layerc::capture
