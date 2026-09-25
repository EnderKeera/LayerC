#include "GameBarTransportState.hpp"

#include <roapi.h>

#include <utility>

#if defined(_MSC_VER)
#include <winrt/Windows.Foundation.Collections.h>
#endif

namespace layerc::gamebar::detail {
namespace {

#if defined(_MSC_VER)
using namespace winrt::Windows::ApplicationModel::AppService;
using Link = GameBarTransportState::Link;

[[nodiscard]] bool connected(const GameBarTransportState &state) noexcept {
  return state.link == Link::connected || state.link == Link::sending;
}

struct ClientResources {
  AppServiceConnection connection{nullptr};
  winrt::Windows::System::Threading::ThreadPoolTimer retry_timer{nullptr};
  decltype(GameBarTransportState::open_operation) open_operation{nullptr};
  decltype(GameBarTransportState::send_operation) send_operation{nullptr};
  winrt::event_token request_token{};
  winrt::event_token closed_token{};
  std::vector<GameBarTransportState::PreferenceResponse> pending_responses;
};

// Generation is bumped on every start/stop; connection_epoch is bumped on
// every open. Callbacks must ignore completions from a torn-down cycle.
[[nodiscard]] bool
is_active_generation(const GameBarTransportState &state,
                     const std::uint64_t generation) noexcept {
  return state.link != Link::stopped && state.generation == generation;
}

[[nodiscard]] bool
is_active_connection(const GameBarTransportState &state,
                     const std::uint64_t generation,
                     const std::uint64_t connection_epoch) noexcept {
  return is_active_generation(state, generation) &&
         state.connection_epoch == connection_epoch;
}

std::wstring hresult_message(const winrt::hresult code) {
  try {
    return std::wstring(winrt::hresult_error(code).message());
  } catch (...) {
    return L"Game Bar App Service operation failed";
  }
}

std::wstring open_failure_message(const AppServiceConnectionStatus result,
                                  const winrt::hresult error_code) {
  if (result == AppServiceConnectionStatus::Unknown)
    return hresult_message(error_code);
  return L"Game Bar App Service open failed (status " +
         std::to_wstring(static_cast<unsigned>(result)) + L")";
}

winrt::fire_and_forget send_preference_response(AppServiceRequest request,
                                                AppServiceDeferral deferral,
                                                const bool accepted) {
  try {
    winrt::Windows::Foundation::Collections::ValueSet response;
    response.Insert(L"accepted", winrt::box_value(accepted));
    co_await request.SendResponseAsync(response);
  } catch (...) {
  }
  deferral.Complete();
}

ClientResources release_client_locked(GameBarTransportState &state) {
  ClientResources resources;
  resources.connection = std::move(state.connection);
  resources.retry_timer = std::move(state.retry_timer);
  resources.open_operation = std::move(state.open_operation);
  resources.send_operation = std::move(state.send_operation);
  resources.request_token = std::exchange(state.request_token, {});
  resources.closed_token = std::exchange(state.closed_token, {});
  resources.pending_responses.reserve(state.preference_responses.size());
  for (auto &entry : state.preference_responses)
    resources.pending_responses.push_back(std::move(entry.second));
  state.preference_responses.clear();
  if (state.link != Link::stopped)
    state.enter(Link::idle);
  return resources;
}

void cleanup_client(ClientResources resources) noexcept {
  try {
    if (resources.retry_timer != nullptr)
      resources.retry_timer.Cancel();
    if (resources.open_operation != nullptr)
      resources.open_operation.Cancel();
    if (resources.send_operation != nullptr)
      resources.send_operation.Cancel();
    if (resources.connection != nullptr) {
      resources.connection.RequestReceived(resources.request_token);
      resources.connection.ServiceClosed(resources.closed_token);
    }
  } catch (...) {
  }
  for (auto &response : resources.pending_responses)
    send_preference_response(std::move(response.first),
                             std::move(response.second), false);
}

[[nodiscard]] bool queue_preference_update(
    const std::weak_ptr<GameBarTransportState> &weak_state,
    const std::uint64_t generation, const std::uint64_t connection_epoch,
    std::string utf8, AppServiceRequest request, AppServiceDeferral deferral) {
  const auto state = weak_state.lock();
  if (!state)
    return false;
  {
    std::lock_guard lock(state->mutex);
    if (!connected(*state) ||
        !is_active_connection(*state, generation, connection_epoch) ||
        state->preference_updates.size() >= max_pending_preference_updates)
      return false;
    const auto request_id = state->next_preference_request_id++;
    state->preference_updates.push_back(
        {.request_id = request_id, .json = std::move(utf8)});
    state->preference_responses.emplace(
        request_id, std::pair{std::move(request), std::move(deferral)});
  }
  raise_wake_event(state);
  return true;
}

void handle_preference_request(std::weak_ptr<GameBarTransportState> weak_state,
                               const std::uint64_t generation,
                               const std::uint64_t connection_epoch,
                               AppServiceRequestReceivedEventArgs args) {
  const auto deferral = args.GetDeferral();
  try {
    const auto message = args.Request().Message();
    const auto kind =
        winrt::unbox_value_or<winrt::hstring>(message.TryLookup(L"kind"), L"");
    const auto json =
        winrt::unbox_value_or<winrt::hstring>(message.TryLookup(L"json"), L"");
    const auto utf8 = winrt::to_string(json);
    if (kind == L"preferences" && !utf8.empty() &&
        utf8.size() <= layerc::core::max_companion_sidecar_bytes &&
        queue_preference_update(weak_state, generation, connection_epoch, utf8,
                                args.Request(), deferral))
      return;
  } catch (...) {
  }
  send_preference_response(args.Request(), deferral, false);
}

void handle_service_closed(
    const std::shared_ptr<GameBarTransportState> &state,
    const std::uint64_t generation, const std::uint64_t connection_epoch,
    const AppServiceClosedStatus status) {
  {
    std::lock_guard lock(state->mutex);
    if (!is_active_connection(*state, generation, connection_epoch))
      return;
    state->retry.needed = true;
    ++state->connection_epoch; // Ignore a send completion from the closed link.
    state->enter(Link::idle, L"Game Bar App Service closed (status " +
                                 std::to_wstring(static_cast<unsigned>(status)) + L")");
  }
  raise_wake_event(state);
}

void apply_open_result(const std::shared_ptr<GameBarTransportState> &state,
                       const std::uint64_t generation,
                       const std::uint64_t connection_epoch,
                       const AppServiceConnectionStatus result,
                       const winrt::hresult error_code) {
  {
    std::lock_guard lock(state->mutex);
    if (!is_active_connection(*state, generation, connection_epoch))
      return;
    state->open_operation = nullptr;
    if (result == AppServiceConnectionStatus::Success) {
      state->retry.delay_ms = 100;
      state->enter(Link::connected, L"");
    } else {
      state->retry.needed = true;
      state->enter(Link::idle, open_failure_message(result, error_code));
    }
  }
  raise_wake_event(state);
}

// A WinRT call on the active link threw: drop to idle and schedule a retry.
// A superseded generation or connection epoch is left untouched.
void fail_active_connection(const std::shared_ptr<GameBarTransportState> &state,
                            const std::uint64_t generation,
                            const std::uint64_t connection_epoch,
                            std::wstring message) {
  {
    std::lock_guard lock(state->mutex);
    if (is_active_connection(*state, generation, connection_epoch)) {
      state->retry.needed = true;
      state->enter(Link::idle, std::move(message));
    }
  }
  raise_wake_event(state);
}

void begin_open(const std::shared_ptr<GameBarTransportState> &state) {
  AppServiceConnection connection{nullptr};
  winrt::Windows::Foundation::IAsyncOperation<AppServiceConnectionStatus>
      operation{nullptr};
  std::uint64_t generation{};
  std::uint64_t connection_epoch{};
  try {
    {
      std::lock_guard lock(state->mutex);
      if (state->link != Link::idle)
        return;
      generation = state->generation;
      connection_epoch = ++state->connection_epoch;
      connection = AppServiceConnection();
      connection.AppServiceName(
          winrt::hstring(GameBarTransport::app_service_name));
      connection.PackageFamilyName(
          winrt::hstring(GameBarTransport::package_family_name));
      state->request_token = connection.RequestReceived(
          [weak_state = std::weak_ptr(state), generation,
           connection_epoch](const auto &, const auto &args) {
            handle_preference_request(weak_state, generation, connection_epoch,
                                      args);
          });
      state->closed_token = connection.ServiceClosed(
          [weak_state = std::weak_ptr(state), generation,
           connection_epoch](const auto &, const auto &args) {
            const auto locked = weak_state.lock();
            if (!locked)
              return;
            handle_service_closed(locked, generation, connection_epoch,
                                  args.Status());
          });
      operation = connection.OpenAsync();
      state->connection = connection;
      state->open_operation = operation;
      state->enter(Link::opening);
    }
    operation.Completed([weak_state = std::weak_ptr(state), generation,
                         connection_epoch](const auto &completed,
                                           const auto status) {
      const auto locked = weak_state.lock();
      if (!locked)
        return;
      AppServiceConnectionStatus result = AppServiceConnectionStatus::Unknown;
      winrt::hresult error_code{E_FAIL};
      if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
        try {
          result = completed.GetResults();
        } catch (const winrt::hresult_error &error) {
          error_code = error.code();
        }
      }
      apply_open_result(locked, generation, connection_epoch, result,
                        error_code);
    });
  } catch (const winrt::hresult_error &error) {
    fail_active_connection(state, generation, connection_epoch,
                           std::wstring(error.message()));
  }
}

void apply_send_result(const std::shared_ptr<GameBarTransportState> &state,
                       const std::uint64_t generation,
                       const std::uint64_t connection_epoch,
                       const bool transport_ok, const bool accepted,
                       std::wstring error_text) {
  {
    std::lock_guard lock(state->mutex);
    if (!is_active_connection(*state, generation, connection_epoch))
      return;
    state->send_operation = nullptr;
    if (!transport_ok) {
      state->retry.needed = true;
      state->enter(Link::idle, error_text.empty()
                                   ? L"Game Bar snapshot send failed"
                                   : std::move(error_text));
    } else if (!accepted) {
      state->enter(Link::connected, L"Game Bar widget rejected a snapshot");
    } else {
      state->enter(Link::connected, L"");
    }
  }
  raise_wake_event(state);
}

void begin_send(const std::shared_ptr<GameBarTransportState> &state) {
  AppServiceConnection connection{nullptr};
  std::string payload;
  std::uint64_t generation{};
  std::uint64_t connection_epoch{};
  {
    std::lock_guard lock(state->mutex);
    if (state->link != Link::connected ||
        !state->pending_snapshot.has_value())
      return;
    generation = state->generation;
    connection_epoch = state->connection_epoch;
    connection = state->connection;
    payload = std::move(*state->pending_snapshot);
    state->pending_snapshot.reset();
    state->enter(Link::sending);
  }

  try {
    winrt::Windows::Foundation::Collections::ValueSet message;
    message.Insert(L"kind", winrt::box_value(L"snapshot"));
    message.Insert(L"json", winrt::box_value(winrt::to_hstring(payload)));
    const auto operation = connection.SendMessageAsync(message);
    {
      std::lock_guard lock(state->mutex);
      if (!is_active_connection(*state, generation, connection_epoch))
        return;
      state->send_operation = operation;
    }
    operation.Completed([weak_state = std::weak_ptr(state), generation,
                         connection_epoch](const auto &completed,
                                           const auto status) {
      const auto locked = weak_state.lock();
      if (!locked)
        return;
      bool transport_ok{};
      bool accepted{};
      std::wstring error_text;
      if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
        try {
          const auto response = completed.GetResults();
          transport_ok = response.Status() == AppServiceResponseStatus::Success;
          if (transport_ok)
            accepted = winrt::unbox_value_or<bool>(
                response.Message().TryLookup(L"accepted"), false);
        } catch (const winrt::hresult_error &error) {
          error_text = std::wstring(error.message());
        }
      }
      apply_send_result(locked, generation, connection_epoch, transport_ok,
                        accepted, std::move(error_text));
    });
  } catch (const winrt::hresult_error &error) {
    fail_active_connection(state, generation, connection_epoch,
                           std::wstring(error.message()));
  }
}
#endif

} // namespace

bool start_app_service(const std::shared_ptr<GameBarTransportState> &state) {
#if !defined(_MSC_VER)
  std::lock_guard lock(state->mutex);
  state->enter(GameBarTransportState::Link::stopped,
               L"Game Bar App Service requires the MSVC package build");
  return false;
#else
  {
    std::lock_guard lock(state->mutex);
    if (state->link != Link::stopped)
      return true;
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
      state->enter(Link::stopped, L"WinRT apartment initialization failed");
      return false;
    }
    state->apartment_initialized = SUCCEEDED(apartment);
    auto wake = std::make_shared<GameBarWakeEvent>();
    if (wake->handle == nullptr) {
      if (state->apartment_initialized) {
        RoUninitialize();
        state->apartment_initialized = false;
      }
      state->enter(Link::stopped, L"Game Bar wake event could not be created");
      return false;
    }
    state->wake_event = std::move(wake);
    state->enter(Link::idle);
    ++state->generation;
    state->retry.ready = true;
    state->retry.delay_ms = 100;
  }
  raise_wake_event(state);
  return true;
#endif
}

void stop_app_service(
    const std::shared_ptr<GameBarTransportState> &state) noexcept {
#if defined(_MSC_VER)
  ClientResources resources;
  bool uninitialize{};
#endif
  std::shared_ptr<GameBarWakeEvent> wake;
  {
    std::lock_guard lock(state->mutex);
    if (state->link == GameBarTransportState::Link::stopped)
      return;
    ++state->generation;
    state->retry = {};
    state->pending_snapshot.reset();
    state->preference_updates.clear();
#if defined(_MSC_VER)
    resources = release_client_locked(*state);
    uninitialize = std::exchange(state->apartment_initialized, false);
#endif
    state->enter(GameBarTransportState::Link::stopped);
    wake = std::move(state->wake_event);
  }
#if defined(_MSC_VER)
  cleanup_client(std::move(resources));
  if (uninitialize) {
    // C++/WinRT caches agile activation factories process-wide. Release them
    // before this service tears down its apartment so a later demand cycle
    // cannot reuse a pointer owned by the previous apartment.
    winrt::clear_factory_cache();
    RoUninitialize();
  }
#endif
  if (wake != nullptr && wake->handle != nullptr)
    ResetEvent(wake->handle);
}

std::vector<GameBarPreferenceUpdate>
poll_app_service(const std::shared_ptr<GameBarTransportState> &state) noexcept {
  bool open{};
  bool send{};
  std::vector<GameBarPreferenceUpdate> updates;
#if defined(_MSC_VER)
  std::optional<ClientResources> failed_resources;
#endif
  {
    std::lock_guard lock(state->mutex);
    if (state->wake_event != nullptr && state->wake_event->handle != nullptr)
      ResetEvent(state->wake_event->handle);
    if (state->link == GameBarTransportState::Link::stopped)
      return updates;
#if defined(_MSC_VER)
    if (state->retry.needed) {
      state->retry.needed = false;
      failed_resources = release_client_locked(*state);
      state->preference_updates.clear();
      schedule_retry_locked(state);
    }
#endif
    updates.reserve(state->preference_updates.size());
    while (!state->preference_updates.empty()) {
      updates.push_back(std::move(state->preference_updates.front()));
      state->preference_updates.pop_front();
    }
#if defined(_MSC_VER)
    if (state->retry.ready) {
      state->retry.ready = false;
      open = true;
    }
    send = state->link == Link::connected &&
           state->pending_snapshot.has_value();
#endif
  }
#if defined(_MSC_VER)
  if (failed_resources.has_value())
    cleanup_client(std::move(*failed_resources));
  if (open)
    begin_open(state);
  if (send)
    begin_send(state);
#else
  (void)open;
  (void)send;
#endif
  return updates;
}

void complete_preference_update(
    const std::shared_ptr<GameBarTransportState> &state,
    const std::uint64_t request_id, const bool accepted) noexcept {
#if defined(_MSC_VER)
  std::optional<GameBarTransportState::PreferenceResponse> response;
  {
    std::lock_guard lock(state->mutex);
    const auto found = state->preference_responses.find(request_id);
    if (found == state->preference_responses.end())
      return;
    response = std::move(found->second);
    state->preference_responses.erase(found);
  }
  send_preference_response(std::move(response->first),
                           std::move(response->second), accepted);
#else
  (void)state;
  (void)request_id;
  (void)accepted;
#endif
}

} // namespace layerc::gamebar::detail
