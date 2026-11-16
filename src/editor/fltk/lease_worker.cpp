#include "lease_worker.hpp"
#include "platform/services.hpp"

#include <stdexcept>

namespace layerc::editor {
namespace core = layerc::core;

LeaseWorker::~LeaseWorker() { stop(); }

void LeaseWorker::start(std::function<void()> body) {
  const std::lock_guard lock(mutex_);
  if (stopping_ || thread_.joinable())
    throw std::logic_error("lease worker already started or stopped");
  thread_ = std::thread(std::move(body));
}

void LeaseWorker::stop() noexcept {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::string make_lease_client_id() {
  return std::to_string(current_process_id()) + "-" +
         std::to_string(core::MonotonicClock::now().time_since_epoch().count());
}

} // namespace layerc::editor
