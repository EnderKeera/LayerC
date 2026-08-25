#include "GameBarTransport.hpp"

#include "GameBarTransportState.hpp"

#include <appmodel.h>

#include <vector>

namespace layerc::gamebar {

GameBarTransport::GameBarTransport() = default;

GameBarTransport::~GameBarTransport() { stop(); }

bool GameBarTransport::is_expected_package_identity(
    const std::wstring_view name, const std::wstring_view publisher,
    const std::wstring_view family_name, const std::uint16_t major,
    const std::uint16_t minor) noexcept {
  return name == package_name && publisher == package_publisher &&
         family_name == package_family_name && major == package_major_version &&
         minor == package_minor_version;
}

bool GameBarTransport::current_process_is_expected_package() noexcept {
  static const bool expected = [] {
    UINT32 id_length{};
    if (GetCurrentPackageId(&id_length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
      return false;
    std::vector<std::byte> id_storage(id_length);
    auto *id = reinterpret_cast<PACKAGE_ID *>(id_storage.data());
    if (GetCurrentPackageId(&id_length, reinterpret_cast<BYTE *>(id)) !=
            ERROR_SUCCESS ||
        id->name == nullptr || id->publisher == nullptr)
      return false;
    UINT32 family_length{};
    if (GetCurrentPackageFamilyName(&family_length, nullptr) !=
        ERROR_INSUFFICIENT_BUFFER)
      return false;
    std::vector<wchar_t> family(family_length);
    return GetCurrentPackageFamilyName(&family_length, family.data()) ==
               ERROR_SUCCESS &&
           is_expected_package_identity(id->name, id->publisher, family.data(),
                                        id->version.Major, id->version.Minor);
  }();
  return expected;
}

bool GameBarTransport::start() {
  if (!state_)
    state_ = std::make_shared<GameBarTransportState>();
#if defined(_MSC_VER)
  if (!current_process_is_expected_package()) {
    std::lock_guard lock(state_->mutex);
    state_->last_error =
        L"Game Bar App Service is unavailable outside the full MSIX";
    return false;
  }
#endif
  return detail::start_app_service(state_);
}

void GameBarTransport::stop() noexcept {
  if (state_)
    detail::stop_app_service(state_);
}

std::vector<GameBarPreferenceUpdate> GameBarTransport::poll() noexcept {
  return state_ ? detail::poll_app_service(state_)
                : std::vector<GameBarPreferenceUpdate>{};
}

bool GameBarTransport::publish(const std::string_view snapshot_json) {
  if (!state_ || snapshot_json.empty() ||
      snapshot_json.size() > layerc::core::max_companion_snapshot_bytes)
    return false;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->started)
      return false;
    state_->pending_snapshot = std::string(snapshot_json);
  }
  detail::signal(state_);
  return true;
}

bool GameBarTransport::connected() const noexcept {
  if (!state_)
    return false;
  std::lock_guard lock(state_->mutex);
  return state_->connected;
}

HANDLE GameBarTransport::wait_handle() const noexcept {
  if (!state_)
    return nullptr;
  std::lock_guard lock(state_->mutex);
  if (!state_->started || state_->wake_event == nullptr)
    return nullptr;
  return state_->wake_event->handle;
}

std::wstring GameBarTransport::last_error() const {
  if (!state_)
    return {};
  std::lock_guard lock(state_->mutex);
  return state_->last_error;
}

std::size_t GameBarTransport::resource_handle_count() const noexcept {
  if (!state_)
    return 0U;
  std::lock_guard lock(state_->mutex);
  return state_->started && state_->wake_event != nullptr &&
                 state_->wake_event->handle != nullptr
             ? 1U
             : 0U;
}

void GameBarTransport::complete_preference_update(
    const std::uint64_t request_id, const bool accepted) noexcept {
  if (state_)
    detail::complete_preference_update(state_, request_id, accepted);
}

} // namespace layerc::gamebar
