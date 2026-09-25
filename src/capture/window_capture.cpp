#include "layerc/capture/window_capture.hpp"

#include "capture_backend.hpp"

#include <dwmapi.h>

#include <array>
#include <optional>
#include <utility>

namespace layerc::capture {

bool capture_window_bounds(const HWND window, RECT &bounds) noexcept {
  bounds = {};
  if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &bounds, sizeof(bounds))) &&
      bounds.right > bounds.left && bounds.bottom > bounds.top)
    return true;
  return GetWindowRect(window, &bounds) != FALSE &&
         bounds.right > bounds.left && bounds.bottom > bounds.top;
}

namespace {

// Ordered backend ladder: WGC first, Desktop Duplication only as a declared
// fallback. switch_to indexes this list so poll() never special-cases identity.
constexpr std::array<core::CaptureBackendKind, 2> backend_order{{
    core::CaptureBackendKind::windows_graphics_capture,
    core::CaptureBackendKind::desktop_duplication,
}};

std::optional<std::size_t>
backend_index(const core::CaptureBackendKind kind) noexcept {
  for (std::size_t index = 0; index < backend_order.size(); ++index) {
    if (backend_order[index] == kind)
      return index;
  }
  return std::nullopt;
}

// One message for a ladder whose declared fallback also failed.
std::string ladder_error(const std::string &first_error,
                         const std::string &fallback_error) {
  return "WGC: " + first_error + "; Desktop Duplication: " + fallback_error;
}

} // namespace

class WindowCaptureSession::Impl {
public:
  Impl(const HWND window, const CaptureOptions options)
      : window_(window), options_(options) {}

  bool start() {
    stop();
    const auto first = backend_order.front();
    if (start_backend(make_backend(first)))
      return true;
    const auto first_error = backend_error();
    backend_.reset();
    const auto fallback = next_backend(first);
    if (!fallback.has_value()) {
      error_ = first_error;
      return false;
    }
    if (!start_backend(make_backend(*fallback))) {
      error_ = ladder_error(first_error, backend_error());
      backend_.reset();
      return false;
    }
    fallback_reason_ = first_error;
    return true;
  }

  void stop() {
    if (backend_)
      backend_->stop();
    backend_.reset();
    error_.clear();
    fallback_reason_.clear();
    health_.reset();
  }

  CapturePollResult poll(const core::TimePoint now, const bool foreground) {
    if (!backend_)
      return {.availability = core::CaptureAvailability::unavailable,
              .backend = core::CaptureBackendKind::none,
              .diagnostic = error_.empty() ? "capture is not started" : error_,
              .frame = {}};
    auto result = backend_->poll(now, foreground);
    if (result.recovery == CaptureRecovery::restart_backend)
      return switch_to(result.backend, std::move(result.diagnostic), foreground);
    if (result.availability == core::CaptureAvailability::unavailable) {
      if (const auto next = next_backend(result.backend);
          next.has_value() && IsWindow(window_))
        return switch_to(*next, std::move(result.diagnostic), foreground);
    }
    if (result.backend == core::CaptureBackendKind::desktop_duplication &&
        !fallback_reason_.empty())
      result.diagnostic += "; WGC fallback reason: " + fallback_reason_;
    return health_.observe(std::move(result));
  }

  void set_exclusions(const std::span<const RECT> rectangles) {
    excluded_.assign(rectangles.begin(), rectangles.end());
    apply_exclusions();
  }

  void set_quality(const core::CaptureQuality quality) {
    options_.quality = quality;
    health_.reset();
    if (backend_)
      backend_->set_quality(quality);
  }

  HWND window() const noexcept { return window_; }
  core::CaptureBackendKind kind() const noexcept {
    return backend_ ? backend_->kind() : core::CaptureBackendKind::none;
  }
  std::string gpu_description() const {
    return backend_ ? backend_->gpu_description() : std::string{};
  }

private:
  std::string backend_error() const {
    return backend_ ? backend_->last_error()
                    : std::string("backend unavailable");
  }

  std::unique_ptr<CaptureBackend> make_backend(
      const core::CaptureBackendKind kind) {
    switch (kind) {
    case core::CaptureBackendKind::windows_graphics_capture:
      return make_wgc_backend(window_);
    case core::CaptureBackendKind::desktop_duplication:
      return make_desktop_duplication_backend(window_);
    case core::CaptureBackendKind::none:
    case core::CaptureBackendKind::x11_composite:
    case core::CaptureBackendKind::x11_full_frame:
      break;
    }
    return nullptr;
  }

  std::optional<core::CaptureBackendKind>
  next_backend(const core::CaptureBackendKind current) const noexcept {
    const auto index = backend_index(current);
    if (!index.has_value() || *index + 1 >= backend_order.size() ||
        !options_.allow_desktop_duplication)
      return std::nullopt;
    return backend_order[*index + 1];
  }

  bool start_backend(std::unique_ptr<CaptureBackend> backend) {
    backend_ = std::move(backend);
    if (!backend_ || !backend_->start()) return false;
    backend_->set_quality(options_.quality);
    apply_exclusions();
    health_.reset();
    error_.clear();
    return true;
  }

  // Single transition path for both WGC→DD fallback and same-backend restart.
  CapturePollResult switch_to(const core::CaptureBackendKind kind,
                              std::string reason, const bool foreground) {
    const auto previous =
        backend_ ? backend_->kind() : core::CaptureBackendKind::none;
    const bool is_fallback =
        previous == core::CaptureBackendKind::windows_graphics_capture &&
        previous != kind;
    if (is_fallback)
      fallback_reason_ = reason;

    backend_->stop();
    if (backend_index(kind).has_value() && start_backend(make_backend(kind))) {
      std::string diagnostic;
      if (is_fallback)
        diagnostic = "WGC became unavailable (" + fallback_reason_ +
                     "); Desktop Duplication fallback is starting";
      else
        diagnostic = reason + "; capture session restarted";
      return health_.observe(
          {.availability = foreground ? core::CaptureAvailability::starting
                                      : core::CaptureAvailability::inactive,
           .backend = kind,
           .diagnostic = std::move(diagnostic),
           .frame = {}});
    }
    const auto restart_error = backend_error();
    if (is_fallback)
      error_ = ladder_error(fallback_reason_, restart_error);
    else
      error_ = reason + "; restart failed: " + restart_error;
    backend_.reset();
    return health_.observe(
        {.availability = core::CaptureAvailability::unavailable,
         .backend = core::CaptureBackendKind::none,
         .diagnostic = error_,
         .frame = {}});
  }

  void apply_exclusions() {
    if (backend_)
      backend_->set_excluded_screen_rects(excluded_);
  }

  HWND window_{};
  CaptureOptions options_;
  std::unique_ptr<CaptureBackend> backend_;
  std::vector<RECT> excluded_;
  std::string error_;
  std::string fallback_reason_;
  CaptureHealthTracker health_;
};

WindowCaptureSession::WindowCaptureSession(const HWND window,
                                           const CaptureOptions options)
    : impl_(std::make_unique<Impl>(window, options)) {}
WindowCaptureSession::~WindowCaptureSession() = default;
WindowCaptureSession::WindowCaptureSession(WindowCaptureSession &&) noexcept =
    default;
WindowCaptureSession &
WindowCaptureSession::operator=(WindowCaptureSession &&) noexcept = default;

bool WindowCaptureSession::start() { return impl_->start(); }
void WindowCaptureSession::stop() { impl_->stop(); }
CapturePoll WindowCaptureSession::poll(const core::TimePoint now,
                                       const bool foreground) {
  return normalize_capture_poll(
      {reinterpret_cast<std::uintptr_t>(impl_->window())},
      impl_->poll(now, foreground));
}
void WindowCaptureSession::set_excluded_screen_rects(
    const std::span<const RECT> rectangles) {
  impl_->set_exclusions(rectangles);
}
void WindowCaptureSession::set_quality(const core::CaptureQuality quality) {
  impl_->set_quality(quality);
}
core::CaptureBackendKind WindowCaptureSession::backend() const noexcept {
  return impl_->kind();
}
std::string WindowCaptureSession::gpu_description() const {
  return impl_->gpu_description();
}

} // namespace layerc::capture
