#pragma once

#include "SettingsWidgetPage.g.h"
#include "WidgetSnapshotHub.hpp"

namespace winrt::LayerC::GameBar::implementation {

struct SettingsWidgetPage : SettingsWidgetPageT<SettingsWidgetPage> {
  SettingsWidgetPage();

  void
  OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);
  void OnNavigatedFrom(
      Windows::UI::Xaml::Navigation::NavigationEventArgs const &args);
  void SaveClicked(Windows::Foundation::IInspectable const &,
                   Windows::UI::Xaml::RoutedEventArgs const &);

private:
  void render(std::optional<layerc::core::CompanionSessionSnapshot> snapshot);
  fire_and_forget save();

  std::uint64_t subscription_{};
  bool save_pending_{};
  std::optional<layerc::core::CompanionSessionSnapshot> snapshot_;
  std::vector<std::string> panel_ids_;
  std::vector<Windows::UI::Xaml::Controls::ComboBox> panel_choices_;
  hstring profile_id_;
  std::vector<Windows::UI::Xaml::Controls::ToggleSwitch> click_through_choices_;
  std::vector<Windows::UI::Xaml::Controls::Slider> opacity_choices_;
};

} // namespace winrt::LayerC::GameBar::implementation

namespace winrt::LayerC::GameBar::factory_implementation {
struct SettingsWidgetPage
    : SettingsWidgetPageT<SettingsWidgetPage,
                          implementation::SettingsWidgetPage> {};
} // namespace winrt::LayerC::GameBar::factory_implementation
