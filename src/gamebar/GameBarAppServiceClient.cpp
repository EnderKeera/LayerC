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

struct ClientResources {
  AppServiceConnection connection{nullptr};
  winrt::Windows::System::Threading::ThreadPoolTimer retry_timer{nullptr};
  decltype(GameBarTransportState::open_operation) open_operation{nullptr};
  decltype(GameBarTransportState::send_operation) send_operation{nullptr};
  winrt::event_token request_token{};
  winrt::event_token closed_token{};
  std::vector<GameBarTransportState::PreferenceResponse> responses;
};

std::wstring hresult_message(const winrt::hresult code) {
  try {
    return std::wstring(winrt::hresult_error(code).message());
  } catch (...) {
    return L"Game Bar App Service operation failed";
  }
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
  resources.responses.reserve(state.preference_responses.size());
  for (auto &[request_id, response] : state.preference_responses) {
    (void)request_id;
    resources.responses.push_back(std::move(response));
  }
  state.preference_responses.clear();
  state.connected = false;
  state.opening = false;
  state.sending = false;
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
  for (auto &response : resources.responses)
    send_preference_response(std::move(response.first),
                             std::move(response.second), false);
}

void handle_preference_request(std::weak_ptr<GameBarTransportState> weak_state,
                               const std::uint64_t generation,
                               const std::uint64_t connection_epoch,
                               AppServiceRequestReceivedEventArgs args) {
  const auto deferral = args.GetDeferral();
  bool queued{};
  try {
    const auto message = args.Request().Message();
    const auto kind =
        winrt::unbox_value_or<winrt::hstring>(message.TryLookup(L"kind"), L"");
    const auto json =
        winrt::unbox_value_or<winrt::hstring>(message.TryLookup(L"json"), L"");
    const auto utf8 = winrt::to_string(json);
    if (kind == L"preferences" && !utf8.empty() &&
        utf8.size() <= layerc::core::max_companion_sidecar_bytes) {
      if (const auto state = weak_state.lock()) {
        {
          std::lock_guard lock(state->mutex);
          if (state->started && state->connected &&
              state->generation == generation &&
              state->connection_epoch == connection_epoch &&
              state->preference_updates.size() <
                  max_pending_preference_updates) {
            const auto request_id = state->next_preference_request_id++;
            state->preference_updates.push_back(
                {.request_id = request_id, .json = utf8});
            state->preference_responses.emplace(
                request_id, std::pair{args.Request(), deferral});
            queued = true;
          }
        }
        if (queued)
          signal(state);
      }
    }
  } catch (...) {
  }
  if (!queued)
    send_preference_response(args.Request(), deferral, false);
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
      if (!state->started || state->opening || state->connected)
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
            {
              std::lock_guard lock(locked->mutex);
              if (!locked->started || locked->generation != generation ||
                  locked->connection_epoch != connection_epoch)
                return;
              locked->connected = false;
              locked->sending = false;
              locked->retry_needed = true;
              locked->last_error =
                  L"Game Bar App Service closed (status " +
                  std::to_wstring(static_cast<unsigned>(args.Status())) + L")";
            }
            signal(locked);
          });
      operation = connection.OpenAsync();
      state->connection = connection;
      state->open_operation = operation;
      state->opening = true;
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
      {
        std::lock_guard lock(locked->mutex);
        if (!locked->started || locked->generation != generation ||
            locked->connection_epoch != connection_epoch)
          return;
        locked->opening = false;
        locked->open_operation = nullptr;
        locked->connected = result == AppServiceConnectionStatus::Success;
        if (locked->connected) {
          locked->retry_ms = 100;
          locked->last_error.clear();
        } else {
          locked->retry_needed = true;
          locked->last_error =
              result == AppServiceConnectionStatus::Unknown
                  ? hresult_message(error_code)
                  : L"Game Bar App Service open failed (status " +
                        std::to_wstring(static_cast<unsigned>(result)) + L")";
        }
      }
      signal(locked);
    });
  } catch (const winrt::hresult_error &error) {
    {
      std::lock_guard lock(state->mutex);
      state->opening = false;
      state->connected = false;
      state->retry_needed = true;
      state->last_error = std::wstring(error.message());
    }
    signal(state);
  }
}

void begin_send(const std::shared_ptr<GameBarTransportState> &state) {
  AppServiceConnection connection{nullptr};
  std::string payload;
  std::uint64_t generation{};
  std::uint64_t connection_epoch{};
  {
    std::lock_guard lock(state->mutex);
    if (!state->started || !state->connected || state->sending ||
        !state->pending_snapshot.has_value())
      return;
    generation = state->generation;
    connection_epoch = state->connection_epoch;
    connection = state->connection;
    payload = std::move(*state->pending_snapshot);
    state->pending_snapshot.reset();
    state->sending = true;
  }

  try {
    winrt::Windows::Foundation::Collections::ValueSet message;
    message.Insert(L"kind", winrt::box_value(L"snapshot"));
    message.Insert(L"json", winrt::box_value(winrt::to_hstring(payload)));
    const auto operation = connection.SendMessageAsync(message);
    {
      std::lock_guard lock(state->mutex);
      if (!state->started || state->generation != generation ||
          state->connection_epoch != connection_epoch)
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
      {
        std::lock_guard lock(locked->mutex);
        if (!locked->started || locked->generation != generation ||
            locked->connection_epoch != connection_epoch)
          return;
        locked->sending = false;
        locked->send_operation = nullptr;
        if (!transport_ok) {
          locked->connected = false;
          locked->retry_needed = true;
          locked->last_error = error_text.empty()
                                   ? L"Game Bar snapshot send failed"
                                   : std::move(error_text);
        } else if (!accepted) {
          locked->last_error = L"Game Bar widget rejected a snapshot";
        } else {
          locked->last_error.clear();
        }
      }
      signal(locked);
    });
  } catch (const winrt::hresult_error &error) {
    {
      std::lock_guard lock(state->mutex);
      if (state->started && state->generation == generation &&
          state->connection_epoch == connection_epoch) {
        state->sending = false;
        state->connected = false;
        state->retry_needed = true;
        state->last_error = std::wstring(error.message());
      }
    }
    signal(state);
  }
}
#endif

} // namespace

bool start_app_service(const std::shared_ptr<GameBarTransportState> &state) {
#if !defined(_MSC_VER)
  std::lock_guard lock(state->mutex);
  state->last_error = L"Game Bar App Service requires the MSVC package build";
  return false;
#else
  {
    std::lock_guard lock(state->mutex);
    if (state->started)
      return true;
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
      state->last_error = L"WinRT apartment initialization failed";
      return false;
    }
    state->apartment_initialized = SUCCEEDED(apartment);
    auto wake = std::make_shared<GameBarWakeEvent>();
    if (wake->handle == nullptr) {
      if (state->apartment_initialized) {
        RoUninitialize();
        state->apartment_initialized = false;
      }
      state->last_error = L"Game Bar wake event could not be created";
      return false;
    }
    state->wake_event = std::move(wake);
    state->started = true;
    ++state->generation;
    state->retry_ready = true;
    state->retry_ms = 100;
  }
  signal(state);
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
    if (!state->started)
      return;
    state->started = false;
    ++state->generation;
    state->retry_ready = false;
    state->retry_needed = false;
    state->pending_snapshot.reset();
    state->preference_updates.clear();
#if defined(_MSC_VER)
    resources = release_client_locked(*state);
    uninitialize = std::exchange(state->apartment_initialized, false);
#else
    state->connected = false;
    state->opening = false;
    state->sending = false;
#endif
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
    if (!state->started)
      return updates;
#if defined(_MSC_VER)
    if (state->retry_needed) {
      state->retry_needed = false;
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
    if (state->retry_ready) {
      state->retry_ready = false;
      open = true;
    }
    send = state->connected && !state->sending &&
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
