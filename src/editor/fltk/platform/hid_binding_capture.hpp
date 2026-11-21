#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace layerc::editor {

struct HidCaptureUpdate {
  // Only a baseline notice keeps the listener active. A result or error stops
  // it before this update is delivered on the editor thread.
  bool listening{};
  std::optional<core::HidControlBinding> control;
  std::string text;
};

class HidBindingCapture {
public:
  using Update = std::function<void(HidCaptureUpdate)>;
  virtual ~HidBindingCapture() = default;

  // Returns display names, in index order. Native handles and device paths
  // never leave the adapter. Refresh stops capture and forgets its result.
  [[nodiscard]] virtual std::vector<std::string> scan(bool include_system) = 0;
  // A successful start begins delivering updates after it returns.
  [[nodiscard]] virtual std::optional<std::string>
  start(std::size_t index, Update update) = 0;
  virtual void stop() noexcept = 0;

  // Called only for a validated edit. Recheck that the captured device is
  // present and save the machine-local association, not its path in the profile.
  [[nodiscard]] virtual std::optional<std::string>
  assign_slot(const core::HidDeviceSelector &selector, bool replace) = 0;
};

[[nodiscard]] bool hid_binding_capture_available() noexcept;
[[nodiscard]] std::unique_ptr<HidBindingCapture> make_hid_binding_capture();

} // namespace layerc::editor
