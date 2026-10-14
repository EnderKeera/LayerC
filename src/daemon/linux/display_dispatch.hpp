#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace layerc::daemon {
// Work surfaced by one pass over all display/input providers.
struct DisplayDispatch {
  std::vector<std::string> triggered;
  std::size_t events{};
  bool windows_changed{};
  bool devices_changed{};
};
} // namespace layerc::daemon
