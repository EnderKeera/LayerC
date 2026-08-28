#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.Gaming.XboxGameBar.h>

#include <optional>
#include <string>
#include <string_view>

namespace winrt::LayerC::GameBar::implementation {

// Extension ids are "PanelSlot<1..8>" (bounds duplicated in the manifest) or
// "Settings". is_panel_extension is the id predicate; panel_slot maps a
// panel extension to its zero-based slot index.
inline bool is_panel_extension(const std::wstring_view id) {
  constexpr std::wstring_view panel_prefix = L"PanelSlot";
  return id.starts_with(panel_prefix) &&
         id.size() == panel_prefix.size() + 1U && id.back() >= L'1' &&
         id.back() <= L'8';
}

inline std::optional<std::uint32_t> panel_slot(const std::wstring_view id) {
  if (!is_panel_extension(id))
    return std::nullopt;
  return static_cast<std::uint32_t>(id.back() - L'1');
}

struct App : AppT<App> {
  App();

  void OnActivated(
      Windows::ApplicationModel::Activation::IActivatedEventArgs const &args);
  void OnBackgroundActivated(
      Windows::ApplicationModel::Activation::BackgroundActivatedEventArgs const
          &args);

private:
  void close_widget(std::wstring const &extension_id);
  void navigation_failed(
      Windows::Foundation::IInspectable const &,
      Windows::UI::Xaml::Navigation::NavigationFailedEventArgs const &args);
  fire_and_forget ensure_daemon();
  fire_and_forget service_request_received(
      Windows::ApplicationModel::AppService::AppServiceConnection const &,
      Windows::ApplicationModel::AppService::
          AppServiceRequestReceivedEventArgs const &args);
  void service_closed(
      Windows::ApplicationModel::AppService::AppServiceConnection const &,
      Windows::ApplicationModel::AppService::AppServiceClosedEventArgs const &);
  void service_canceled(
      Windows::ApplicationModel::Background::IBackgroundTaskInstance const &,
      Windows::ApplicationModel::Background::BackgroundTaskCancellationReason);
  void complete_service() noexcept;

  std::map<std::wstring, Microsoft::Gaming::XboxGameBar::XboxGameBarWidget,
           std::less<>>
      widgets_;
  std::map<std::wstring, event_token, std::less<>> close_tokens_;
  Windows::ApplicationModel::AppService::AppServiceConnection app_service_{
      nullptr};
  Windows::ApplicationModel::Background::BackgroundTaskDeferral
      service_deferral_{nullptr};
  event_token service_request_token_{};
  event_token service_closed_token_{};
  event_token service_canceled_token_{};
};

} // namespace winrt::LayerC::GameBar::implementation
