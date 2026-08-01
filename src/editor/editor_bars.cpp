#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <windowsx.h>
namespace layerc::editor {
LRESULT CALLBACK EditorWindow::preview_subclass(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR subclass_id,
                                                DWORD_PTR reference) {
  auto *editor = reinterpret_cast<EditorWindow *>(reference);
  if (editor != nullptr && editor->handle_preview_message(window, message, wparam, lparam)) return 0;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &EditorWindow::preview_subclass, subclass_id);
  return DefSubclassProc(window, message, wparam, lparam);
}

layerc::core::Panel *EditorWindow::selected_bar() {
  const LRESULT row = SendMessageW(bar_list_, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value = SendMessageW(bar_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 || static_cast<std::size_t>(value) >= profile_.panels.size()) return nullptr;
  return &profile_.panels[static_cast<std::size_t>(value)];
}

const layerc::core::Panel *EditorWindow::selected_bar() const {
  return const_cast<EditorWindow *>(this)->selected_bar();
}


void EditorWindow::refresh_bar_list(const std::size_t preferred_panel) {
  SendMessageW(bar_list_, LB_RESETCONTENT, 0, 0);
  int selected_row = -1;
  for (std::size_t index = 0; index < profile_.panels.size(); ++index) {
    const auto &panel = profile_.panels[index];
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    std::wstring label = to_wide(panel.name);
    if (panel.cooldown_ids.size() > 1)
      label += L"  (grouped: " + std::to_wstring(panel.cooldown_ids.size()) + L")";
    else if (!panel.cooldown_ids.empty()) label += L"  —  " + to_wide(panel.cooldown_ids.front());
    const auto row = SendMessageW(bar_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (row != LB_ERR && row != LB_ERRSPACE) {
      SendMessageW(bar_list_, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(index));
      if (index == preferred_panel) selected_row = static_cast<int>(row);
    }
  }
  if (selected_row < 0 && SendMessageW(bar_list_, LB_GETCOUNT, 0, 0) > 0) selected_row = 0;
  if (selected_row >= 0) SendMessageW(bar_list_, LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
  refresh_bar_controls();
}

void EditorWindow::refresh_bar_controls() {
  syncing_ = true;
  const auto *panel = selected_bar();
  bar_controls_panel_ = panel == nullptr
                            ? SIZE_MAX
                            : static_cast<std::size_t>(panel - profile_.panels.data());
  for (const int id : {IDC_BAR_WIDTH, IDC_BAR_HEIGHT, IDC_BAR_FILL, IDC_BAR_BACKGROUND,
                       IDC_BAR_BORDER, IDC_BAR_TEXT, IDC_SHOW_TEXT, IDC_HIDE_READY})
    EnableWindow(GetDlgItem(dialog_, id), panel != nullptr);
  if (panel != nullptr) {
    set_text(GetDlgItem(dialog_, IDC_BAR_WIDTH), std::to_wstring(panel->bar.width));
    set_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT), std::to_wstring(panel->bar.height));
    set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_SHOW_TEXT),
                            panel->bar.show_text);
    set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_HIDE_READY),
                            panel->bar.hide_when_ready);
    set_text(GetDlgItem(dialog_, IDC_POSITION_STATUS), panel->cooldown_ids.size() > 1
      ? L"Legacy grouped bar. Split it to give each cooldown its own position."
      : L"Drag the selected bar in the preview to save normalized position.");
    set_text(GetDlgItem(dialog_, IDC_BAR_FILL), color_label(L"Fill", panel->bar.fill_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_BACKGROUND), color_label(L"Background", panel->bar.background_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_BORDER), color_label(L"Border", panel->bar.border_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_TEXT), color_label(L"Text", panel->bar.text_color));
  } else set_text(GetDlgItem(dialog_, IDC_POSITION_STATUS), L"Add a cooldown to create a bar.");
  syncing_ = false;
  InvalidateRect(preview_, nullptr, TRUE);
}


void EditorWindow::split_independent_bars() {
  sync_profile_from_controls();
  std::vector<layerc::core::Panel> split;
  split.reserve(profile_.panels.size() + profile_.cooldowns.size());
  bool changed{}; std::size_t first_split = SIZE_MAX;
  for (const auto &panel : profile_.panels) {
    if (panel.style != layerc::core::PanelStyle::bar || panel.cooldown_ids.size() <= 1) {
      split.push_back(panel); continue;
    }
    changed = true;
    const double offset = (static_cast<double>(panel.bar.height) + 8.0) / 900.0;
    for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index) {
      auto bar = panel;
      const auto &cooldown = panel.cooldown_ids[index];
      bar.id = unique_panel_id(panel.id + "-" + cooldown, profile_.panels, &split);
      bar.name = panel.name + " " + cooldown;
      bar.cooldown_ids = {cooldown};
      bar.anchor_y = std::clamp(panel.anchor_y +
          (static_cast<double>(index) - (static_cast<double>(panel.cooldown_ids.size()) - 1.0) / 2.0) * offset,
          0.0, 1.0);
      if (first_split == SIZE_MAX) first_split = split.size();
      split.push_back(std::move(bar));
    }
  }
  if (!changed) { set_status(L"Bars are already independent; legacy groups were left unchanged."); return; }
  profile_.panels = std::move(split);
  refresh_bar_list(first_split);
  record_profile_change(ChangeKind::immediate);
  set_status(L"Split grouped bars into independently positioned cooldown bars.");
}

void EditorWindow::choose_bar_color(const int id) {
  sync_profile_from_controls();
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  std::uint32_t *value{};
  switch (id) {
  case IDC_BAR_FILL: value = &panel->bar.fill_color; break;
  case IDC_BAR_BACKGROUND: value = &panel->bar.background_color; break;
  case IDC_BAR_BORDER: value = &panel->bar.border_color; break;
  case IDC_BAR_TEXT: value = &panel->bar.text_color; break;
  default: return;
  }
  if (!choose_color(dialog_, *value)) return;
  refresh_bar_controls();
  record_profile_change(ChangeKind::immediate);
}

double EditorWindow::preview_aspect_ratio() const {
  const auto expected_path = to_wide(profile_.game.executable_path);
  const auto expected_name = to_wide(profile_.game.executable_name);
  for (const auto &candidate : task_candidates_) {
    const bool path_matches = !expected_path.empty() &&
                              _wcsicmp(candidate.executable_path.c_str(), expected_path.c_str()) == 0;
    const bool name_matches = !expected_name.empty() &&
                              _wcsicmp(candidate.executable_name.c_str(), expected_name.c_str()) == 0;
    if (!path_matches && !name_matches) continue;
    RECT client{};
    if (GetClientRect(candidate.window, &client) && client.right > client.left && client.bottom > client.top)
      return static_cast<double>(client.right - client.left) / static_cast<double>(client.bottom - client.top);
  }
  return 16.0 / 9.0;
}

RECT EditorWindow::preview_canvas() const {
  RECT client{}; GetClientRect(preview_, &client);
  constexpr int inset = 12;
  const int available_width = std::max(1L, client.right - client.left - inset * 2);
  const int available_height = std::max(1L, client.bottom - client.top - inset * 2);
  const double aspect = preview_aspect_ratio();
  int width = available_width;
  int height = static_cast<int>(std::lround(width / aspect));
  if (height > available_height) { height = available_height; width = static_cast<int>(std::lround(height * aspect)); }
  const int left = (client.right - width) / 2;
  const int top = (client.bottom - height) / 2;
  return {left, top, left + width, top + height};
}

RECT EditorWindow::bar_preview_rect(const layerc::core::Panel &panel, const RECT &canvas) const {
  const auto canvas_width = std::max(1L, canvas.right - canvas.left);
  const auto canvas_height = std::max(1L, canvas.bottom - canvas.top);
  const auto scale = std::min(static_cast<double>(canvas_width) / 1920.0,
                              static_cast<double>(canvas_height) / 1080.0);
  const int width = std::max(28, static_cast<int>(std::lround(panel.bar.width * scale)));
  const int height = std::max(14, static_cast<int>(std::lround(panel.bar.height * scale)));
  const int center_x = canvas.left + static_cast<int>(std::lround(panel.anchor_x * canvas_width));
  const int center_y = canvas.top + static_cast<int>(std::lround(panel.anchor_y * canvas_height));
  return {center_x - width / 2, center_y - height / 2, center_x - width / 2 + width, center_y - height / 2 + height};
}

void EditorWindow::paint_preview(const HWND window) {
  PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
  const HGDIOBJ previous_font = SelectObject(dc, font_);
  const auto preview_colors = colors(layerc::platform::EditorControlRole::preview);
  RECT client{}; GetClientRect(window, &client);
  const HBRUSH fill = CreateSolidBrush(preview_colors.fill); FillRect(dc, &client, fill); DeleteObject(fill);
  const RECT canvas = preview_canvas();
  const HBRUSH canvas_fill = CreateSolidBrush(colors(layerc::platform::EditorControlRole::window).fill);
  FillRect(dc, &canvas, canvas_fill); DeleteObject(canvas_fill);
  const HPEN canvas_pen = CreatePen(PS_SOLID, 1, preview_colors.border);
  const HGDIOBJ old_pen = SelectObject(dc, canvas_pen); const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
  SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(canvas_pen);
  const auto *selected = selected_bar();
  for (const auto &panel : profile_.panels) {
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    const RECT bar = bar_preview_rect(panel, canvas);
    const HBRUSH background = CreateSolidBrush(color_ref(panel.bar.background_color)); FillRect(dc, &bar, background); DeleteObject(background);
    const RECT progress{bar.left, bar.top, std::max(bar.left + 1, bar.left + (bar.right - bar.left) * 3 / 5), bar.bottom};
    const HBRUSH progress_fill = CreateSolidBrush(color_ref(panel.bar.fill_color)); FillRect(dc, &progress, progress_fill); DeleteObject(progress_fill);
    const HPEN border = CreatePen(PS_SOLID, selected == &panel ? 2 : 1,
                                  selected == &panel ? preview_colors.selection : color_ref(panel.bar.border_color));
    const HGDIOBJ previous = SelectObject(dc, border); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom); SelectObject(dc, previous); DeleteObject(border);
    if (panel.bar.show_text) {
      SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color_ref(panel.bar.text_color));
      const auto name = to_wide(panel.name); RECT text = bar; text.left += 4; text.right -= 4;
      DrawTextW(dc, name.c_str(), static_cast<int>(name.size()), &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }
  SelectObject(dc, previous_font);
  EndPaint(window, &paint);
}

bool EditorWindow::handle_preview_message(const HWND window, const UINT message,
                                          const WPARAM, const LPARAM lparam) {
  switch (message) {
  case WM_PAINT: paint_preview(window); return true;
  case WM_ERASEBKGND: return true;
  case WM_SETCURSOR: {
    if (LOWORD(lparam) != HTCLIENT) return false;
    const auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return false;
    POINT point{};
    if (GetCursorPos(&point) == FALSE ||
        ScreenToClient(window, &point) == FALSE)
      return false;
    const auto bar = bar_preview_rect(*panel, preview_canvas());
    if (!PtInRect(&bar, point)) return false;
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
  }
  case WM_LBUTTONDOWN: {
    const auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return true;
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const auto bar = bar_preview_rect(*panel, preview_canvas());
    if (PtInRect(&bar, point)) {
      dragging_bar_ = true;
      bar_drag_moved_ = false;
      bar_drag_start_ = point;
      bar_drag_offset_ = {point.x - (bar.left + bar.right) / 2,
                          point.y - (bar.top + bar.bottom) / 2};
      SetCapture(window);
    }
    return true;
  }
  case WM_MOUSEMOVE:
    if (dragging_bar_) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != bar_drag_start_.x || point.y != bar_drag_start_.y)
        bar_drag_moved_ = true;
      if (bar_drag_moved_) update_dragged_bar(point, false);
    }
    return dragging_bar_;
  case WM_LBUTTONUP:
    if (dragging_bar_) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != bar_drag_start_.x || point.y != bar_drag_start_.y)
        bar_drag_moved_ = true;
      if (bar_drag_moved_) update_dragged_bar(point, true);
      dragging_bar_ = false;
      bar_drag_moved_ = false;
      if (GetCapture() == window) ReleaseCapture();
    }
    return true;
  case WM_CAPTURECHANGED:
    if (dragging_bar_ && bar_drag_moved_) finalize_dragged_bar();
    dragging_bar_ = false;
    bar_drag_moved_ = false;
    return true;
  default: return false;
  }
}

void EditorWindow::update_dragged_bar(const POINT point, const bool final) {
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto canvas = preview_canvas();
  const LONG center_x = point.x - bar_drag_offset_.x;
  const LONG center_y = point.y - bar_drag_offset_.y;
  panel->anchor_x = std::clamp(
      (center_x - canvas.left) /
          static_cast<double>(std::max(1L, canvas.right - canvas.left)),
      0.0, 1.0);
  panel->anchor_y = std::clamp(
      (center_y - canvas.top) /
          static_cast<double>(std::max(1L, canvas.bottom - canvas.top)),
      0.0, 1.0);
  if (final) {
    finalize_dragged_bar();
  } else {
    send_live_anchor(false);
  }
  InvalidateRect(preview_, nullptr, TRUE);
}

void EditorWindow::finalize_dragged_bar() {
  record_profile_change(ChangeKind::drag);
  set_status(current_path_.empty()
                 ? L"Bar position updated — Save once to create the profile."
                 : L"Bar position updated and autosaving.");
  send_live_anchor(true);
  InvalidateRect(preview_, nullptr, TRUE);
}

} // namespace layerc::editor
