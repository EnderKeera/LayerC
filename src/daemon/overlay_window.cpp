#include "overlay_window.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <cmath>

#include <windowsx.h>

namespace layerc::daemon {

OverlayWindow::OverlayWindow(const layerc::core::Panel &panel,
                             const std::span<const layerc::core::CooldownDefinition>
                                 cooldowns)
    : panel_(panel) {
  (void)assign_rows(cooldowns);
  calculate_size();
}

bool OverlayWindow::assign_rows(
    const std::span<const layerc::core::CooldownDefinition> cooldowns) {
  cooldowns_.assign(cooldowns.begin(), cooldowns.end());
  auto seed = overrides_;
  seed.edit_mode = true;
  seed.visible = true;
  seed.items.clear();
  return sync_rows(layerc::core::present_panel(panel_, cooldowns_, {}, nullptr, seed, {}));
}

bool OverlayWindow::sync_rows(const layerc::core::PanelPresentation &presentation) {
  if (rows_.size() == presentation.items.size() &&
      std::ranges::equal(rows_, presentation.items, {}, &Row::item_id,
                         &layerc::core::ItemPresentation::item_id)) return false;
  const bool resized = rows_.size() != presentation.items.size();
  std::vector<Row> next;
  next.reserve(presentation.items.size());
  for (const auto &item : presentation.items) {
    const auto found = std::ranges::find(rows_, item.item_id, &Row::item_id);
    if (found != rows_.end()) next.push_back(std::move(*found));
    else next.push_back({item.item_id, {}, {}});
  }
  rows_ = std::move(next);
  if (resized) {
    release_render_resources();
    calculate_size();
    if (window_ != nullptr && IsWindow(window_)) {
      SetWindowRgn(window_, nullptr, TRUE);
      last_region_width_ = -1;
      SetWindowPos(window_, nullptr, 0, 0, size_.cx, size_.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  return true;
}

void OverlayWindow::update_snapshot(
    const layerc::core::Panel &panel,
    const std::span<const layerc::core::CooldownDefinition> cooldowns) {
  // Separate content from geometry: an unchanged panel does nothing, and a
  // moved anchor (a drag) only repositions the existing window.
  auto moved = panel_;
  moved.anchor_x = panel.anchor_x;
  moved.anchor_y = panel.anchor_y;
  const bool anchor_changed = moved != panel_;
  const bool appearance_changed = moved != panel;
  const bool definitions_changed = !std::ranges::equal(cooldowns_, cooldowns);
  panel_ = panel;
  const bool rows_changed = (appearance_changed || definitions_changed) && assign_rows(cooldowns);
  if (!anchor_changed && !appearance_changed && !definitions_changed && !rows_changed) return;
  const bool window_valid = window_ != nullptr && IsWindow(window_);
  if (appearance_changed || rows_changed) {
    release_render_resources();
    calculate_size();
    if (window_valid) {
      SetWindowRgn(window_, nullptr, TRUE);
      last_region_width_ = -1;
      SetWindowPos(window_, nullptr, 0, 0, size_.cx, size_.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (window_valid && target_.right > target_.left &&
      target_.bottom > target_.top)
    update_position(target_);
}

void OverlayWindow::set_session_override(
    const std::optional<bool> visible,
    std::optional<std::string> text,
    std::vector<layerc::core::ItemPresentationOverride> items) {
  overrides_.visible = visible;
  overrides_.text = std::move(text);
  overrides_.items = std::move(items);
}
void OverlayWindow::set_edit_mode(const bool enabled, const bool direct_drag,
                                  const bool selected,
                                  DragCallback callback,
                                  DirectDragLost lost) {
  edit_mode_ = enabled;
  overrides_.edit_mode = enabled;
  edit_selected_ = enabled && selected;
  drag_callback_ = std::move(callback);
  direct_drag_lost_ = std::move(lost);
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
void OverlayWindow::lose_direct_drag_capture() {
  const bool moved = drag_moved_;
  const auto finish = drag_callback_;
  const auto notify = direct_drag_lost_;
  const auto panel_id = panel_.id;
  const auto x = panel_.anchor_x, y = panel_.anchor_y;
  dragging_ = false;
  drag_moved_ = false;
  set_direct_drag(false);
  // Disarm all panels before reporting the final anchor. The callbacks may
  // rebuild this window, so use copies and don't touch it again.
  if (notify) notify(panel_id);
  if (moved && finish) finish(panel_id, x, y, true);
}

void OverlayWindow::set_capture_exclusion(const bool enabled) {
  capture_exclusion_ = enabled;
  if (window_ != nullptr && IsWindow(window_)) {
    (void)layerc::platform::apply_capture_exclusion(window_, enabled);
  }
}

bool OverlayWindow::screen_bounds(RECT &bounds) const noexcept {
  bounds = {};
  return window_ != nullptr && visible_ && IsWindowVisible(window_) &&
         GetWindowRect(window_, &bounds) != FALSE;
}

OverlayWindow::~OverlayWindow() {
  if (window_ != nullptr && IsWindow(window_)) {
    DestroyWindow(window_);
  }
}

void OverlayWindow::calculate_size() {
  const auto geometry =
      layerc::core::panel_geometry(panel_, rows_.size());
  size_.cx = std::max(1L, scaled(geometry.size.width));
  size_.cy = std::max(1L, scaled(geometry.size.height));
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
  if (window_ == nullptr)
    return;
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
  if (window_ == nullptr)
    return;
  if (owner_ != nullptr) {
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, 0);
    owner_ = nullptr;
  }
  target_ = target;
  bring_to_front_ = true;
  update_position(target_);
  visible_ = true;
}

layerc::core::PanelPresentation OverlayWindow::present(
    const std::span<const layerc::core::CooldownState> states,
    const layerc::core::TimePoint now,
    const layerc::core::AutomationRuntimeSnapshot *automation,
    const layerc::core::TimePoint epoch) const {
  auto options = overrides_;
  options.presentation_epoch = epoch;
  return layerc::core::present_panel(panel_, cooldowns_, states, automation, options, now);
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
  // GDI resources stay alive so hide-when-ready bars reappear without
  // recreating their font and buffer; window destruction releases them.
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
    // Rendering calls this every frame; skip the move when the overlay
    // already sits visible at the target.
    RECT current{};
    if (IsWindowVisible(window_) != FALSE &&
        GetWindowRect(window_, &current) != FALSE && current.left == x &&
        current.top == y && current.right - current.left == size_.cx &&
        current.bottom - current.top == size_.cy)
      return;
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
    // Screen-space cursor for this message; any movement away from the press
    // point marks the drag as moved.
    const auto tracked_drag_point = [&] {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(window, &point);
      if (point.x != overlay->drag_start_.x ||
          point.y != overlay->drag_start_.y)
        overlay->drag_moved_ = true;
      return point;
    };
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
      if (GetCapture() != window) overlay->lose_direct_drag_capture();
      return 0;
    }
    if (message == WM_MOUSEMOVE && overlay->dragging_) {
      const POINT point = tracked_drag_point();
      if (overlay->drag_moved_) overlay->update_drag(point, false);
      return 0;
    }
    if (message == WM_LBUTTONUP && overlay->dragging_) {
      const POINT point = tracked_drag_point();
      if (overlay->drag_moved_) overlay->update_drag(point, true);
      overlay->dragging_ = false;
      overlay->drag_moved_ = false;
      if (GetCapture() == window) ReleaseCapture();
      return 0;
    }
    if (message == WM_CAPTURECHANGED) {
      // Mouse-up and explicit turn-off clear dragging_ before releasing.
      // Losing the capture mid-drag ends Direct drag itself.
      if (overlay->dragging_) overlay->lose_direct_drag_capture();
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
