#include "platform/calibration.hpp"
#include "x11_capture.hpp"
#include "x11_window.hpp"

#include <algorithm>

namespace layerc::editor {
namespace {
bool same_identity(const platform::X11WindowSnapshot &current,
                   const platform::X11WindowSnapshot &expected) {
  return !current.hidden && current.title == expected.title &&
      current.class_name == expected.class_name &&
      current.instance_name == expected.instance_name && current.pid == expected.pid &&
      current.executable_name == expected.executable_name &&
      current.executable_path == expected.executable_path;
}
bool matches(const platform::X11WindowSnapshot &window, const core::GameMatch &match) {
  platform::X11WindowScan candidate;
  candidate.windows.push_back(window);
  return platform::match_window(candidate, match) != nullptr;
}
} // namespace

struct CalibrationSource::Impl {
  // Reverse destruction releases capture and discovery before their display.
  std::optional<platform::X11Display> display;
  std::unique_ptr<platform::X11WindowTracker> tracker;
  std::unique_ptr<platform::X11CaptureSession> capture;
  std::vector<platform::X11WindowSnapshot> windows;
  std::optional<std::size_t> selected;

  bool open(std::string &error) {
    if (display) return true;
    display = platform::X11Display::open(error);
    if (!display) return false;
    tracker = std::make_unique<platform::X11WindowTracker>(*display);
    return true;
  }
};

CalibrationSource::CalibrationSource() : impl_{std::make_unique<Impl>()} {}
CalibrationSource::~CalibrationSource() = default;

CalibrationScan CalibrationSource::scan(const core::GameMatch &match) {
  stop();
  impl_->windows.clear();
  CalibrationScan result;
  std::string error;
  if (!impl_->open(error)) { result.fault = std::move(error); return result; }
  const auto scan = impl_->tracker->scan();
  result.warning = scan.warning;
  for (const auto &window : scan.windows) {
    if (window.hidden || !matches(window, match)) continue;
    impl_->windows.push_back(window);
    result.windows.push_back({impl_->windows.size(),
        window.title + (window.class_name.empty() ? "" : " — " + window.class_name)});
  }
  return result;
}

CalibrationStartResult CalibrationSource::start(std::uint64_t key,
    const core::GameMatch &match, core::CaptureQuality quality) {
  stop();
  if (key == 0 || key > impl_->windows.size())
    return {CalibrationStartState::identity_changed, {}};
  std::string error;
  if (!impl_->open(error))
    return {CalibrationStartState::unavailable, std::move(error)};
  const auto selected = static_cast<std::size_t>(key - 1);
  const auto &expected = impl_->windows[selected];
  const auto scan = impl_->tracker->scan();
  const auto current = std::ranges::find(scan.windows, expected.window,
                                         &platform::X11WindowSnapshot::window);
  if (current == scan.windows.end() || !same_identity(*current, expected) ||
      !matches(*current, match)) {
    stop();
    return {CalibrationStartState::identity_changed, {}};
  }
  impl_->selected = selected;
  impl_->capture = std::make_unique<platform::X11CaptureSession>(
      *impl_->display, core::WindowId{current->window}, quality);
  (void)impl_->capture->start();
  return {CalibrationStartState::started, {}};
}

void CalibrationSource::stop() noexcept {
  impl_->capture.reset();
  impl_->tracker.reset();
  impl_->display.reset();
  impl_->selected.reset();
}

bool CalibrationSource::running() const noexcept {
  return static_cast<bool>(impl_->capture);
}

CalibrationSample CalibrationSource::poll(std::uint64_t key, core::TimePoint now) {
  if (!impl_->capture || !impl_->selected) return {};
  (void)impl_->tracker->poll_changes();
  if (key == 0 || key - 1 != *impl_->selected) {
    stop();
    return {true, {}};
  }
  const auto &expected = impl_->windows[*impl_->selected];
  const auto current = impl_->tracker->find(expected.window);
  if (!current || !same_identity(*current, expected)) {
    stop();
    return {true, {}};
  }
  auto poll = impl_->capture->poll(now);
  return {false, std::move(poll.result)};
}

void CalibrationSource::set_quality(core::CaptureQuality quality) noexcept {
  if (impl_->capture) impl_->capture->set_quality(quality);
}
} // namespace layerc::editor
