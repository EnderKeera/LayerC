#include "editor_window.hpp"
#include "editor_resource.h"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/panel_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace layerc::editor {
namespace {

struct PreviewPlacement {
  layerc::core::PanelGeometry geometry;
  double scale{};
  int left{};
  int top{};
};

PreviewPlacement preview_placement(const layerc::core::Panel &panel,
                                   const std::size_t item_count,
                                   const RECT &canvas, const SIZE target) {
  const auto canvas_width = std::max(1L, canvas.right - canvas.left);
  const auto canvas_height = std::max(1L, canvas.bottom - canvas.top);
  PreviewPlacement placement;
  placement.geometry = layerc::core::panel_geometry(panel, item_count);
  placement.scale = std::min(
      static_cast<double>(canvas_width) / std::max(1L, target.cx),
      static_cast<double>(canvas_height) / std::max(1L, target.cy));
  const int width = static_cast<int>(
      std::lround(placement.geometry.size.width * placement.scale));
  const int height = static_cast<int>(
      std::lround(placement.geometry.size.height * placement.scale));
  const int center_x = canvas.left +
      static_cast<int>(std::lround(panel.anchor_x * canvas_width));
  const int center_y = canvas.top +
      static_cast<int>(std::lround(panel.anchor_y * canvas_height));
  placement.left = center_x - width / 2;
  placement.top = center_y - height / 2;
  return placement;
}

RECT preview_rect(const layerc::core::PanelRect &rect,
                  const PreviewPlacement &placement) {
  return {placement.left +
              static_cast<int>(std::lround(rect.left * placement.scale)),
          placement.top +
              static_cast<int>(std::lround(rect.top * placement.scale)),
          placement.left +
              static_cast<int>(std::lround(rect.right * placement.scale)),
          placement.top +
              static_cast<int>(std::lround(rect.bottom * placement.scale))};
}

} // namespace

core::PanelPresentation EditorWindow::preview_presentation(
    const core::Panel &panel) const {
  if (const auto simulated = automation_editor_
          ? automation_editor_->panel_preview(panel.id) : std::nullopt)
    return simulated->presentation;
  const auto &profile = document_.profile();
  core::CooldownEngine engine(profile.cooldowns, profile.cooldown_groups);
  const auto states = engine.snapshot(core::TimePoint{});
  std::optional<core::AutomationRuntimeSnapshot> automation;
  if (document_.automation())
    automation = core::automation_default_snapshot(*document_.automation());
  core::PanelPresentationOverrides overrides;
  overrides.edit_mode = true;
  return core::present_panel(panel, profile.cooldowns, states,
      automation ? &*automation : nullptr, overrides, core::TimePoint{});
}

int EditorWindow::preview_actual_size_height() const {
  const auto *panel = selected_bar();
  if (panel == nullptr || panel_preview_.window == nullptr) return 0;
  const auto simulated = automation_editor_
      ? automation_editor_->panel_preview(panel->id) : std::nullopt;
  const auto effective = simulated ? simulated->panel : *panel;
  const auto geometry = core::panel_geometry(
      effective, preview_presentation(effective).items.size());
  const int dpi = static_cast<int>(native_window_dpi(panel_preview_.window));
  const int height = static_cast<int>(
      std::lround(geometry.size.height * dpi / 96.0));
  RECT client{}; GetClientRect(panel_preview_.window, &client);
  const int needed = height + MulDiv(40, dpi, 96);
  return needed * 2 <= client.bottom - client.top ? needed : 0;
}

RECT EditorWindow::bar_preview_rect(
    const core::Panel &panel, const RECT &canvas) const {
  const auto effective = preview_panel(panel);
  const auto frame = preview_presentation(effective);
  const auto count = frame.items.size();
  const auto placement = preview_placement(
      effective, count, canvas, preview_target_size());
  if (effective.style == core::PanelStyle::bar && count == 1) {
    const auto &item = frame.items.front();
    return preview_rect(core::bar_geometry(effective, 0, 1,
        item.has_progress ? item.remaining : 1.0).outline, placement);
  }
  return preview_rect({0, 0, placement.geometry.size.width,
                      placement.geometry.size.height}, placement);
}

void EditorWindow::paint_preview(const HWND window) {
  PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
  const HGDIOBJ previous_font = SelectObject(dc, shell_.font);
  const auto preview_colors = colors(layerc::platform::EditorControlRole::preview);
  RECT client{}; GetClientRect(window, &client);
  fill_solid_rect(dc, client, preview_colors.fill);
  const RECT canvas = preview_canvas();
  fill_solid_rect(dc, canvas,
                  colors(layerc::platform::EditorControlRole::window).fill);
  outline_rect(dc, canvas, 1, preview_colors.border);
  const auto *selected = selected_bar();
  const auto target = preview_target_size();
  const auto draw_panel = [&](const layerc::core::Panel &panel,
                              const PreviewPlacement &placement) {
    const bool is_selected = selected != nullptr && selected->id == panel.id;
    const auto frame = preview_presentation(panel);
    if (!frame.visible) return;
    const auto count = frame.items.size();
    for (std::size_t index = 0; index < count; ++index) {
      const auto &shown = frame.items[index];
      const bool indicator_on = shown.kind == core::OverlayItemKind::indicator &&
          !shown.unavailable && shown.active && shown.flash_on;
      const bool fill_visible = !shown.unavailable &&
          (shown.has_progress || indicator_on);
      layerc::core::PanelRect outline = placement.geometry.items[index];
      layerc::core::PanelRect fill_rect{};
      layerc::core::PanelRect text_rect = layerc::core::inset_panel_rect(
          outline, layerc::core::overlay_metrics.card_text_inset);
      int radius = layerc::core::overlay_metrics.fill_radius;
      bool draw_fill{};
      if (panel.style == layerc::core::PanelStyle::bar) {
        const auto shape = layerc::core::bar_geometry(
            panel, index, count, shown.has_progress ? shown.remaining : 1.0);
        outline = shape.outline;
        fill_rect = shape.fill;
        text_rect = shape.text;
        radius = shape.corner_radius;
        draw_fill = shape.grouped && fill_visible;
      }

      const RECT item = preview_rect(outline, placement);
      const HPEN border = CreatePen(
          PS_SOLID, is_selected ? 2 : 1,
          is_selected ? preview_colors.selection
                             : native_color_ref(panel.bar.border_color));
      const HBRUSH background = CreateSolidBrush(
          native_color_ref(((panel.style == core::PanelStyle::bar && count == 1 && fill_visible) ||
                            (panel.style == core::PanelStyle::card && indicator_on))
                        ? shown.color
                        : panel.bar.background_color));
      const HGDIOBJ previous_pen = SelectObject(dc, border);
      const HGDIOBJ previous_brush = SelectObject(dc, background);
      const int scaled_radius =
          std::max(1, static_cast<int>(std::lround(radius * placement.scale)));
      RoundRect(dc, item.left, item.top, item.right, item.bottom, scaled_radius,
                scaled_radius);
      SelectObject(dc, previous_brush);
      SelectObject(dc, previous_pen);
      DeleteObject(background);
      DeleteObject(border);

      if (draw_fill && fill_rect.right > fill_rect.left) {
        fill_solid_rect(dc, preview_rect(fill_rect, placement),
                        native_color_ref(shown.color));
      }
      const auto pips = core::item_pip_layout(panel.style, text_rect, shown.pips);
      for (std::size_t pip = 0; pip < pips.pips.size(); ++pip) {
        const RECT rect = preview_rect(pips.pips[pip], placement);
        const auto pen = CreatePen(PS_SOLID, 1, native_color_ref(shown.color));
        const auto brush = CreateSolidBrush(native_color_ref(
            pip < shown.filled_pips ? shown.color : panel.bar.background_color));
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, brush);
        const int corner = std::max(1, static_cast<int>(std::lround(2 * placement.scale)));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, corner, corner);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
      }
      if (!panel.bar.show_text) continue;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, native_color_ref(panel.bar.text_color));
      const auto label = to_wide(shown.label);
      RECT text = preview_rect(pips.text, placement);
      DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    }
  };
  for (const auto &panel : document_.profile().panels) {
    const auto simulated = automation_editor_
        ? automation_editor_->panel_preview(panel.id) : std::nullopt;
    const auto effective = preview_panel(simulated ? simulated->panel : panel);
    draw_panel(effective,
               preview_placement(effective, preview_presentation(effective).items.size(),
                                 canvas, target));
  }
  if (const int strip = preview_actual_size_height();
      selected != nullptr && strip > 0) {
    const UINT dpi = GetDpiForWindow(window);
    const int pad = MulDiv(12, static_cast<int>(dpi == 0 ? 96U : dpi), 96);
    RECT label{client.left + pad, client.bottom - strip, client.right - pad,
               client.bottom - strip + MulDiv(20, static_cast<int>(dpi), 96)};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, preview_colors.secondary);
    DrawTextW(dc, L"Actual size", -1, &label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const auto simulated = automation_editor_
        ? automation_editor_->panel_preview(selected->id) : std::nullopt;
    const auto effective = simulated ? simulated->panel : *selected;
    PreviewPlacement actual;
    actual.geometry = layerc::core::panel_geometry(
        effective, preview_presentation(effective).items.size());
    actual.scale = (dpi == 0 ? 96.0 : static_cast<double>(dpi)) / 96.0;
    actual.left = client.left + pad;
    actual.top = label.bottom + MulDiv(4, static_cast<int>(dpi), 96);
    HRGN clip = CreateRectRgn(client.left, label.bottom, client.right - pad,
                              client.bottom);
    SelectClipRgn(dc, clip);
    draw_panel(effective, actual);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
  }
  if (GetFocus() == window)
    layerc::platform::paint_focus_ring(dc, client, preview_colors.accent,
                                       GetDpiForWindow(window));
  SelectObject(dc, previous_font);
  EndPaint(window, &paint);
}

} // namespace layerc::editor
