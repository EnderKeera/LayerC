#include "overlay_window.hpp"

#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <cmath>

#include <windowsx.h>

namespace layerc::daemon {
OverlayWindow::OverlayWindow(const layerc::core::Panel &panel,
                             const std::span<const OverlayCooldown> cooldowns)
    : panel_(panel), cooldowns_(cooldowns.begin(), cooldowns.end()) {
  update_snapshot(panel, cooldowns);
}

void OverlayWindow::update_snapshot(
    const layerc::core::Panel &panel,
    const std::span<const OverlayCooldown> cooldowns) {
  panel_ = panel;
  cooldowns_.assign(cooldowns.begin(), cooldowns.end());
  cooldown_indices_.clear();
  cooldown_indices_.reserve(panel_.cooldown_ids.size());
  for (const auto &cooldown_id : panel_.cooldown_ids) {
    const auto definition =
        std::ranges::find_if(cooldowns_, [&](const OverlayCooldown &cooldown) {
          return cooldown.id == cooldown_id;
        });
    if (definition != cooldowns_.end()) {
      cooldown_indices_.push_back(
          static_cast<std::size_t>(definition - cooldowns_.begin()));
    }
  }
  label_buffer_.reserve(64);
  release_render_resources();
  calculate_size();
  if (window_ != nullptr && IsWindow(window_)) {
    SetWindowRgn(window_, nullptr, TRUE);
    last_region_width_ = -1;
    SetWindowPos(window_, nullptr, 0, 0, size_.cx, size_.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (target_.right > target_.left && target_.bottom > target_.top)
      update_position(target_);
  }
}

void OverlayWindow::set_edit_mode(const bool enabled, const bool direct_drag,
                                  const bool selected,
                                  DragCallback callback) {
  edit_mode_ = enabled;
  edit_selected_ = enabled && selected;
  drag_callback_ = std::move(callback);
  set_direct_drag(enabled && direct_drag);
  release_render_resources();
}

void OverlayWindow::set_direct_drag(const bool enabled) {
  if (!enabled && dragging_) {
    if (drag_moved_ && drag_callback_)
      drag_callback_(panel_.id, panel_.anchor_x, panel_.anchor_y, true);
    dragging_ = false;
    drag_moved_ = false;
    if (window_ != nullptr && GetCapture() == window_) ReleaseCapture();
  }
  direct_drag_ = enabled;
  if (window_ == nullptr || !IsWindow(window_)) return;
  auto style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
  if (enabled)
    style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
  else
    style |= WS_EX_TRANSPARENT;
  SetWindowLongPtrW(window_, GWL_EXSTYLE, style);
  SetWindowPos(window_, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
}
void OverlayWindow::set_capture_exclusion(const bool enabled) {
  capture_exclusion_ = enabled;
  if (window_ != nullptr && IsWindow(window_)) {
    (void)layerc::platform::apply_capture_exclusion(window_, enabled);
  }
}

OverlayWindow::~OverlayWindow() {
  if (window_ != nullptr && IsWindow(window_)) {
    DestroyWindow(window_);
  }
}

void OverlayWindow::calculate_size() {
  const LONG scaled_padding = scaled(padding);
  const LONG scaled_spacing = scaled(spacing);
  const auto count =
      static_cast<LONG>(std::max<std::size_t>(cooldown_indices_.size(), 1));
  if (panel_.style == layerc::core::PanelStyle::bar) {
    size_.cx = scaled_padding * 2 +
               scaled(static_cast<LONG>(panel_.bar.width));
    size_.cy = scaled_padding * 2 +
               count * scaled(static_cast<LONG>(panel_.bar.height)) +
               std::max<LONG>(0, count - 1) * scaled_spacing;
    return;
  }

  LONG columns = count;
  LONG rows = 1;
  if (panel_.layout == layerc::core::PanelLayout::column) {
    columns = 1;
    rows = count;
  } else if (panel_.layout == layerc::core::PanelLayout::grid) {
    columns =
        static_cast<LONG>(std::ceil(std::sqrt(static_cast<double>(count))));
    rows = (count + columns - 1) / columns;
  }
  size_.cx = scaled_padding * 2 + columns * scaled(item_width) +
             std::max<LONG>(0, columns - 1) * scaled_spacing;
  size_.cy = scaled_padding * 2 + rows * scaled(item_height) +
             std::max<LONG>(0, rows - 1) * scaled_spacing;
}

LONG OverlayWindow::scaled(const LONG value) const noexcept {
  return MulDiv(value, static_cast<int>(dpi_), 96);
}

void OverlayWindow::update_dpi(const UINT dpi) {
  const UINT next_dpi = dpi == 0 ? 96U : dpi;
  if (next_dpi == dpi_) return;
  dpi_ = next_dpi;
  release_render_resources();
  calculate_size();
  if (window_ != nullptr && IsWindow(window_)) {
    SetWindowRgn(window_, nullptr, TRUE);
    last_region_width_ = -1;
    SetWindowPos(window_, nullptr, 0, 0, size_.cx, size_.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

bool OverlayWindow::create_window() {
  WNDCLASSW definition{};
  definition.lpfnWndProc = &OverlayWindow::window_proc;
  definition.hInstance = GetModuleHandleW(nullptr);
  definition.lpszClassName = L"LayerDOverlayWindow";
  definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&definition);

  // A layered, transparent popup is skipped by cross-process mouse hit testing.
  // HTTRANSPARENT alone only forwards hits within this process's UI thread.
  // The overlay is raised only while its game is foreground and is never
  // global-topmost.
  window_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      definition.lpszClassName, L"LayerD Overlay", WS_POPUP, 0, 0, size_.cx,
      size_.cy, nullptr, nullptr, definition.hInstance, this);
  if (window_ == nullptr) {
    return false;
  }
  if (SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA) == FALSE) {
    DestroyWindow(window_);
    window_ = nullptr;
    return false;
  }
  (void)layerc::platform::apply_capture_exclusion(window_, capture_exclusion_);
  set_direct_drag(edit_mode_ && direct_drag_);
  return true;
}

void OverlayWindow::show_for(const HWND game_window) {
  if (game_window == nullptr) {
    hide();
    return;
  }
  RECT client{};
  POINT origin{};
  if (!GetClientRect(game_window, &client) ||
      !ClientToScreen(game_window, &origin)) {
    hide();
    return;
  }
  const auto width = client.right - client.left;
  const auto height = client.bottom - client.top;
  target_ = {origin.x, origin.y, origin.x + width, origin.y + height};
  update_dpi(layerc::platform::monitor_dpi_for_rect(target_));
  if (window_ == nullptr && !create_window()) {
    return;
  }
  if (owner_ != game_window) {
    SetLastError(0);
    const auto previous_owner = SetWindowLongPtrW(
        window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(game_window));
    if (previous_owner == 0 && GetLastError() != ERROR_SUCCESS) {
      hide();
      return;
    }
    owner_ = game_window;
  }
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  // Ownership already keeps this popup immediately above its game. Raising an
  // edit overlay to HWND_TOP while the game is in the background would place it
  // over LayerE and unrelated foreground applications.
  bring_to_front_ = GetAncestor(game_window, GA_ROOT) == foreground;
  update_position(target_);
  visible_ = true;
}

void OverlayWindow::show_on_rect(const RECT &target) {
  if (target.right <= target.left || target.bottom <= target.top) {
    hide();
    return;
  }
  update_dpi(layerc::platform::monitor_dpi_for_rect(target));
  if (window_ == nullptr && !create_window()) {
    return;
  }
  if (owner_ != nullptr) {
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, 0);
    owner_ = nullptr;
  }
  target_ = target;
  bring_to_front_ = true;
  update_position(target_);
  visible_ = true;
}

bool OverlayWindow::should_show(
    const std::vector<layerc::core::CooldownState> &states) const {
  if (edit_mode_ || panel_.style != layerc::core::PanelStyle::bar ||
      !panel_.bar.hide_when_ready) {
    return true;
  }
  return std::ranges::any_of(cooldown_indices_, [&](const std::size_t index) {
    const auto &cooldown = cooldowns_[index];
    const auto state = std::ranges::find_if(
        states, [&](const layerc::core::CooldownState &candidate) {
          return candidate.id == cooldown.id;
        });
    return state != states.end() && state->next_charge_at.has_value();
  });
}

void OverlayWindow::clear_window_state() {
  window_ = nullptr;
  owner_ = nullptr;
  visible_ = false;
  target_ = {};
  release_render_resources();
}

void OverlayWindow::hide() {
  if (window_ != nullptr && visible_) {
    ShowWindow(window_, SW_HIDE);
  }
  visible_ = false;
  target_ = {};
  release_render_resources();
}

void OverlayWindow::update_position(const RECT &target) {
  const auto width = target.right - target.left;
  const auto height = target.bottom - target.top;
  if (width <= 0 || height <= 0) {
    hide();
    return;
  }
  const int x = target.left +
                static_cast<int>(std::lround(panel_.anchor_x * width)) -
                size_.cx / 2;
  const int y = target.top +
                static_cast<int>(std::lround(panel_.anchor_y * height)) -
                size_.cy / 2;
  if (bring_to_front_) {
    SetWindowPos(window_, HWND_TOP, x, y, size_.cx, size_.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
  } else {
    // Preserve the current normal z-order while another application is
    // foreground. The foreground app naturally covers this overlay, without any
    // global-topmost behavior.
    SetWindowPos(window_, nullptr, x, y, size_.cx, size_.cy,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_SHOWWINDOW);
  }
}

LRESULT CALLBACK OverlayWindow::window_proc(const HWND window,
                                            const UINT message,
                                            const WPARAM wparam,
                                            const LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto *overlay = reinterpret_cast<OverlayWindow *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCDESTROY && overlay != nullptr) {
    overlay->clear_window_state();
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
  }
  if (message == WM_NCHITTEST) {
    return overlay != nullptr && overlay->direct_drag_ ? HTCLIENT
                                                       : HTTRANSPARENT;
  }
  if (message == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  if (message == WM_PAINT && overlay != nullptr) {
    overlay->paint();
    return 0;
  }
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (overlay != nullptr && overlay->direct_drag_) {
    if (message == WM_LBUTTONDOWN) {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(window, &point);
      const int overlay_x = overlay->target_.left + static_cast<int>(std::lround(
          overlay->panel_.anchor_x *
          (overlay->target_.right - overlay->target_.left)));
      const int overlay_y = overlay->target_.top + static_cast<int>(std::lround(
          overlay->panel_.anchor_y *
          (overlay->target_.bottom - overlay->target_.top)));
      overlay->drag_start_ = point;
      overlay->drag_offset_ = {point.x - overlay_x, point.y - overlay_y};
      overlay->dragging_ = true;
      overlay->drag_moved_ = false;
      SetCapture(window);
      return 0;
    }
    if (message == WM_MOUSEMOVE && overlay->dragging_) {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(window, &point);
      if (point.x != overlay->drag_start_.x || point.y != overlay->drag_start_.y)
        overlay->drag_moved_ = true;
      if (overlay->drag_moved_) overlay->update_drag(point, false);
      return 0;
    }
    if (message == WM_LBUTTONUP && overlay->dragging_) {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(window, &point);
      if (point.x != overlay->drag_start_.x || point.y != overlay->drag_start_.y)
        overlay->drag_moved_ = true;
      if (overlay->drag_moved_) overlay->update_drag(point, true);
      overlay->dragging_ = false;
      overlay->drag_moved_ = false;
      if (GetCapture() == window) ReleaseCapture();
      return 0;
    }
    if (message == WM_CAPTURECHANGED) {
      if (overlay->dragging_ && overlay->drag_moved_ &&
          overlay->drag_callback_)
        overlay->drag_callback_(overlay->panel_.id, overlay->panel_.anchor_x,
                                overlay->panel_.anchor_y, true);
      overlay->dragging_ = false;
      overlay->drag_moved_ = false;
      return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void OverlayWindow::update_drag(const POINT screen_point, const bool final) {
  const auto width = std::max(1L, target_.right - target_.left);
  const auto height = std::max(1L, target_.bottom - target_.top);
  panel_.anchor_x = std::clamp(
      (screen_point.x - drag_offset_.x - target_.left) /
          static_cast<double>(width),
      0.0, 1.0);
  panel_.anchor_y = std::clamp(
      (screen_point.y - drag_offset_.y - target_.top) /
          static_cast<double>(height),
      0.0, 1.0);
  update_position(target_);
  if (drag_callback_)
    drag_callback_(panel_.id, panel_.anchor_x, panel_.anchor_y, final);
}

} // namespace layerc::daemon
