#pragma once

#include "GameBarTransport.hpp"

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#if defined(_MSC_VER)
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/base.h>
#endif

namespace layerc::gamebar {

struct GameBarWakeEvent {
  GameBarWakeEvent() : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~GameBarWakeEvent() {
    if (handle != nullptr)
      CloseHandle(handle);
  }

  HANDLE handle{};
};

struct GameBarTransportState {
  mutable std::mutex mutex;
  std::shared_ptr<GameBarWakeEvent> wake_event;
  enum class Link { stopped, idle, opening, connected, sending };
  struct RetryState {
    bool ready{};
    bool needed{};
    unsigned delay_ms{100};
  };

  // Caller holds mutex for every transition and observation.
  void enter(Link next, std::optional<std::wstring> reason = std::nullopt) {
    link = next;
    if (reason.has_value())
      last_error = std::move(*reason);
  }

  Link link{Link::stopped};
  RetryState retry;
  std::uint64_t generation{};
  std::uint64_t connection_epoch{};
  std::optional<std::string> pending_snapshot;
  std::deque<GameBarPreferenceUpdate> preference_updates;
  std::uint64_t next_preference_request_id{1};
  std::wstring last_error;

#if defined(_MSC_VER)
  using PreferenceResponse = std::pair<
      winrt::Windows::ApplicationModel::AppService::AppServiceRequest,
      winrt::Windows::ApplicationModel::AppService::AppServiceDeferral>;

  bool apartment_initialized{};
  winrt::Windows::ApplicationModel::AppService::AppServiceConnection connection{
      nullptr};
  winrt::Windows::Foundation::IAsyncOperation<
      winrt::Windows::ApplicationModel::AppService::AppServiceConnectionStatus>
      open_operation{nullptr};
  winrt::Windows::Foundation::IAsyncOperation<
      winrt::Windows::ApplicationModel::AppService::AppServiceResponse>
      send_operation{nullptr};
  winrt::Windows::System::Threading::ThreadPoolTimer retry_timer{nullptr};
  winrt::event_token request_token{};
  winrt::event_token closed_token{};
  std::map<std::uint64_t, PreferenceResponse> preference_responses;
#endif
};

namespace detail {

inline constexpr std::size_t max_pending_preference_updates = 16;

void raise_wake_event(
    const std::shared_ptr<GameBarTransportState> &state) noexcept;
[[nodiscard]] bool
start_app_service(const std::shared_ptr<GameBarTransportState> &state);
void stop_app_service(
    const std::shared_ptr<GameBarTransportState> &state) noexcept;
[[nodiscard]] std::vector<GameBarPreferenceUpdate>
poll_app_service(const std::shared_ptr<GameBarTransportState> &state) noexcept;
void complete_preference_update(
    const std::shared_ptr<GameBarTransportState> &state,
    std::uint64_t request_id, bool accepted) noexcept;

} // namespace detail
} // namespace layerc::gamebar
