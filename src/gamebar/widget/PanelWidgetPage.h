#pragma once

#include "PanelWidgetPage.g.h"
#include "WidgetSnapshotHub.hpp"

#include <winrt/Microsoft.Gaming.XboxGameBar.h>

namespace winrt::LayerC::GameBar::implementation {

struct PanelWidgetPage : PanelWidgetPageT<PanelWidgetPage> {
  PanelWidgetPage();
  ~PanelWidgetPage();

  void
  OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);
  void OnNavigatedFrom(
      Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);

private:
  void cleanup() noexcept;
  void render(std::optional<layerc::core::CompanionSessionSnapshot> snapshot);
  void apply_presentation();
  fire_and_forget
  open_settings(Microsoft::Gaming::XboxGameBar::XboxGameBarWidget const &sender,
                Windows::Foundation::IInspectable const &);

  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget widget_{nullptr};
  std::uint32_t slot_index_{};
  layerc::gamebar::widget::SnapshotHub::Subscription subscription_;
  std::optional<layerc::core::CompanionSessionSnapshot> snapshot_;
  Windows::UI::Xaml::FrameworkElement::Unloaded_revoker unloaded_;
  Windows::UI::Xaml::Window::Closed_revoker closed_;
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget::SettingsClicked_revoker settings_;
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget::RequestedThemeChanged_revoker theme_;
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget::RequestedOpacityChanged_revoker opacity_;
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget::CompactModeEnabledChanged_revoker compact_;
  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget::ClickThroughEnabledChanged_revoker click_through_;
};

} // namespace winrt::LayerC::GameBar::implementation

namespace winrt::LayerC::GameBar::factory_implementation {
struct PanelWidgetPage
    : PanelWidgetPageT<PanelWidgetPage, implementation::PanelWidgetPage> {};
} // namespace winrt::LayerC::GameBar::factory_implementation
