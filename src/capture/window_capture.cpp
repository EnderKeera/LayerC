#include "layerc/capture/window_capture.hpp"

#include "capture_backend.hpp"

#include <dwmapi.h>

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

// Backend ladder: WGC first, Desktop Duplication only as the declared fallback.
std::optional<core::CaptureBackendKind>
fallback_backend(const core::CaptureBackendKind current,
                 const CaptureOptions &options) noexcept {
  if (current != core::CaptureBackendKind::windows_graphics_capture ||
      !options.allow_desktop_duplication)
    return std::nullopt;
  return core::CaptureBackendKind::desktop_duplication;
}

std::unique_ptr<CaptureBackend> make_backend(const core::CaptureBackendKind kind,
                                             const HWND window) {
  switch (kind) {
  case core::CaptureBackendKind::windows_graphics_capture:
    return make_wgc_backend(window);
  case core::CaptureBackendKind::desktop_duplication:
    return make_desktop_duplication_backend(window);
  case core::CaptureBackendKind::none:
  case core::CaptureBackendKind::x11_composite:
  case core::CaptureBackendKind::x11_full_frame:
  case core::CaptureBackendKind::pipewire_portal:
    break;
  }
  return nullptr;
}

// One message for a ladder whose declared fallback also failed.
std::string ladder_error(const std::string &first_error,
                         const std::string &fallback_error) {
  return "WGC: " + first_error + "; Desktop Duplication: " + fallback_error;
}

} // namespace

WindowCaptureSession::WindowCaptureSession(const HWND window,
                                           const CaptureOptions options)
    : window_(window), options_(options) {}
WindowCaptureSession::~WindowCaptureSession() = default;
WindowCaptureSession::WindowCaptureSession(WindowCaptureSession &&) noexcept =
    default;
WindowCaptureSession &
WindowCaptureSession::operator=(WindowCaptureSession &&) noexcept = default;

bool WindowCaptureSession::start() {
  stop();
  constexpr auto first = core::CaptureBackendKind::windows_graphics_capture;
  if (start_backend(first))
    return true;
  const auto first_error = backend_error();
  backend_.reset();
  const auto fallback = fallback_backend(first, options_);
  if (!fallback.has_value()) {
    error_ = first_error;
    return false;
  }
  if (!start_backend(*fallback)) {
    error_ = ladder_error(first_error, backend_error());
    backend_.reset();
    return false;
  }
  fallback_reason_ = first_error;
  return true;
}

void WindowCaptureSession::stop() {
  if (backend_)
    backend_->stop();
  backend_.reset();
  error_.clear();
  fallback_reason_.clear();
  health_.reset();
}

CapturePoll WindowCaptureSession::poll(const core::TimePoint now,
                                       const bool foreground) {
  return normalize_capture_poll({reinterpret_cast<std::uintptr_t>(window_)},
                                poll_backend(now, foreground));
}

CapturePollResult WindowCaptureSession::poll_backend(const core::TimePoint now,
                                                     const bool foreground) {
  if (!backend_)
    return {.availability = core::CaptureAvailability::unavailable,
            .backend = core::CaptureBackendKind::none,
            .diagnostic = error_.empty() ? "capture is not started" : error_,
            .frame = {}};
  auto result = backend_->poll(now, foreground);
  if (result.recovery == CaptureRecovery::restart_backend)
    return switch_to(result.backend, std::move(result.diagnostic), foreground);
  if (result.availability == core::CaptureAvailability::unavailable) {
    if (const auto next = fallback_backend(result.backend, options_);
        next.has_value() && IsWindow(window_))
      return switch_to(*next, std::move(result.diagnostic), foreground);
  }
  if (result.backend == core::CaptureBackendKind::desktop_duplication &&
      !fallback_reason_.empty())
    result.diagnostic += "; WGC fallback reason: " + fallback_reason_;
  return health_.observe(std::move(result));
}

void WindowCaptureSession::set_excluded_screen_rects(
    const std::span<const RECT> rectangles) {
  excluded_.assign(rectangles.begin(), rectangles.end());
  apply_exclusions();
}

void WindowCaptureSession::set_quality(const core::CaptureQuality quality) {
  options_.quality = quality;
  health_.reset();
  if (backend_)
    backend_->set_quality(quality);
}

core::CaptureBackendKind WindowCaptureSession::backend() const noexcept {
  return backend_ ? backend_->kind() : core::CaptureBackendKind::none;
}

std::string WindowCaptureSession::gpu_description() const {
  return backend_ ? backend_->gpu_description() : std::string{};
}

std::string WindowCaptureSession::backend_error() const {
  return backend_ ? backend_->last_error() : std::string("backend unavailable");
}

bool WindowCaptureSession::start_backend(const core::CaptureBackendKind kind) {
  backend_ = make_backend(kind, window_);
  if (!backend_ || !backend_->start()) return false;
  backend_->set_quality(options_.quality);
  apply_exclusions();
  health_.reset();
  error_.clear();
  return true;
}

// Single transition path for both WGC→DD fallback and same-backend restart.
CapturePollResult WindowCaptureSession::switch_to(
    const core::CaptureBackendKind kind, std::string reason,
    const bool foreground) {
  const auto previous =
      backend_ ? backend_->kind() : core::CaptureBackendKind::none;
  const bool is_fallback =
      previous == core::CaptureBackendKind::windows_graphics_capture &&
      previous != kind;
  if (is_fallback)
    fallback_reason_ = reason;

  backend_->stop();
  if (start_backend(kind)) {
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

void WindowCaptureSession::apply_exclusions() {
  if (backend_)
    backend_->set_excluded_screen_rects(excluded_);
}

} // namespace layerc::capture
