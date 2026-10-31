#include "pch.h"
#include "App.h"
#include "PanelWidgetPage.h"
// Generated definitions require the projected page declaration above.
#include "PanelWidgetPage.g.cpp"
#include "WidgetRowModel.hpp"

namespace winrt::LayerC::GameBar::implementation {
namespace {
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

constexpr std::uint32_t default_accent = 0x50BAC8;

Media::SolidColorBrush brush(const std::uint32_t rgb) {
  return Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(
      255, static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
      static_cast<std::uint8_t>(rgb)));
}

// A fixed 8x8 DIP square with a 3 DIP gap (eight pips = 85 DIP), outlined
// when empty, so pips never widen a row beyond its slot.
Border pip(const Media::SolidColorBrush &accent, const bool filled) {
  Border square;
  square.Width(8.0);
  square.Height(8.0);
  square.BorderThickness(ThicknessHelper::FromUniformLength(1.0));
  square.BorderBrush(accent);
  if (filled) square.Background(accent);
  return square;
}

// Label and value share one row; the label wraps instead of widening it.
UIElement row_view(const layerc::gamebar::widget::RowModel &item) {
  const auto accent = brush(item.color.value_or(default_accent));
  StackPanel row;
  row.Spacing(4);
  row.MinHeight(36.0);
  if (item.unavailable) row.Opacity(0.55);

  Grid heading;
  heading.ColumnSpacing(8);
  for (const auto width : {GridLengthHelper::Auto(),
                           GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star),
                           GridLengthHelper::Auto()}) {
    ColumnDefinition column;
    column.Width(width);
    heading.ColumnDefinitions().Append(column);
  }
  if (item.indicator) {
    // The swatch keeps its space in both flash phases.
    Border swatch;
    swatch.Width(12.0);
    swatch.Height(12.0);
    swatch.CornerRadius(CornerRadiusHelper::FromUniformRadius(6.0));
    swatch.BorderThickness(ThicknessHelper::FromUniformLength(1.0));
    swatch.BorderBrush(accent);
    if (item.accent_on) swatch.Background(accent);
    swatch.VerticalAlignment(VerticalAlignment::Center);
    heading.Children().Append(swatch);
  }
  TextBlock label;
  label.Text(winrt::to_hstring(item.label));
  label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
  label.TextWrapping(TextWrapping::Wrap);
  Grid::SetColumn(label, 1);
  heading.Children().Append(label);
  if (!item.value.empty()) {
    TextBlock value;
    value.Text(winrt::to_hstring(item.value));
    value.Opacity(0.8);
    Grid::SetColumn(value, 2);
    heading.Children().Append(value);
  }
  row.Children().Append(heading);

  if (item.pips != 0) {
    StackPanel pips;
    pips.Orientation(Orientation::Horizontal);
    pips.Spacing(3);
    for (std::uint32_t index = 0; index < item.pips; ++index)
      pips.Children().Append(pip(accent, index < item.filled_pips));
    row.Children().Append(pips);
  }
  if (item.fill) {
    ProgressBar progress;
    progress.Minimum(0.0);
    progress.Maximum(100.0);
    progress.Value(*item.fill * 100.0);
    progress.Height(5.0);
    if (item.color) progress.Foreground(accent);
    row.Children().Append(progress);
  }
  return row;
}

} // namespace

PanelWidgetPage::PanelWidgetPage() {
  InitializeComponent();
  unloaded_ = Unloaded(auto_revoke, [weak = get_weak()](const auto &, const auto &) {
    if (const auto self = weak.get())
      self->cleanup();
  });
}

PanelWidgetPage::~PanelWidgetPage() { cleanup(); }

void PanelWidgetPage::OnNavigatedTo(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &args) {
  cleanup();
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
  settings_ = widget_.SettingsClicked(
      auto_revoke, [weak = get_weak()](const auto &sender, const auto &args) {
        if (const auto self = weak.get(); self && self->widget_ != nullptr)
          self->open_settings(sender, args);
      });
  theme_ = widget_.RequestedThemeChanged(auto_revoke, refresh);
  opacity_ = widget_.RequestedOpacityChanged(auto_revoke, refresh);
  compact_ = widget_.CompactModeEnabledChanged(auto_revoke, refresh);
  click_through_ = widget_.ClickThroughEnabledChanged(auto_revoke, refresh);
  closed_ = Windows::UI::Xaml::Window::Current().Closed(
      auto_revoke, [weak = get_weak()](const auto &, const auto &) {
        if (const auto self = weak.get())
          self->cleanup();
      });

  subscription_ = layerc::gamebar::widget::subscribe_on_dispatcher(
      Dispatcher(), get_weak(), [](const auto &self, auto snapshot) {
        self->render(std::move(snapshot));
      });
  apply_presentation();
}

void PanelWidgetPage::OnNavigatedFrom(
    Windows::UI::Xaml::Navigation::NavigationEventArgs const &) {
  cleanup();
}

void PanelWidgetPage::cleanup() noexcept {
  subscription_.reset();
  closed_.revoke();
  settings_.revoke();
  theme_.revoke();
  opacity_.revoke();
  compact_.revoke();
  click_through_.revoke();
  widget_ = nullptr;
  snapshot_.reset();
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
  const auto model =
      layerc::gamebar::widget::panel_model(*panel, snapshot_->snapshot_version);
  StatusText().Text(winrt::to_hstring(model.status));
  for (const auto &item : model.rows) ItemsHost().Children().Append(row_view(item));
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
