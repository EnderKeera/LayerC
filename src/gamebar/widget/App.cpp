#include "pch.h"

#include "App.h"
#include "PanelWidgetPage.h"
#include "SettingsWidgetPage.h"
#include "WidgetSnapshotHub.hpp"

namespace winrt::LayerC::GameBar::implementation {
namespace {

constexpr std::wstring_view app_service_name = L"LayerC.GameBar.v1";
constexpr std::wstring_view settings_extension = L"Settings";

} // namespace

App::App() {
  InitializeComponent();
  Suspending([weak = get_weak()](const auto &, const auto &) {
    if (const auto self = weak.get())
      self->clear_widgets();
  });
}

App::~App() { clear_widgets(); }

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
  auto window = std::make_shared<WidgetWindow>();
  window->frame = frame;
  window->widget = widget;
  window->dispatcher = Window::Current().Dispatcher();

  if (is_panel_extension(extension_id)) {
    frame.Navigate(xaml_typename<LayerC::GameBar::PanelWidgetPage>(), widget);
  } else {
    frame.Navigate(xaml_typename<LayerC::GameBar::SettingsWidgetPage>(), widget);
  }

  window->closed = Window::Current().Closed(
      auto_revoke, [weak = get_weak(), extension_id,
                    owner = std::weak_ptr<WidgetWindow>(window)](const auto &,
                                                                const auto &) {
        if (const auto self = weak.get())
          self->close_widget(extension_id, owner.lock());
      });
  std::shared_ptr<WidgetWindow> retired;
  {
    std::lock_guard lock(widgets_mutex_);
    auto &current = widgets_[extension_id];
    retired = std::exchange(current, std::move(window));
  }
  if (retired)
    retired->close();
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

  layerc::gamebar::widget::SnapshotHub::instance().connect(
      args.TaskInstance(), details.AppServiceConnection());
}

void App::WidgetWindow::close() noexcept {
  closed.revoke();
  try {
    if (dispatcher.HasThreadAccess())
      frame.Content(nullptr);
    else
      (void)dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                                [frame = frame] { frame.Content(nullptr); });
  } catch (...) {
  }
}

void App::close_widget(std::wstring const &extension_id,
                       const std::shared_ptr<WidgetWindow> &owner) {
  std::shared_ptr<WidgetWindow> retired;
  {
    std::lock_guard lock(widgets_mutex_);
    const auto found = widgets_.find(extension_id);
    if (found == widgets_.end() || found->second != owner)
      return;
    retired = std::move(found->second);
    widgets_.erase(found);
  }
  retired->close();
}

void App::clear_widgets() noexcept {
  decltype(widgets_) retired;
  {
    std::lock_guard lock(widgets_mutex_);
    retired.swap(widgets_);
  }
  for (const auto &[id, window] : retired) {
    (void)id;
    window->close();
  }
}

void App::navigation_failed(
    Windows::Foundation::IInspectable const &,
    Windows::UI::Xaml::Navigation::NavigationFailedEventArgs const &args) {
  throw hresult_error(E_FAIL, L"Failed to load " + args.SourcePageType().Name);
}

} // namespace winrt::LayerC::GameBar::implementation
