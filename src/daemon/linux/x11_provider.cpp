#include "x11_provider.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace layerc::daemon {
namespace {
class X11Capture final : public VisualCaptureSession {
public:
  X11Capture(const layerc::platform::X11Display &display,
             const layerc::runtime::WindowId window,
             const layerc::core::CaptureQuality quality)
      : session_(display, window, quality) {}
  bool start() override { return session_.start(); }
  void stop() noexcept override { session_.stop(); }
  void set_quality(layerc::core::CaptureQuality quality) noexcept override {
    session_.set_quality(quality);
  }
  void set_sample_rate(std::uint8_t) noexcept override {}
  layerc::capture::CapturePoll poll(layerc::core::TimePoint now) override {
    return session_.poll(now);
  }
  layerc::runtime::RuntimeResourceStats resources() const noexcept override {
    return session_.resources();
  }
private:
  layerc::platform::X11CaptureSession session_;
};
} // namespace

bool X11OverlayBackend::show(const std::string_view key,
                              const layerc::platform::OverlayFrame &frame) {
  if (!display_.valid() ||
      !layerc::platform::overlay_unavailable(display_.probe()).empty())
    return false;
  auto found = windows_.find(key);
  if (found == windows_.end()) {
    std::string error;
    auto window = layerc::platform::X11OverlayWindow::create(display_, error);
    if (!window) {
      if (error.empty()) error = "overlay: X11 overlay creation failed";
      faults_.insert_or_assign(std::string(key), std::move(error));
      return false;
    }
    found = windows_.emplace(std::string(key),
                             Window{std::move(*window), generation_}).first;
  }
  auto &entry = found->second;
  // Raise before painting only after a foreign stacking or focus change.
  if (frame.anchor_active && entry.raised_generation != generation_) {
    entry.window.raise();
    entry.raised_generation = generation_;
  }
  if (!entry.window.update(frame)) {
    faults_.insert_or_assign(std::string(key), "overlay: X11 overlay update failed");
    windows_.erase(found);
    return false;
  }
  const auto fault = faults_.find(key);
  if (fault != faults_.end()) faults_.erase(fault);
  return true;
}
void X11OverlayBackend::hide(const std::string_view key) noexcept {
  const auto found = windows_.find(key);
  if (found != windows_.end()) found->second.window.hide();
  const auto fault = faults_.find(key);
  if (fault != faults_.end()) faults_.erase(fault);
}
void X11OverlayBackend::retain(const std::span<const std::string> keys) noexcept {
  std::erase_if(windows_, [&](const auto &entry) {
    return std::ranges::find(keys, entry.first) == keys.end();
  });
  std::erase_if(faults_, [&](const auto &entry) {
    return std::ranges::find(keys, entry.first) == keys.end();
  });
}
std::vector<std::string> X11OverlayBackend::unavailable() const {
  if (!display_.valid()) return {"overlay: no X display for the overlay service"};
  auto reasons = layerc::platform::overlay_unavailable(display_.probe());
  if (reasons.empty()) {
    for (const auto &[key, reason] : faults_) {
      (void)key;
      reasons.push_back(reason);
    }
  }
  return reasons;
}
std::vector<std::uint64_t> X11OverlayBackend::windows() const {
  std::vector<std::uint64_t> result;
  for (const auto &[key, entry] : windows_) {
    (void)key;
    if (entry.window.valid()) result.push_back(entry.window.window());
  }
  return result;
}
std::optional<std::uint64_t> X11CaptureFactory::x11_window(const std::uint64_t window) const {
  return window;
}
std::unique_ptr<VisualCaptureSession> X11CaptureFactory::create_capture(
    std::string_view, const layerc::core::WindowId window,
    const layerc::core::CaptureQuality quality, std::uint8_t) {
  return std::make_unique<X11Capture>(display_, window, quality);
}

X11Provider::X11Provider(layerc::platform::X11Display display)
    : display_(std::move(display)) { initialize(); }
void X11Provider::initialize() {
  fd_ = display_->fd();
  tracker_.emplace(*display_);
  overlay_.emplace(*display_);
  capture_.emplace(*display_);
}
void X11Provider::start(layerc::platform::EventLoop &loop,
                         LinuxStatusSnapshot &status,
                         std::vector<std::string> &diagnostics) {
  auto opened = layerc::platform::X11Display::open_result();
  if (!opened.display.has_value()) {
    const char *session_type = std::getenv("XDG_SESSION_TYPE");
    const bool wayland = session_type != nullptr &&
                         std::string_view{session_type} == "wayland";
    // Typed open failure: the reason switches on the error code, never on
    // the human-readable message.
    const char *reason = "session_not_x11";
    if (opened.error ==
        layerc::platform::X11OpenError::cannot_open_display)
      reason = "permission_denied";
    else if (opened.error ==
                 layerc::platform::X11OpenError::no_display_variable &&
             std::getenv("FLATPAK_ID") != nullptr && !wayland)
      reason = "sandbox_denied";
    status.x11.reason = reason;
    for (auto &capability : status.capabilities)
      capability.reasons = {reason};
    diagnostics.push_back(
        opened.message +
        " (input, overlay, automation and capture capabilities stay unavailable)");
    return;
  }

  display_ = std::move(*opened.display);
  initialize();
  const auto &probe = display_->probe();
  status.x11 = {
      true,
      {probe.xi2.present, probe.xi2.major, probe.xi2.minor},
      {probe.shape.present, probe.shape.major, probe.shape.minor},
      {probe.randr.present, probe.randr.major, probe.randr.minor},
      {probe.composite.present, probe.composite.major, probe.composite.minor},
      {probe.damage.present, probe.damage.major, probe.damage.minor},
      {probe.xfixes.present, probe.xfixes.major, probe.xfixes.minor},
      probe.ewmh,
      {},
  };
  loop.register_fd(fd_);
  for (const auto &reason : probe.unavailable())
    diagnostics.push_back("x11 " + reason);
}

layerc::platform::WindowScan X11Provider::windows() const {
  return tracker_ ? tracker_->scan() : layerc::platform::WindowScan{};
}
layerc::platform::MonitorScan X11Provider::monitors() const {
  return display_ ? layerc::platform::scan_monitors(*display_)
                  : layerc::platform::MonitorScan{};
}
bool X11Provider::input_available() const noexcept {
  return display_ && display_->valid() && display_->probe().xi2.present;
}
bool X11Provider::open_input(std::string &error) {
  if (input_) return true;
  if (!display_) { error = "input: no X display for the input service"; return false; }
  input_ = layerc::platform::X11InputSource::create(*display_, error);
  return input_.has_value();
}
std::vector<std::string> X11Provider::input_unavailable(const InputDeviceNeeds needed) const {
  if (!input_) return {"input: XI2 source is not attached"};
  return input_->unavailable({.keyboard = needed.keyboard, .mouse = needed.mouse,
                              .gamepad = needed.gamepad});
}
layerc::runtime::RuntimeResourceStats X11Provider::input_resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result;
  result.handles = input_ ? 1U : 0U;
  result.objects = input_ ? 1U : 0U;
  return result;
}
X11Provider::Dispatch X11Provider::dispatch(const std::span<const int> ready) {
  if (fd_ < 0 || std::ranges::find(ready, fd_) == ready.end()) return {};
  if (tracker_ && overlay_) tracker_->ignore_windows(overlay_->windows());
  Dispatch result;
  if (input_) {
    auto poll = input_->poll(&*tracker_);
    result = {std::move(poll.events), poll.windows_changed, poll.devices_changed};
  } else {
    result.windows_changed = tracker_ && tracker_->poll_changes();
  }
  if (result.windows_changed && overlay_) overlay_->restack();
  return result;
}
bool X11Provider::has_pending_events() noexcept {
  try { return display_ && display_->pending_events() > 0; }
  catch (...) { return false; }
}
X11Provider::Dispatch X11Provider::dispatch_pending() {
  if (!has_pending_events()) return {};
  return dispatch({&fd_, 1});
}
} // namespace layerc::daemon
