#pragma once

#include "WidgetSnapshotState.hpp"

#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.ApplicationModel.Background.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.Threading.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace layerc::gamebar::widget {

class AppServiceSession;

class SnapshotHub final {
public:
  using State = SnapshotState<AppServiceSession>;
  using Snapshot = State::Snapshot;
  using Subscription = State::Subscription;
  using Callback = State::Callback;

  static SnapshotHub &instance();
  ~SnapshotHub();

  void connect(
      winrt::Windows::ApplicationModel::Background::IBackgroundTaskInstance task,
      winrt::Windows::ApplicationModel::AppService::AppServiceConnection connection);
  void disconnect(const std::shared_ptr<AppServiceSession> &session) noexcept;
  [[nodiscard]] bool accept_snapshot_json(
      const std::shared_ptr<AppServiceSession> &session, std::string_view json,
      std::string &error);
  [[nodiscard]] Subscription subscribe(Callback callback);
  [[nodiscard]] std::optional<Snapshot> current() const;
  [[nodiscard]] winrt::Windows::Foundation::IAsyncOperation<bool>
  save_preferences(layerc::core::CompanionProfile profile);

private:
  SnapshotHub() = default;
  void arm_timer(const State::Publication &publication);
  void cancel_timer(std::uint64_t revision) noexcept;
  void expire(std::uint64_t revision);

  State state_;
  std::mutex timer_mutex_;
  std::uint64_t timer_revision_{};
  winrt::Windows::System::Threading::ThreadPoolTimer stale_timer_{nullptr};
};

// Neither the page nor its dispatcher is retained by the registration. Closing
// a subscription also invalidates already-queued work for that page lifetime.
template <typename Weak, typename Render>
[[nodiscard]] SnapshotHub::Subscription subscribe_on_dispatcher(
    winrt::Windows::UI::Core::CoreDispatcher dispatcher, Weak weak,
    Render render) {
  return SnapshotHub::instance().subscribe(latest_state_callback(
      [] { return SnapshotHub::instance().current(); },
      [dispatcher = winrt::make_weak(dispatcher)](auto work) {
        if (const auto current_dispatcher = dispatcher.get())
          (void)current_dispatcher.RunAsync(
              winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
              std::move(work));
      },
      [weak] { return static_cast<bool>(weak.get()); },
      [weak, render](auto snapshot) {
        if (const auto self = weak.get())
          render(self, std::move(snapshot));
      }));
}

} // namespace layerc::gamebar::widget
