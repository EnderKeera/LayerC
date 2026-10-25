#pragma once

#include "display_providers.hpp"
#include "event_loop.hpp"
#include "layerc/runtime/display_diagnostics.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace layerc::daemon {

// The bus provider group owns only bus reports, overlay and portal transports.
// One demanded session-bus connection and all PipeWire descriptors share the
// daemon's EventLoop. No connection, thread or timer exists while idle.
class BusProviders final : public DisplaySource, public OverlayBackend,
                           public CaptureFactory {
public:
  explicit BusProviders(layerc::platform::EventLoop &loop);
  ~BusProviders() override;
  BusProviders(const BusProviders &) = delete;
  BusProviders &operator=(const BusProviders &) = delete;

  void configure(bool tracking, std::string_view desktop, bool configuration_changed);
  void want_overlay(bool wanted);
  void use_window_source(const DisplaySource *source);
  [[nodiscard]] bool dispatch(std::span<const int> ready);
  void present();
  void stop() noexcept;
  [[nodiscard]] std::optional<layerc::core::TimePoint> next_deadline() const noexcept;
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] bool kwin_owner() const;
  [[nodiscard]] bool gnome_owner() const;
  [[nodiscard]] std::optional<layerc::runtime::DisplayProviderStatus> window_provider() const;
  [[nodiscard]] std::optional<layerc::runtime::DisplayProviderStatus> overlay_provider() const;
  [[nodiscard]] layerc::runtime::DisplayProviderStatus capture_provider() const;
  [[nodiscard]] bool capture_demanded() const noexcept;

  [[nodiscard]] bool has_windows() const override;
  [[nodiscard]] layerc::platform::WindowScan windows() const override;
  [[nodiscard]] bool has_monitors() const override;
  [[nodiscard]] layerc::platform::MonitorScan monitors() const override;
  [[nodiscard]] OverlayBackend *overlay() override;
  [[nodiscard]] CaptureFactory *capture() override { return this; }

  [[nodiscard]] bool show(std::string_view key, const layerc::platform::OverlayFrame &frame) override;
  void hide(std::string_view key) noexcept override;
  void retain(std::span<const std::string> keys) noexcept override;
  [[nodiscard]] std::vector<std::string> unavailable() const override;
  [[nodiscard]] std::optional<std::uint64_t> x11_window(std::uint64_t window) const override;
  [[nodiscard]] std::unique_ptr<VisualCaptureSession>
  create_capture(std::string_view profile_id, layerc::core::WindowId window,
                 layerc::core::CaptureQuality quality, std::uint8_t sample_rate) override;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace layerc::daemon
