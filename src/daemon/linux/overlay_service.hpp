#pragma once

#include "display_providers.hpp"
#include "session_registry.hpp"

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

// Repaint interval while an overlay is shown or a demanded show has failed.
// Hidden, removed and quiesced targets leave no deadline armed.
inline constexpr core::Milliseconds overlay_repaint_interval{100};

// One panel per active session, painted through the selected backend only
// while the kernel demands overlay.
class OverlayService final : public layerc::runtime::IRuntimeService {
public:
  // The backend outlives the service or is reset before destruction.
  void use_backend(OverlayBackend *backend) noexcept;

  // Rebuilds the panel list from the input sessions. A session whose window
  // is gone or hidden gets no overlay.
  void refresh(std::span<InputSession> sessions,
               const layerc::platform::WindowScan &scan,
               const layerc::platform::MonitorScan &monitors);

  // Paints every overlay from the sessions' current cooldown states.
  void render(std::span<InputSession> sessions,
              layerc::core::TimePoint now);

  // Armed while shown or retrying a demanded failed show; otherwise disarmed.
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_render_deadline(layerc::core::TimePoint now) const;

  [[nodiscard]] std::size_t window_count() const noexcept;
  // Overlay windows of the session matching `profile_id` on `session_window`.
  [[nodiscard]] std::size_t
  window_count(std::string_view profile_id,
               std::uint64_t session_window) const noexcept;
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
    layerc::platform::OverlayFrame frame;
    // frame.panel carries an automation override that must be undone once
    // the override is cleared.
    bool overridden{};
    // Backend mode: the backend currently shows this panel.
    bool shown{};
    // A failed demanded show is retried without depending on another event.
    std::optional<layerc::core::TimePoint> retry_at;
  };

  [[nodiscard]] static std::string key(const Target &target);

  OverlayBackend *backend_{};
  std::vector<Target> targets_;
  bool reconciled_{};
};

} // namespace layerc::daemon
