#include "pch.h"
#include "App.h"
#include "PanelWidgetPage.h"
// Generated definitions require the projected page declaration above.
#include "PanelWidgetPage.g.cpp"

namespace winrt::LayerC::GameBar::implementation {
namespace {

hstring remaining_text(const layerc::core::CompanionSnapshotItem &item) {
  if (item.is_ready)
    return L"Ready";
  const auto seconds = (item.remaining_ms + 999) / 1000;
  std::wstring text = std::to_wstring(seconds) + L" s";
  if (item.max_charges > 1) {
    text += L"  ·  ";
    text += std::to_wstring(item.available_charges);
    text += L"/";
    text += std::to_wstring(item.max_charges);
  }
  return hstring(text);
}

} // namespace

PanelWidgetPage::PanelWidgetPage() { InitializeComponent(); }

void PanelWidgetPage::OnNavigatedTo(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &args) {
  widget_ = args.Parameter()
                .try_as<Microsoft::Gaming::XboxGameBar::XboxGameBarWidget>();
  if (widget_ == nullptr)
    return;
  const auto slot = panel_slot(widget_.AppExtensionId());
  if (!slot.has_value())
    return;
  slot_index_ = *slot;

  auto refresh = [weak = get_weak()](const auto &, const auto &) {
    if (const auto self = weak.get())
      self->apply_presentation();
  };
  settings_token_ =
      widget_.SettingsClicked({this, &PanelWidgetPage::open_settings});
  theme_token_ = widget_.RequestedThemeChanged(refresh);
  opacity_token_ = widget_.RequestedOpacityChanged(refresh);
  compact_token_ = widget_.CompactModeEnabledChanged(refresh);
  click_through_token_ = widget_.ClickThroughEnabledChanged(refresh);

  subscription_ = layerc::gamebar::widget::subscribe_on_dispatcher(
      Dispatcher(), get_weak(), [](const auto &self, auto snapshot) {
        self->render(std::move(snapshot));
      });
  apply_presentation();
}

void PanelWidgetPage::OnNavigatedFrom(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &) {
  if (subscription_ != 0) {
    layerc::gamebar::widget::SnapshotHub::instance().unsubscribe(subscription_);
    subscription_ = 0;
  }
  if (widget_ != nullptr) {
    widget_.SettingsClicked(settings_token_);
    widget_.RequestedThemeChanged(theme_token_);
    widget_.RequestedOpacityChanged(opacity_token_);
    widget_.CompactModeEnabledChanged(compact_token_);
    widget_.ClickThroughEnabledChanged(click_through_token_);
  }
  widget_ = nullptr;
}

void PanelWidgetPage::render(
    std::optional<layerc::core::CompanionSessionSnapshot> snapshot) {
  snapshot_ = std::move(snapshot);
  ItemsHost().Children().Clear();
  if (!snapshot_.has_value()) {
    PanelTitle().Text(L"LayerC");
    StatusText().Text(L"LayerD disconnected — cooldown data is hidden.");
    apply_presentation();
    return;
  }

  const auto panel =
      std::ranges::find(snapshot_->panels, slot_index_,
                        &layerc::core::CompanionPanelSnapshot::slot_index);
  if (panel == snapshot_->panels.end()) {
    PanelTitle().Text(L"LayerC");
    StatusText().Text(L"No panel is assigned to this slot.");
    apply_presentation();
    return;
  }

  const auto descriptor =
      std::ranges::find(snapshot_->available_panels, panel->panel_id,
                        &layerc::core::CompanionAvailablePanel::panel_id);
  PanelTitle().Text(winrt::to_hstring(
      descriptor != snapshot_->available_panels.end() ? descriptor->name
                                                      : panel->panel_id));
  StatusText().Text(panel->items.empty() ? L"No cooldowns in this panel."
                                         : L"");

  for (const auto &item : panel->items) {
    Windows::UI::Xaml::Controls::StackPanel row;
    row.Spacing(4);

    Windows::UI::Xaml::Controls::StackPanel heading;
    heading.Orientation(Windows::UI::Xaml::Controls::Orientation::Horizontal);
    heading.Spacing(8);

    Windows::UI::Xaml::Controls::TextBlock name;
    name.Text(winrt::to_hstring(item.name));
    name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    name.TextWrapping(Windows::UI::Xaml::TextWrapping::Wrap);
    heading.Children().Append(name);

    Windows::UI::Xaml::Controls::TextBlock remaining;
    remaining.Text(remaining_text(item));
    remaining.Opacity(0.8);
    heading.Children().Append(remaining);
    row.Children().Append(heading);

    Windows::UI::Xaml::Controls::ProgressBar progress;
    progress.Minimum(0.0);
    progress.Maximum(100.0);
    progress.Value(item.progress * 100.0);
    progress.Height(5.0);
    row.Children().Append(progress);
    ItemsHost().Children().Append(row);
  }
  apply_presentation();
}

void PanelWidgetPage::apply_presentation() {
  if (widget_ == nullptr)
    return;
  Root().RequestedTheme(widget_.RequestedTheme());
  widget_.PinningSupported(true);
  widget_.SettingsSupported(true);

  double sidecar_opacity = 1.0;
  bool allow_click_through = true;
  if (snapshot_.has_value()) {
    const auto preference =
        std::ranges::find(snapshot_->preferences, slot_index_,
                          &layerc::core::CompanionSlotMapping::slot_index);
    if (preference != snapshot_->preferences.end()) {
      sidecar_opacity = preference->opacity;
      allow_click_through = preference->click_through;
    }
  }
  Root().Opacity(std::clamp(widget_.RequestedOpacity(), 0.0, 1.0) *
                 sidecar_opacity);
  Root().IsHitTestVisible(
      !(allow_click_through && widget_.ClickThroughEnabled()));
  StatusText().Visibility(widget_.CompactModeEnabled() && snapshot_.has_value()
                              ? Windows::UI::Xaml::Visibility::Collapsed
                              : Windows::UI::Xaml::Visibility::Visible);
}

fire_and_forget PanelWidgetPage::open_settings(
    Microsoft::Gaming::XboxGameBar::XboxGameBarWidget const &sender,
    Windows::Foundation::IInspectable const &) {
  try {
    co_await sender.ActivateSettingsAsync();
  } catch (...) {
  }
}

} // namespace winrt::LayerC::GameBar::implementation
