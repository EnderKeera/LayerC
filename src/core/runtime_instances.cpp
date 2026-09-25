#include "layerc/core/runtime_instances.hpp"

#include <algorithm>
#include <iterator>

namespace layerc::core {

RuntimeInstanceChanges
RuntimeInstanceTracker::reconcile(std::vector<RuntimeInstanceKey> desired) {
  std::sort(desired.begin(), desired.end());
  desired.erase(std::unique(desired.begin(), desired.end()), desired.end());

  RuntimeInstanceChanges changes;
  std::set_difference(desired.begin(), desired.end(), active_.begin(), active_.end(),
                      std::back_inserter(changes.added));
  std::set_difference(active_.begin(), active_.end(), desired.begin(), desired.end(),
                      std::back_inserter(changes.removed));
  active_ = std::move(desired);
  return changes;
}

std::vector<RuntimeInstanceKey>
RuntimeInstanceTracker::foreground(const WindowId window) const {
  std::vector<RuntimeInstanceKey> matches;
  std::copy_if(active_.begin(), active_.end(), std::back_inserter(matches),
               [window](const RuntimeInstanceKey &key) {
                 return key.window == window;
               });
  return matches;
}

bool RuntimeInstanceTracker::contains(const RuntimeInstanceKey &key) const {
  return std::binary_search(active_.begin(), active_.end(), key);
}

void RuntimeInstanceTracker::erase_profile(const std::string &profile_key) {
  std::erase_if(active_, [&](const RuntimeInstanceKey &key) {
    return key.profile_key == profile_key;
  });
}

} // namespace layerc::core
