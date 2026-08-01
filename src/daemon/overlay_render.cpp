#include "overlay_window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace layerc::daemon {
namespace {

COLORREF to_color_ref(const std::uint32_t color) {
  return RGB((color >> 16U) & 0xFFU, (color >> 8U) & 0xFFU, color & 0xFFU);
}

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

void append_unsigned(std::wstring &output, std::uint64_t value) {
  std::array<wchar_t, 32> digits{};
  auto *end = digits.data() + digits.size();
  auto *cursor = end;
  do {
    *--cursor = static_cast<wchar_t>(L'0' + value % 10U);
    value /= 10U;
  } while (value != 0);
  output.append(cursor, end);
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
  const int font_height = -MulDiv(14, GetDeviceCaps(window_dc, LOGPIXELSY), 72);
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

  const auto background_color = panel_.style == layerc::core::PanelStyle::bar
                                    ? panel_.bar.background_color
                                    : 0x272D35U;
  const auto border_color = panel_.style == layerc::core::PanelStyle::bar
                                ? panel_.bar.border_color
                                : 0x50BAC8U;
  const auto fill_color = panel_.style == layerc::core::PanelStyle::bar
                              ? panel_.bar.fill_color
                              : border_color;
  const auto text_color = panel_.style == layerc::core::PanelStyle::bar
                              ? panel_.bar.text_color
                              : 0xE4E8ECU;

  font_ =
      CreateFontW(font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  window_brush_ = CreateSolidBrush(to_color_ref(0x151A20U));
  panel_brush_ = CreateSolidBrush(to_color_ref(background_color));
  fill_brush_ = CreateSolidBrush(to_color_ref(fill_color));
  border_pen_ = CreatePen(PS_SOLID, 1, to_color_ref(border_color));
  text_color_ = to_color_ref(text_color);

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

void OverlayWindow::render(
    const std::vector<layerc::core::CooldownState> &states,
    const layerc::core::TimePoint now) {
  if (!visible_ || !ensure_render_resources()) {
    return;
  }
  if (!should_show(states)) {
    hide();
    return;
  }
  update_position(target_);

  RECT canvas{0, 0, size_.cx, size_.cy};
  FillRect(buffer_dc_, &canvas, window_brush_);
  const int saved_state = SaveDC(buffer_dc_);
  SetBkMode(buffer_dc_, TRANSPARENT);
  SetTextColor(buffer_dc_, text_color_);
  SelectObject(buffer_dc_, font_);

  const auto count =
      static_cast<LONG>(std::max<std::size_t>(cooldown_indices_.size(), 1));
  const LONG grid_columns =
      panel_.layout == layerc::core::PanelLayout::grid
          ? static_cast<LONG>(std::ceil(std::sqrt(static_cast<double>(count))))
          : count;
  const auto null_pen = static_cast<HPEN>(GetStockObject(NULL_PEN));
  const HPEN selection_pen = edit_selected_
      ? CreatePen(PS_SOLID, 2, to_color_ref(0xE0A15AU))
      : nullptr;
  const HPEN visible_border = selection_pen != nullptr ? selection_pen
                                                        : border_pen_;

  for (std::size_t index = 0; index < cooldown_indices_.size(); ++index) {
    const auto &cooldown = cooldowns_[cooldown_indices_[index]];
    const auto state = std::ranges::find_if(
        states, [&](const layerc::core::CooldownState &candidate) {
          return candidate.id == cooldown.id;
        });
    label_buffer_.assign(cooldown.name);
    double progress = 1.0;
    if (state != states.end()) {
      if (state->next_charge_at.has_value()) {
        const auto remaining =
            std::max(layerc::core::Milliseconds::zero(),
                     std::chrono::duration_cast<layerc::core::Milliseconds>(
                         *state->next_charge_at - now));
        const auto tenths = (remaining.count() + 99) / 100;
        label_buffer_.append(L"  ");
        append_unsigned(label_buffer_, static_cast<std::uint64_t>(tenths / 10));
        label_buffer_.push_back(L'.');
        append_unsigned(label_buffer_, static_cast<std::uint64_t>(tenths % 10));
        label_buffer_.push_back(L's');
        if (cooldown.duration > layerc::core::Milliseconds::zero()) {
          progress =
              std::clamp(static_cast<double>(remaining.count()) /
                             static_cast<double>(cooldown.duration.count()),
                         0.0, 1.0);
        } else {
          progress = 0.0;
        }
      } else {
        label_buffer_.append(L"  READY");
      }
      if (state->max_charges > 1) {
        label_buffer_.append(L"  [");
        append_unsigned(label_buffer_, state->available_charges);
        label_buffer_.push_back(L']');
      }
    }

    if (panel_.style == layerc::core::PanelStyle::bar) {
      const int left = padding;
      const int bar_width = static_cast<int>(panel_.bar.width);
      const int bar_height = static_cast<int>(panel_.bar.height);
      const int top =
          padding + static_cast<int>(index) * (bar_height + spacing);
      const bool is_single_bar = cooldown_indices_.size() == 1;
      const int visible_width =
          std::max(1, static_cast<int>(std::lround(
                          static_cast<double>(bar_width) * progress)));
      const RECT bar{left, top,
                     left + (is_single_bar ? visible_width : bar_width),
                     top + bar_height};

      if (is_single_bar) {
        const HRGN region =
            CreateRoundRectRgn(bar.left, bar.top, bar.right, bar.bottom, 8, 8);
        if (region != nullptr && SetWindowRgn(window_, region, TRUE) == 0) {
          DeleteObject(region);
        }
        draw_rounded_rectangle(buffer_dc_, bar, 8, fill_brush_, visible_border);
      } else {
        draw_rounded_rectangle(buffer_dc_, bar, 8, panel_brush_, visible_border);
        const int fill_width = static_cast<int>(
            std::lround(static_cast<double>(bar_width - 4) * progress));
        if (fill_width > 0) {
          const RECT fill{left + 2, top + 2, left + 2 + fill_width,
                          top + bar_height - 2};
          draw_rounded_rectangle(buffer_dc_, fill, 6, fill_brush_, null_pen);
        }
      }

      RECT text_rectangle{left + 8, top + 2,
                          left + (is_single_bar ? visible_width : bar_width) -
                              8,
                          top + bar_height - 2};
      if (panel_.bar.show_text &&
          text_rectangle.right > text_rectangle.left) {
        DrawTextW(buffer_dc_, label_buffer_.c_str(),
                  static_cast<int>(label_buffer_.size()), &text_rectangle,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      }
      continue;
    }

    LONG column = static_cast<LONG>(index);
    LONG row = 0;
    if (panel_.layout == layerc::core::PanelLayout::column) {
      column = 0;
      row = static_cast<LONG>(index);
    } else if (panel_.layout == layerc::core::PanelLayout::grid) {
      column = static_cast<LONG>(index) % grid_columns;
      row = static_cast<LONG>(index) / grid_columns;
    }

    const LONG left = padding + column * (item_width + spacing);
    const LONG top = padding + row * (item_height + spacing);
    const RECT item{left, top, left + item_width, top + item_height};
    draw_rounded_rectangle(buffer_dc_, item, 6, panel_brush_, visible_border);

    RECT text_rectangle{left + 3, top + 3, left + item_width - 3,
                        top + item_height - 3};
    DrawTextW(buffer_dc_, label_buffer_.c_str(),
              static_cast<int>(label_buffer_.size()), &text_rectangle,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  }

  if (selection_pen != nullptr) DeleteObject(selection_pen);

  if (saved_state != 0) {
    RestoreDC(buffer_dc_, saved_state);
  }
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
