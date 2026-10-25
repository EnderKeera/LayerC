#pragma once

#include "display_dispatch.hpp"
#include "session_registry.hpp"
#include "daemon_host.hpp"
#include "display_providers.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/automation_session.hpp"
#include "layerc/platform/automation_persistence.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// The daemon's Capability::input service: an ordered set of borrowed passive
// sources (route order, fallback last) feeding the registry's sessions. A
// source opens only while the kernel demands input, so an idle daemon holds no
// input resources. Sources outlive the service.
class InputService final : public layerc::runtime::IRuntimeService {
public:
  explicit InputService(SessionRegistry &registry) noexcept
      : registry_(registry) {}

  void use_sources(std::span<PassiveInputSource *const> ordered);
  void release() noexcept;
  [[nodiscard]] PassiveInputSource *attached() const noexcept;
  [[nodiscard]] DisplayDispatch poll(std::span<const int> ready);
  [[nodiscard]] std::vector<std::string> unavailable() const;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void feed(std::span<const layerc::core::InputEvent> events, DisplayDispatch &result);

private:
  SessionRegistry &registry_;
  std::vector<PassiveInputSource *> sources_;
};

class AutomationService final : public layerc::runtime::IRuntimeService {
public:
  explicit AutomationService(SessionRegistry &registry) noexcept
      : registry_(registry) {}

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  SessionRegistry &registry_;
};

} // namespace layerc::daemon
