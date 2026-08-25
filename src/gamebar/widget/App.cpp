#include "pch.h"

#include "App.h"
#include "PanelWidgetPage.h"
#include "SettingsWidgetPage.h"
#include "WidgetSnapshotHub.hpp"

namespace winrt::LayerC::GameBar::implementation {
namespace {

constexpr std::wstring_view app_service_name = L"LayerC.GameBar.v1";
constexpr std::wstring_view settings_extension = L"Settings";
constexpr std::wstring_view panel_prefix = L"PanelSlot";

bool is_panel_extension(const std::wstring_view id) {
  return id.starts_with(panel_prefix) &&
         id.size() == panel_prefix.size() + 1U && id.back() >= L'1' &&
         id.back() <= L'8';
}

} // namespace

App::App() {
  InitializeComponent();
  Suspending([weak = get_weak()](const auto &, const auto &) {
    if (const auto self = weak.get()) {
      self->widgets_.clear();
      self->close_tokens_.clear();
    }
  });
}

void App::OnActivated(
    Windows::ApplicationModel::Activation::IActivatedEventArgs const &args) {
  using namespace Windows::ApplicationModel::Activation;
  using namespace Windows::UI::Xaml;
  using namespace Windows::UI::Xaml::Controls;

  if (args.Kind() != ActivationKind::Protocol)
    return;
  const auto protocol = args.try_as<IProtocolActivatedEventArgs>();
  if (protocol == nullptr || protocol.Uri().SchemeName() != L"ms-gamebarwidget")
    return;
  const auto widget_args = args.try_as<
      Microsoft::Gaming::XboxGameBar::XboxGameBarWidgetActivatedEventArgs>();
  if (widget_args == nullptr)
    return;

  const std::wstring extension_id(widget_args.AppExtensionId());
  if (!is_panel_extension(extension_id) && extension_id != settings_extension)
    return;
  if (!widget_args.IsLaunchActivation())
    return;
  ensure_daemon();

  Frame frame;
  frame.NavigationFailed({this, &App::navigation_failed});
  Window::Current().Content(frame);
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget widget(
      widget_args, Window::Current().CoreWindow(), frame);
  widgets_.insert_or_assign(extension_id, widget);

  if (is_panel_extension(extension_id)) {
    frame.Navigate(xaml_typename<LayerC::GameBar::PanelWidgetPage>(), widget);
  } else {
    frame.Navigate(xaml_typename<LayerC::GameBar::SettingsWidgetPage>(),
                   widget);
  }

  close_tokens_.insert_or_assign(
      extension_id, Window::Current().Closed([weak = get_weak(), extension_id](
                                                 const auto &, const auto &) {
        if (const auto self = weak.get())
          self->close_widget(extension_id);
      }));
  Window::Current().Activate();
}

fire_and_forget App::ensure_daemon() {
  try {
    co_await Windows::ApplicationModel::FullTrustProcessLauncher::
        LaunchFullTrustProcessForCurrentAppAsync();
  } catch (...) {
  }
}

void App::OnBackgroundActivated(
    Windows::ApplicationModel::Activation::BackgroundActivatedEventArgs const
        &args) {
  using namespace Windows::ApplicationModel;
  using namespace Windows::ApplicationModel::AppService;

  const auto details =
      args.TaskInstance().TriggerDetails().try_as<AppServiceTriggerDetails>();
  if (details == nullptr || details.Name() != app_service_name ||
      details.CallerPackageFamilyName() != Package::Current().Id().FamilyName())
    return;

  complete_service();
  service_deferral_ = args.TaskInstance().GetDeferral();
  app_service_ = details.AppServiceConnection();
  service_request_token_ =
      app_service_.RequestReceived({this, &App::service_request_received});
  service_closed_token_ =
      app_service_.ServiceClosed({this, &App::service_closed});
  service_canceled_token_ =
      args.TaskInstance().Canceled({this, &App::service_canceled});
  layerc::gamebar::widget::SnapshotHub::instance().set_app_service(
      app_service_);
}

void App::close_widget(std::wstring const &extension_id) {
  const auto token = close_tokens_.find(extension_id);
  if (token != close_tokens_.end()) {
    Windows::UI::Xaml::Window::Current().Closed(token->second);
    close_tokens_.erase(token);
  }
  widgets_.erase(extension_id);
}

void App::navigation_failed(
    Windows::Foundation::IInspectable const &,
    Windows::UI::Xaml::Navigation::NavigationFailedEventArgs const &args) {
  throw hresult_error(E_FAIL, L"Failed to load " + args.SourcePageType().Name);
}

fire_and_forget App::service_request_received(
    Windows::ApplicationModel::AppService::AppServiceConnection const &,
    Windows::ApplicationModel::AppService::
        AppServiceRequestReceivedEventArgs const &args) {
  auto lifetime = get_strong();
  const auto deferral = args.GetDeferral();
  Windows::Foundation::Collections::ValueSet response;
  bool accepted{};
  std::string error;
  try {
    const auto message = args.Request().Message();
    const auto kind =
        winrt::unbox_value_or<hstring>(message.TryLookup(L"kind"), L"");
    const auto payload =
        winrt::unbox_value_or<hstring>(message.TryLookup(L"json"), L"");
    if (kind != L"snapshot") {
      error = "unsupported companion message";
    } else {
      const auto utf8 = winrt::to_string(payload);
      if (utf8.size() > layerc::core::max_companion_snapshot_bytes)
        error = "companion snapshot exceeds bounded message size";
      else
        accepted = layerc::gamebar::widget::SnapshotHub::instance()
                       .accept_snapshot_json(utf8, error);
    }
    response.Insert(L"accepted", winrt::box_value(accepted));
    if (!error.empty())
      response.Insert(L"error", winrt::box_value(winrt::to_hstring(error)));
    co_await args.Request().SendResponseAsync(response);
  } catch (...) {
  }
  deferral.Complete();
}

void App::service_closed(
    Windows::ApplicationModel::AppService::AppServiceConnection const &,
    Windows::ApplicationModel::AppService::AppServiceClosedEventArgs const &) {
  complete_service();
}

void App::service_canceled(
    Windows::ApplicationModel::Background::IBackgroundTaskInstance const &,
    Windows::ApplicationModel::Background::BackgroundTaskCancellationReason) {
  complete_service();
}

void App::complete_service() noexcept {
  layerc::gamebar::widget::SnapshotHub::instance().clear_app_service();
  if (app_service_ != nullptr) {
    app_service_.RequestReceived(service_request_token_);
    app_service_.ServiceClosed(service_closed_token_);
    app_service_ = nullptr;
  }
  if (service_deferral_ != nullptr) {
    service_deferral_.Complete();
    service_deferral_ = nullptr;
  }
}

} // namespace winrt::LayerC::GameBar::implementation
