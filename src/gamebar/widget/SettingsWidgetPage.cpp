#include "pch.h"
#include "SettingsWidgetPage.h"
// Generated definitions require the projected page declaration above.
#include "SettingsWidgetPage.g.cpp"

#include <set>

namespace winrt::LayerC::GameBar::implementation {

SettingsWidgetPage::SettingsWidgetPage() { InitializeComponent(); }

void SettingsWidgetPage::OnNavigatedTo(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &) {
  const auto dispatcher = Dispatcher();
  subscription_ = layerc::gamebar::widget::SnapshotHub::instance().subscribe(
      [dispatcher, weak = get_weak()](
          std::optional<layerc::core::CompanionSessionSnapshot> snapshot) {
        (void)dispatcher.RunAsync(
            Windows::UI::Core::CoreDispatcherPriority::Normal,
            [weak, snapshot = std::move(snapshot)]() mutable {
              if (const auto self = weak.get())
                self->render(std::move(snapshot));
            });
      });
}

void SettingsWidgetPage::OnNavigatedFrom(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &) {
  if (subscription_ != 0) {
    layerc::gamebar::widget::SnapshotHub::instance().unsubscribe(subscription_);
    subscription_ = 0;
  }
}

void SettingsWidgetPage::render(
    std::optional<layerc::core::CompanionSessionSnapshot> snapshot) {
  const bool unchanged = snapshot_.has_value() && snapshot.has_value() &&
                         snapshot_->profile_id == snapshot->profile_id &&
                         snapshot_->available_panels == snapshot->available_panels &&
                         snapshot_->preferences == snapshot->preferences;
  snapshot_ = std::move(snapshot);
  if (unchanged)
    return;
  panel_ids_.clear();
  panel_choices_.clear();
  click_through_choices_.clear();
  opacity_choices_.clear();
  SlotsHost().Children().Clear();

  if (!snapshot_.has_value()) {
    profile_id_.clear();
    ProfileText().Text(L"LayerD disconnected — settings are unavailable.");
    StatusText().Text(L"");
    SaveButton().IsEnabled(false);
    return;
  }

  profile_id_ = winrt::to_hstring(snapshot_->profile_id);
  ProfileText().Text(L"Profile: " + profile_id_);
  panel_ids_.reserve(snapshot_->available_panels.size());
  for (const auto &panel : snapshot_->available_panels)
    panel_ids_.push_back(panel.panel_id);

  for (std::uint32_t slot = 0; slot < layerc::core::max_companion_slots;
       ++slot) {
    Windows::UI::Xaml::Controls::StackPanel card;
    card.Spacing(6);

    Windows::UI::Xaml::Controls::TextBlock title;
    title.Text(L"Panel slot " + std::to_wstring(slot + 1U));
    title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    card.Children().Append(title);

    Windows::UI::Xaml::Controls::ComboBox panels;
    panels.Header(winrt::box_value(L"Panel"));
    panels.HorizontalAlignment(Windows::UI::Xaml::HorizontalAlignment::Stretch);
    panels.Items().Append(winrt::box_value(L"Hidden"));
    for (const auto &panel : snapshot_->available_panels)
      panels.Items().Append(winrt::box_value(winrt::to_hstring(panel.name)));

    const auto preference =
        std::ranges::find(snapshot_->preferences, slot,
                          &layerc::core::CompanionSlotMapping::slot_index);
    std::int32_t selected{};
    bool click_through = true;
    double opacity = 1.0;
    if (preference != snapshot_->preferences.end()) {
      const auto panel = std::ranges::find(panel_ids_, preference->panel_id);
      if (panel != panel_ids_.end())
        selected = static_cast<std::int32_t>(
            std::distance(panel_ids_.begin(), panel) + 1);
      click_through = preference->click_through;
      opacity = preference->opacity;
    }
    panels.SelectedIndex(selected);
    card.Children().Append(panels);
    panel_choices_.push_back(panels);

    Windows::UI::Xaml::Controls::ToggleSwitch click;
    click.Header(winrt::box_value(L"Allow click-through"));
    click.IsOn(click_through);
    card.Children().Append(click);
    click_through_choices_.push_back(click);

    Windows::UI::Xaml::Controls::Slider opacity_slider;
    opacity_slider.Header(winrt::box_value(L"Panel opacity"));
    opacity_slider.Minimum(0.0);
    opacity_slider.Maximum(1.0);
    opacity_slider.StepFrequency(0.05);
    opacity_slider.Value(opacity);
    card.Children().Append(opacity_slider);
    opacity_choices_.push_back(opacity_slider);

    SlotsHost().Children().Append(card);
  }
  StatusText().Text(L"");
  SaveButton().IsEnabled(!save_pending_);
}

void SettingsWidgetPage::SaveClicked(
    Windows::Foundation::IInspectable const &,
    Windows::UI::Xaml::RoutedEventArgs const &) {
  if (!snapshot_.has_value() || save_pending_)
    return;
  save_pending_ = true;
  SaveButton().IsEnabled(false);
  StatusText().Text(L"Saving…");
  save();
}

fire_and_forget SettingsWidgetPage::save() {
  auto lifetime = get_strong();
  layerc::core::CompanionProfile profile;
  profile.profile_id = snapshot_->profile_id;
  std::set<std::string, std::less<>> selected_panels;
  for (std::size_t slot = 0; slot < panel_choices_.size(); ++slot) {
    const auto selected = panel_choices_[slot].SelectedIndex();
    if (selected <= 0)
      continue;
    const auto panel_index = static_cast<std::size_t>(selected - 1);
    if (panel_index >= panel_ids_.size() ||
        !selected_panels.insert(panel_ids_[panel_index]).second) {
      StatusText().Text(L"Each panel can be assigned to only one slot.");
      save_pending_ = false;
      SaveButton().IsEnabled(true);
      co_return;
    }
    bool pinned{};
    const auto existing = std::ranges::find(
        snapshot_->preferences, static_cast<std::uint32_t>(slot),
        &layerc::core::CompanionSlotMapping::slot_index);
    if (existing != snapshot_->preferences.end())
      pinned = existing->pinned;
    profile.slots.push_back(
        {.slot_index = static_cast<std::uint32_t>(slot),
         .panel_id = panel_ids_[panel_index],
         .pinned = pinned,
         .click_through = click_through_choices_[slot].IsOn(),
         .opacity = opacity_choices_[slot].Value()});
  }

  const bool saved = co_await layerc::gamebar::widget::SnapshotHub::instance()
                         .save_preferences(std::move(profile));
  save_pending_ = false;
  StatusText().Text(saved ? L"Preferences saved."
                          : L"LayerD rejected or could not save preferences.");
  SaveButton().IsEnabled(snapshot_.has_value());
}

} // namespace winrt::LayerC::GameBar::implementation
