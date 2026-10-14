#pragma once

#include "display_providers.hpp"

#include "layerc/runtime/display_diagnostics.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {
class WaylandConnection;
}

namespace layerc::daemon {

// The native Wayland providers (wl_output/xdg-output monitors, wlr foreign
// toplevels, wlr layer-shell overlay) on the daemon's one connection. They
// bind lazily from the registry and are released by the connection's hook
// before its display goes away; no thread, no second connection.
class WaylandProviders final : public NativeDisplaySource, public OverlayBackend {
public:
  // The connection outlives this object.
  explicit WaylandProviders(layerc::platform::WaylandConnection &connection);
  ~WaylandProviders() override;
  WaylandProviders(const WaylandProviders &) = delete;
  WaylandProviders &operator=(const WaylandProviders &) = delete;

  // Rebinds after a registry change.
  void sync();
  // True once after windows, monitors or overlay surfaces changed.
  [[nodiscard]] bool take_changed() noexcept;
  // Paints surfaces whose configure arrived since the last frame.
  void repaint();
  // The seat keyboard's XKB keymap for evdev translation; the flag is true
  // once after it changed.
  [[nodiscard]] bool take_keymap_changed() noexcept;
  [[nodiscard]] const std::optional<std::string> &keymap() const noexcept;

  [[nodiscard]] bool has_windows() const override;
  [[nodiscard]] layerc::platform::WindowScan windows() const override;
  [[nodiscard]] bool has_monitors() const override;
  [[nodiscard]] layerc::platform::MonitorScan monitors() const override;
  [[nodiscard]] OverlayBackend *overlay() override;

  [[nodiscard]] bool show(std::string_view key,
                          const layerc::platform::OverlayFrame &frame) override;
  void hide(std::string_view key) noexcept override;
  void retain(std::span<const std::string> keys) noexcept override;
  [[nodiscard]] std::vector<std::string> unavailable() const override;

  // Status rows for the native providers; `unavailable` with reasons when a
  // provider cannot be used (the caller may report its fallback instead).
  [[nodiscard]] layerc::runtime::DisplayProviderStatus window_provider() const;
  [[nodiscard]] layerc::runtime::DisplayProviderStatus monitor_provider() const;
  [[nodiscard]] layerc::runtime::DisplayProviderStatus overlay_provider() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::daemon
