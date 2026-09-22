#pragma once

#include "layerc/core/types.hpp"

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

// Platform-neutral bookkeeping for the daemon's profile/window instances.
// WindowId is the shared opaque native-window token; native handles never cross
// this boundary.
struct RuntimeInstanceKey {
  std::string profile_key;
  WindowId window{};

  auto operator<=>(const RuntimeInstanceKey &) const = default;
};

struct RuntimeInstanceChanges {
  std::vector<RuntimeInstanceKey> added;
  std::vector<RuntimeInstanceKey> removed;
};

class RuntimeInstanceTracker {
public:
  [[nodiscard]] RuntimeInstanceChanges
  reconcile(std::vector<RuntimeInstanceKey> desired);
  [[nodiscard]] std::vector<RuntimeInstanceKey>
  foreground(WindowId window) const;
  [[nodiscard]] bool contains(const RuntimeInstanceKey &key) const;
  void erase_profile(const std::string &profile_key);
  [[nodiscard]] std::span<const RuntimeInstanceKey> active() const noexcept {
    return active_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return active_.size(); }

private:
  std::vector<RuntimeInstanceKey> active_;
};

} // namespace layerc::core
