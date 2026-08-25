#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.Gaming.XboxGameBar.h>

namespace winrt::LayerC::GameBar::implementation {

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
