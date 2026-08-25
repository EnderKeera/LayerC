#include "GameBarTransportState.hpp"

#include <algorithm>
#include <chrono>

namespace layerc::gamebar::detail {

void signal(const std::shared_ptr<GameBarTransportState> &state) noexcept {
  std::shared_ptr<GameBarWakeEvent> wake;
  {
    std::lock_guard lock(state->mutex);
    wake = state->wake_event;
  }
  if (wake != nullptr && wake->handle != nullptr)
    SetEvent(wake->handle);
}

void schedule_retry_locked(
    const std::shared_ptr<GameBarTransportState> &state) {
#if defined(_MSC_VER)
  if (!state->started || state->retry_timer != nullptr)
    return;
  const auto generation = state->generation;
  const auto delay = std::chrono::milliseconds(state->retry_ms);
  state->retry_ms = std::min(state->retry_ms * 2U, 2'000U);
  state->retry_timer =
      winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
          [weak_state = std::weak_ptr(state), generation](const auto &) {
            const auto locked = weak_state.lock();
            if (!locked)
              return;
            {
              std::lock_guard lock(locked->mutex);
              if (!locked->started || locked->generation != generation)
                return;
              locked->retry_timer = nullptr;
              locked->retry_ready = true;
            }
            signal(locked);
          },
          delay);
#else
  (void)state;
#endif
}

} // namespace layerc::gamebar::detail
