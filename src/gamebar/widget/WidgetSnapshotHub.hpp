#pragma once

#include "layerc/core/companion.hpp"

#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.Threading.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::gamebar::widget {

class SnapshotHub final {
public:
  using Snapshot = layerc::core::CompanionSessionSnapshot;
  using Callback = std::function<void(std::optional<Snapshot>)>;

  static SnapshotHub &instance();

  [[nodiscard]] bool accept_snapshot_json(std::string_view json,
                                          std::string &error);
  [[nodiscard]] std::uint64_t subscribe(Callback callback);
  void unsubscribe(std::uint64_t token) noexcept;

  void set_app_service(
      winrt::Windows::ApplicationModel::AppService::AppServiceConnection
          connection);
  void clear_app_service() noexcept;
  [[nodiscard]] winrt::Windows::Foundation::IAsyncOperation<bool>
  save_preferences(layerc::core::CompanionProfile profile);

private:
  SnapshotHub() = default;
  void publish(Snapshot snapshot);
  void expire(std::uint64_t generation);

  static constexpr auto stale_after = std::chrono::seconds(2);
  std::mutex mutex_;
  std::optional<Snapshot> latest_;
  std::chrono::steady_clock::time_point received_at_{};
  std::map<std::uint64_t, Callback> callbacks_;
  std::uint64_t next_token_{1};
  std::uint64_t generation_{};
  winrt::Windows::System::Threading::ThreadPoolTimer stale_timer_{nullptr};
  winrt::Windows::ApplicationModel::AppService::AppServiceConnection
      app_service_{nullptr};
};

// Deliver a subscription on the page's dispatcher without keeping the page
// alive after navigation. Both page types use the same snapshot handoff.
template <typename Weak, typename Render>
[[nodiscard]] std::uint64_t subscribe_on_dispatcher(
    winrt::Windows::UI::Core::CoreDispatcher dispatcher, Weak weak,
    Render render) {
  return SnapshotHub::instance().subscribe(
      [dispatcher, weak, render](std::optional<SnapshotHub::Snapshot> snapshot) {
        (void)dispatcher.RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [weak, render, snapshot = std::move(snapshot)]() mutable {
              if (const auto self = weak.get())
                render(self, std::move(snapshot));
            });
      });
}

} // namespace layerc::gamebar::widget
