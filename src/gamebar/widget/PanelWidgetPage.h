#pragma once

#include "PanelWidgetPage.g.h"
#include "WidgetSnapshotHub.hpp"

#include <winrt/Microsoft.Gaming.XboxGameBar.h>

namespace winrt::LayerC::GameBar::implementation {

struct PanelWidgetPage : PanelWidgetPageT<PanelWidgetPage> {
  PanelWidgetPage();

  void
  OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);
  void OnNavigatedFrom(
      Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);

private:
  void render(std::optional<layerc::core::CompanionSessionSnapshot> snapshot);
  void apply_presentation();
  fire_and_forget
  open_settings(Microsoft::Gaming::XboxGameBar::XboxGameBarWidget const &sender,
                Windows::Foundation::IInspectable const &);

  Microsoft::Gaming::XboxGameBar::XboxGameBarWidget widget_{nullptr};
  std::uint32_t slot_index_{};
  std::uint64_t subscription_{};
  std::optional<layerc::core::CompanionSessionSnapshot> snapshot_;
  event_token settings_token_{};
  event_token theme_token_{};
  event_token opacity_token_{};
  event_token compact_token_{};
  event_token click_through_token_{};
};

} // namespace winrt::LayerC::GameBar::implementation

namespace winrt::LayerC::GameBar::factory_implementation {
struct PanelWidgetPage
    : PanelWidgetPageT<PanelWidgetPage, implementation::PanelWidgetPage> {};
} // namespace winrt::LayerC::GameBar::factory_implementation
