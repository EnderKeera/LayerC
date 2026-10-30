#include "overlay_window.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::platform::native_color_ref;

template <typename Handle> void delete_gdi_object(Handle &object) {
  if (object != nullptr) {
    DeleteObject(object);
    object = nullptr;
  }
}

void draw_rounded_rectangle(HDC dc, const RECT &rectangle, const int radius,
                            HBRUSH brush, HPEN pen) {
  const HGDIOBJ previous_brush = SelectObject(dc, brush);
  const HGDIOBJ previous_pen = SelectObject(dc, pen);
  RoundRect(dc, rectangle.left, rectangle.top, rectangle.right,
            rectangle.bottom, radius, radius);
  SelectObject(dc, previous_pen);
  SelectObject(dc, previous_brush);
}

void draw_label(HDC dc, const std::wstring &label, RECT &rectangle) {
  DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &rectangle,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
}

} // namespace
bool OverlayWindow::ensure_render_resources() {
  if (buffer_dc_ != nullptr) {
    return true;
  }
  if (window_ == nullptr) {
    return false;
  }

  const HDC window_dc = GetDC(window_);
  if (window_dc == nullptr) {
    return false;
  }
  const int font_height = -MulDiv(14, static_cast<int>(dpi_), 72);
  buffer_dc_ = CreateCompatibleDC(window_dc);
  if (buffer_dc_ != nullptr) {
    buffer_bitmap_ = CreateCompatibleBitmap(window_dc, size_.cx, size_.cy);
  }
  ReleaseDC(window_, window_dc);

  if (buffer_dc_ == nullptr || buffer_bitmap_ == nullptr) {
    release_render_resources();
    return false;
  }
  previous_bitmap_ = SelectObject(buffer_dc_, buffer_bitmap_);
  if (previous_bitmap_ == nullptr || previous_bitmap_ == HGDI_ERROR) {
    release_render_resources();
    return false;
  }

  const bool is_bar = panel_.style == layerc::core::PanelStyle::bar;
  const auto background_color =
      is_bar ? panel_.bar.background_color : 0x272D35U;
  const auto border_color = is_bar ? panel_.bar.border_color : 0x50BAC8U;
  const auto fill_color = is_bar ? panel_.bar.fill_color : border_color;
  const auto text_color = is_bar ? panel_.bar.text_color : 0xE4E8ECU;

  font_ =
      CreateFontW(font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  window_brush_ = CreateSolidBrush(native_color_ref(0x151A20U));
  panel_brush_ = CreateSolidBrush(native_color_ref(background_color));
  fill_brush_ = CreateSolidBrush(native_color_ref(fill_color));
  border_pen_ =
      CreatePen(PS_SOLID, std::max(1L, scaled(1)), native_color_ref(border_color));
  text_color_ = native_color_ref(text_color);

  const bool ready = font_ != nullptr && window_brush_ != nullptr &&
                     panel_brush_ != nullptr && fill_brush_ != nullptr &&
                     border_pen_ != nullptr;
  if (!ready) {
    release_render_resources();
  }
  return ready;
}

void OverlayWindow::release_render_resources() {
  if (buffer_dc_ != nullptr && previous_bitmap_ != nullptr &&
      previous_bitmap_ != HGDI_ERROR) {
    SelectObject(buffer_dc_, previous_bitmap_);
  }
  previous_bitmap_ = nullptr;
  for (auto &[color, brush] : row_brushes_) {
    (void)color;
    delete_gdi_object(brush);
  }
  row_brushes_.clear();
  delete_gdi_object(buffer_bitmap_);
  delete_gdi_object(font_);
  delete_gdi_object(window_brush_);
  delete_gdi_object(panel_brush_);
  delete_gdi_object(fill_brush_);
  delete_gdi_object(border_pen_);
  if (buffer_dc_ != nullptr) {
    DeleteDC(buffer_dc_);
    buffer_dc_ = nullptr;
  }
}

void OverlayWindow::render(const layerc::core::PanelPresentation &presentation) {
  if (!visible_) return;
  (void)sync_rows(presentation);
  if (!ensure_render_resources()) return;
  update_position(target_);

  RECT canvas{0, 0, size_.cx, size_.cy};
  FillRect(buffer_dc_, &canvas, window_brush_);
  const int saved_state = SaveDC(buffer_dc_);
  SetBkMode(buffer_dc_, TRANSPARENT);
  SetTextColor(buffer_dc_, text_color_);
  SelectObject(buffer_dc_, font_);

  using namespace layerc::core;
  const auto geometry = panel_geometry(panel_, rows_.size());
  const auto scaled_rect = [&](const PanelRect rect) {
    return RECT{scaled(rect.left), scaled(rect.top), scaled(rect.right), scaled(rect.bottom)};
  };
  const auto null_pen = static_cast<HPEN>(GetStockObject(NULL_PEN));
  const HPEN selection_pen = edit_selected_
      ? CreatePen(PS_SOLID, 2, native_color_ref(0xE0A15AU)) : nullptr;
  const HPEN visible_border = selection_pen != nullptr ? selection_pen : border_pen_;
  const auto color_brush = [&](const std::uint32_t color) {
    auto [entry, inserted] = row_brushes_.try_emplace(color, nullptr);
    if (inserted) entry->second = CreateSolidBrush(native_color_ref(color));
    return entry->second != nullptr ? entry->second : fill_brush_;
  };
  const auto draw_pips = [&](const ItemPipLayout &layout, const ItemPresentation &shown,
                             const HBRUSH brush) {
    if (layout.pips.empty()) return;
    const HPEN pen = CreatePen(PS_SOLID, std::max(1L, scaled(1)), native_color_ref(shown.color));
    if (pen == nullptr) return;
    for (std::size_t pip = 0; pip < layout.pips.size(); ++pip) {
      const RECT rect = scaled_rect(layout.pips[pip]);
      const bool filled = pip < shown.filled_pips;
      draw_rounded_rectangle(buffer_dc_, rect, std::max(1L, scaled(2)),
          filled ? brush : panel_brush_,
          filled ? null_pen : pen);
    }
    DeleteObject(pen);
  };
  for (std::size_t index = 0; index < presentation.items.size(); ++index) {
    auto &row = rows_[index];
    const auto &shown = presentation.items[index];
    if (shown.label != row.label) {
      row.wide_label = layerc::platform::utf8_to_wide(shown.label);
      row.label = shown.label;
    }
    const HBRUSH brush = color_brush(shown.color);
    const bool indicator_on = shown.kind == OverlayItemKind::indicator &&
                               !shown.unavailable && shown.active && shown.flash_on;
    const bool fill_visible = !shown.unavailable && (shown.has_progress || indicator_on);
    const double progress = shown.has_progress ? shown.remaining : 1.0;
    if (panel_.style == PanelStyle::bar) {
      const auto shape = bar_geometry(panel_, index, rows_.size(), progress);
      const RECT bar = scaled_rect(shape.outline);
      const int radius = std::max(1L, scaled(shape.corner_radius));
      if (!shape.grouped) {
        if (bar.right - bar.left != last_region_width_) {
          const HRGN region = CreateRoundRectRgn(
              bar.left, bar.top, bar.right, bar.bottom, radius, radius);
          if (region != nullptr) {
            if (SetWindowRgn(window_, region, TRUE) == 0) DeleteObject(region);
            else last_region_width_ = bar.right - bar.left;
          }
        }
        draw_rounded_rectangle(buffer_dc_, bar, radius,
                               fill_visible ? brush : panel_brush_, visible_border);
      } else {
        draw_rounded_rectangle(buffer_dc_, bar, radius, panel_brush_, visible_border);
        const RECT fill = scaled_rect(shape.fill);
        if (fill_visible && fill.right > fill.left && fill.bottom > fill.top)
          draw_rounded_rectangle(buffer_dc_, fill,
              std::max(1L, scaled(shape.fill_radius)), brush, null_pen);
      }
      const auto layout = item_pip_layout(PanelStyle::bar, shape.text, shown.pips);
      draw_pips(layout, shown, brush);
      RECT text = scaled_rect(layout.text);
      if (panel_.bar.show_text && text.right > text.left) draw_label(buffer_dc_, row.wide_label, text);
    } else {
      const RECT item = scaled_rect(geometry.items[index]);
      draw_rounded_rectangle(buffer_dc_, item, std::max(1L, scaled(overlay_metrics.fill_radius)),
                             indicator_on ? brush : panel_brush_, visible_border);
      const auto layout = item_pip_layout(PanelStyle::card,
          inset_panel_rect(geometry.items[index], overlay_metrics.card_text_inset), shown.pips);
      draw_pips(layout, shown, brush);
      RECT text = scaled_rect(layout.text);
      if (panel_.bar.show_text && text.right > text.left) draw_label(buffer_dc_, row.wide_label, text);
    }
  }

  if (selection_pen != nullptr) DeleteObject(selection_pen);

  if (saved_state != 0) {
    RestoreDC(buffer_dc_, saved_state);
  }
  for (auto &[color, brush] : row_brushes_) {
    (void)color;
    delete_gdi_object(brush);
  }
  row_brushes_.clear();
  InvalidateRect(window_, nullptr, FALSE);
  UpdateWindow(window_);
}

void OverlayWindow::paint() {
  PAINTSTRUCT paint{};
  const HDC window_dc = BeginPaint(window_, &paint);
  if (window_dc != nullptr && buffer_dc_ != nullptr) {
    BitBlt(window_dc, 0, 0, size_.cx, size_.cy, buffer_dc_, 0, 0, SRCCOPY);
  }
  EndPaint(window_, &paint);
}

} // namespace layerc::daemon
