#include "GameBarTransportState.hpp"

#include <algorithm>
#include <chrono>

namespace layerc::gamebar::detail {

void raise_wake_event(
    const std::shared_ptr<GameBarTransportState> &state) noexcept {
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
  if (state->link == GameBarTransportState::Link::stopped ||
      state->retry_timer != nullptr)
    return;
  const auto generation = state->generation;
  const auto delay = std::chrono::milliseconds(state->retry.delay_ms);
  state->retry.delay_ms = std::min(state->retry.delay_ms * 2U, 2'000U);
  state->retry_timer =
      winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
          [weak_state = std::weak_ptr(state), generation](const auto &) {
            const auto locked = weak_state.lock();
            if (!locked)
              return;
            {
              std::lock_guard lock(locked->mutex);
              if (locked->link == GameBarTransportState::Link::stopped ||
                  locked->generation != generation)
                return;
              locked->retry_timer = nullptr;
              locked->retry.ready = true;
            }
            raise_wake_event(locked);
          },
          delay);
#else
  (void)state;
#endif
}

} // namespace layerc::gamebar::detail
