#include "pch.h"

#include "WidgetSnapshotHub.hpp"

namespace layerc::gamebar::widget {

SnapshotHub &SnapshotHub::instance() {
  static SnapshotHub hub;
  return hub;
}

bool SnapshotHub::accept_snapshot_json(const std::string_view json,
                                       std::string &error) {
  const auto parsed = layerc::core::parse_companion_snapshot_json(json);
  const auto *snapshot = std::get_if<Snapshot>(&parsed);
  if (snapshot == nullptr) {
    error = std::get<layerc::core::CompanionError>(parsed).message;
    return false;
  }
  error.clear();
  publish(*snapshot);
  return true;
}

std::uint64_t SnapshotHub::subscribe(Callback callback) {
  std::optional<Snapshot> current;
  std::uint64_t token{};
  {
    std::lock_guard lock(mutex_);
    token = next_token_++;
    callbacks_.emplace(token, callback);
    if (latest_.has_value() &&
        std::chrono::steady_clock::now() - received_at_ <= stale_after)
      current = latest_;
  }
  callback(std::move(current));
  return token;
}

void SnapshotHub::unsubscribe(const std::uint64_t token) noexcept {
  std::lock_guard lock(mutex_);
  callbacks_.erase(token);
}

void SnapshotHub::set_app_service(
    winrt::Windows::ApplicationModel::AppService::AppServiceConnection
        connection) {
  std::lock_guard lock(mutex_);
  app_service_ = std::move(connection);
}

void SnapshotHub::clear_app_service() noexcept {
  std::lock_guard lock(mutex_);
  app_service_ = nullptr;
}

winrt::Windows::Foundation::IAsyncOperation<bool>
SnapshotHub::save_preferences(layerc::core::CompanionProfile profile) {
  winrt::Windows::ApplicationModel::AppService::AppServiceConnection connection{
      nullptr};
  {
    std::lock_guard lock(mutex_);
    connection = app_service_;
  }
  if (connection == nullptr)
    co_return false;

  try {
    winrt::Windows::Foundation::Collections::ValueSet message;
    message.Insert(L"kind", winrt::box_value(L"preferences"));
    message.Insert(
        L"json", winrt::box_value(winrt::to_hstring(
                     layerc::core::serialize_companion_profile_json(profile))));
    const auto response = co_await connection.SendMessageAsync(message);
    if (response.Status() != winrt::Windows::ApplicationModel::AppService::
                                 AppServiceResponseStatus::Success)
      co_return false;
    co_return winrt::unbox_value_or<bool>(
        response.Message().TryLookup(L"accepted"), false);
  } catch (...) {
    co_return false;
  }
}

void SnapshotHub::publish(Snapshot snapshot) {
  std::vector<Callback> callbacks;
  std::uint64_t generation{};
  std::optional<Snapshot> current;
  {
    std::lock_guard lock(mutex_);
    latest_ = std::move(snapshot);
    received_at_ = std::chrono::steady_clock::now();
    generation = ++generation_;
    current = latest_;
    callbacks.reserve(callbacks_.size());
    for (const auto &[token, callback] : callbacks_) {
      (void)token;
      callbacks.push_back(callback);
    }
    if (stale_timer_ != nullptr)
      stale_timer_.Cancel();
    stale_timer_ =
        winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
            [generation](const auto &) {
              SnapshotHub::instance().expire(generation);
            },
            stale_after);
  }
  for (const auto &callback : callbacks)
    callback(current);
}

void SnapshotHub::expire(const std::uint64_t generation) {
  std::vector<Callback> callbacks;
  {
    std::lock_guard lock(mutex_);
    if (generation != generation_ || !latest_.has_value())
      return;
    latest_.reset();
    stale_timer_ = nullptr;
    callbacks.reserve(callbacks_.size());
    for (const auto &[token, callback] : callbacks_) {
      (void)token;
      callbacks.push_back(callback);
    }
  }
  for (const auto &callback : callbacks)
    callback(std::nullopt);
}

} // namespace layerc::gamebar::widget
