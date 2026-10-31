#pragma once

#include "daemon_host.hpp"
#include "display_providers.hpp"
#include "event_loop.hpp"
#include "x11_capture.hpp"
#include "x11_display.hpp"
#include "x11_input.hpp"
#include "x11_monitor.hpp"
#include "x11_overlay.hpp"
#include "x11_window.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>

namespace layerc::daemon {

class X11OverlayBackend final : public OverlayBackend {
public:
  explicit X11OverlayBackend(layerc::platform::X11Display &display) noexcept
      : display_(display) {}
  X11OverlayBackend(const X11OverlayBackend &) = delete;
  X11OverlayBackend &operator=(const X11OverlayBackend &) = delete;
  X11OverlayBackend(X11OverlayBackend &&) = delete;
  X11OverlayBackend &operator=(X11OverlayBackend &&) = delete;
  [[nodiscard]] bool show(std::string_view key,
                          const layerc::platform::OverlayFrame &frame) override;
  void hide(std::string_view key) noexcept override;
  void retain(std::span<const std::string> keys) noexcept override;
  [[nodiscard]] std::vector<std::string> unavailable() const override;
  [[nodiscard]] std::vector<std::uint64_t> windows() const;
  void restack() noexcept { ++generation_; }

private:
  layerc::platform::X11Display &display_;
  struct Window {
    layerc::platform::X11OverlayWindow window;
    std::uint64_t raised_generation{};
  };
  std::map<std::string, Window, std::less<>> windows_;
  std::uint64_t generation_{};
  std::map<std::string, std::string, std::less<>> faults_;
};

class X11CaptureFactory final : public CaptureFactory {
public:
  explicit X11CaptureFactory(layerc::platform::X11Display &display) noexcept
      : display_(display) {}
  X11CaptureFactory(const X11CaptureFactory &) = delete;
  X11CaptureFactory &operator=(const X11CaptureFactory &) = delete;
  X11CaptureFactory(X11CaptureFactory &&) = delete;
  X11CaptureFactory &operator=(X11CaptureFactory &&) = delete;
  [[nodiscard]] std::optional<std::uint64_t> x11_window(std::uint64_t window) const override;
  [[nodiscard]] std::unique_ptr<VisualCaptureSession>
  create_capture(std::string_view profile_id, layerc::core::WindowId window,
                 layerc::core::CaptureQuality quality, std::uint8_t sample_rate) override;

private:
  layerc::platform::X11Display &display_;
};

// Owns the one X11 connection, its passive decoder and graphical transports.
// Dispatch returns raw input; only the neutral engine feeds sessions.
class X11Provider final : public DisplaySource, public PassiveInputSource {
public:
  struct Dispatch {
    std::vector<layerc::core::InputEvent> events;
    bool windows_changed{};
    bool devices_changed{};
    bool repaint_requested{};
  };
  X11Provider() = default;
  explicit X11Provider(layerc::platform::X11Display display);
  X11Provider(const X11Provider &) = delete;
  X11Provider &operator=(const X11Provider &) = delete;
  X11Provider(X11Provider &&) = delete;
  X11Provider &operator=(X11Provider &&) = delete;
  void start(layerc::platform::EventLoop &loop, LinuxStatusSnapshot &status,
             std::vector<std::string> &diagnostics);
  [[nodiscard]] Dispatch dispatch(std::span<const int> ready);
  [[nodiscard]] bool has_pending_events() noexcept;
  [[nodiscard]] Dispatch dispatch_pending();

  [[nodiscard]] bool has_windows() const override { return tracker_.has_value(); }
  [[nodiscard]] layerc::platform::WindowScan windows() const override;
  [[nodiscard]] bool has_monitors() const override { return display_.has_value(); }
  [[nodiscard]] layerc::platform::MonitorScan monitors() const override;
  [[nodiscard]] OverlayBackend *overlay() override { return overlay_ ? &*overlay_ : nullptr; }
  [[nodiscard]] CaptureFactory *capture() override { return capture_ ? &*capture_ : nullptr; }
  [[nodiscard]] PassiveInputSource *input() override { return this; }
  [[nodiscard]] bool input_available() const noexcept override;
  [[nodiscard]] std::string_view input_name() const noexcept override { return "XI2"; }
  [[nodiscard]] bool open_input(std::string &error) override;
  void close_input() noexcept override { input_.reset(); }
  [[nodiscard]] bool input_open() const noexcept override { return input_.has_value(); }
  [[nodiscard]] std::vector<std::string> input_unavailable(InputDeviceNeeds needed) const override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats input_resources() const noexcept override;

  [[nodiscard]] layerc::platform::X11Display &display() { return *display_; }
  [[nodiscard]] layerc::platform::X11WindowTracker &tracker() { return *tracker_; }

private:
  void initialize();
  std::optional<layerc::platform::X11Display> display_;
  std::optional<layerc::platform::X11WindowTracker> tracker_;
  std::optional<layerc::platform::X11InputSource> input_;
  std::optional<X11OverlayBackend> overlay_;
  std::optional<X11CaptureFactory> capture_;
  int fd_{-1};
};

} // namespace layerc::daemon
