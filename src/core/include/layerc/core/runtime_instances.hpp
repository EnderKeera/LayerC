#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace layerc::core {

// Platform-neutral bookkeeping for the daemon's profile/window instances. A
// window value is an opaque native handle cast to an integer by the platform.
struct RuntimeInstanceKey {
  std::wstring profile_key;
  std::uintptr_t window{};

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
  foreground(std::uintptr_t window) const;
  [[nodiscard]] bool contains(const RuntimeInstanceKey &key) const;
  void erase_profile(const std::wstring &profile_key);
  [[nodiscard]] std::size_t size() const noexcept { return active_.size(); }

private:
  std::vector<RuntimeInstanceKey> active_;
};

} // namespace layerc::core
