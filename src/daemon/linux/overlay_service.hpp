#pragma once

#include "input_service.hpp"
#include "x11_monitor.hpp"
#include "x11_overlay.hpp"
#include "x11_window.hpp"

#include "layerc/core/types.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// Repaint interval while an overlay is on screen. Nothing is scheduled once no
// overlay is mapped, so a daemon without an active session never wakes.
inline constexpr core::Milliseconds overlay_repaint_interval{100};

// The daemon's Capability::overlay service on X11: one click-through window
// per panel of every active session. Windows exist only while the kernel
// demands overlay, so an idle daemon holds no overlay resources.
class X11OverlayService final : public layerc::runtime::IRuntimeService {
public:
  // The display outlives the service; the shared connection owner keeps it.
  void attach(layerc::platform::X11Display &display) noexcept;

  // Rebuilds the panel list from the input sessions. A session whose window
  // is gone or hidden gets no overlay.
  void refresh(std::span<InputSession> sessions,
               const layerc::platform::X11WindowScan &scan,
               const layerc::platform::X11MonitorScan &monitors);

  // Paints every overlay from the sessions' current cooldown states.
  void render(std::span<InputSession> sessions,
              layerc::core::TimePoint now);

  // Armed only while an overlay is mapped; nullopt disarms the loop.
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_render_deadline(layerc::core::TimePoint now) const;

  [[nodiscard]] std::size_t window_count() const noexcept;
  // Overlay windows of the session matching `profile_id` on `session_window`.
  [[nodiscard]] std::size_t
  window_count(std::string_view profile_id,
               std::uint64_t session_window) const noexcept;
  [[nodiscard]] std::vector<std::uint64_t> windows() const;
  [[nodiscard]] std::vector<std::string> unavailable() const;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  struct Target {
    std::string profile_id;
    std::string panel_id;
    // The matched game window this panel belongs to.
    std::uint64_t session_window{};
    layerc::platform::X11OverlayWindow window;
    layerc::platform::X11OverlayFrame frame;
    // Override-redirect windows are unmanaged: while their game is the active
    // window they are re-raised, so nothing can stack the game above them.
    bool active{};
    // frame.panel carries an automation override that must be undone once
    // the override is cleared.
    bool overridden{};
  };

  layerc::platform::X11Display *display_{};
  std::vector<Target> targets_;
  std::string fault_;
};

} // namespace layerc::daemon
