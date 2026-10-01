#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace layerc::gamebar::widget {

// One terminal boundary for a connection's task, deferral and revocations.
// Copies used during registration keep the resources alive until registration
// finishes. Close and a late registration both destroy resources outside locks.
template <typename Resources> class ServiceSessionOwner final {
public:
  explicit ServiceSessionOwner(std::shared_ptr<Resources> resources)
      : resources_(std::move(resources)) {}
  ~ServiceSessionOwner() { close(); }

  [[nodiscard]] std::shared_ptr<Resources> resources() const {
    std::lock_guard lock(mutex_);
    return resources_;
  }
  void install_revocations(
      std::unique_ptr<typename Resources::Revocations> revocations) {
    {
      std::lock_guard lock(mutex_);
      if (resources_)
        resources_->revocations.swap(revocations);
    }
  }
  void close() noexcept {
    std::shared_ptr<Resources> retired;
    {
      std::lock_guard lock(mutex_);
      retired = std::move(resources_);
    }
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<Resources> resources_;
};

} // namespace layerc::gamebar::widget
