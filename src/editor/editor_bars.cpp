#include "editor_window.hpp"
#include "editor_resource.h"
#include "layerc/core/panel_geometry.hpp"

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

LRESULT CALLBACK EditorWindow::preview_subclass(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR subclass_id,
                                                DWORD_PTR reference) {
  auto *editor = reinterpret_cast<EditorWindow *>(reference);
  if (editor != nullptr && editor->handle_preview_message(window, message, wparam, lparam)) return 0;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &EditorWindow::preview_subclass, subclass_id);
  return DefSubclassProc(window, message, wparam, lparam);
}

layerc::core::Panel *EditorWindow::selected_bar() {
  const LRESULT row = SendMessageW(panel_preview_.list(), LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value = SendMessageW(panel_preview_.list(), LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 || static_cast<std::size_t>(value) >= document_.profile().panels.size()) return nullptr;
  return &document_.profile().panels[static_cast<std::size_t>(value)];
}

const layerc::core::Panel *EditorWindow::selected_bar() const {
  return const_cast<EditorWindow *>(this)->selected_bar();
}


void EditorWindow::refresh_bar_list(const std::size_t preferred_panel) {
  SendMessageW(panel_preview_.list(), LB_RESETCONTENT, 0, 0);
  int selected_row = -1;
  for (std::size_t index = 0; index < document_.profile().panels.size(); ++index) {
    const auto &panel = document_.profile().panels[index];
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    std::wstring label = to_wide(panel.name);
    if (panel.cooldown_ids.size() > 1)
      label += L"  (grouped: " + std::to_wstring(panel.cooldown_ids.size()) + L")";
    else if (!panel.cooldown_ids.empty()) label += L"  —  " + to_wide(panel.cooldown_ids.front());
    const auto row = SendMessageW(panel_preview_.list(), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (row != LB_ERR && row != LB_ERRSPACE) {
      SendMessageW(panel_preview_.list(), LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(index));
      if (index == preferred_panel) selected_row = static_cast<int>(row);
    }
  }
  if (selected_row < 0 && SendMessageW(panel_preview_.list(), LB_GETCOUNT, 0, 0) > 0) selected_row = 0;
  if (selected_row >= 0) SendMessageW(panel_preview_.list(), LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
  refresh_bar_controls();
}

void EditorWindow::refresh_bar_controls() {
  auto sync = shell_.sync_scope();
  const auto *panel = selected_bar();
  panel_preview_.set_controls_panel(panel == nullptr
                            ? SIZE_MAX
                            : static_cast<std::size_t>(panel - document_.profile().panels.data()));
  for (const auto &binding : profile_field_bindings())
    if (binding.owner == ProfileFieldOwner::panel)
      EnableWindow(GetDlgItem(shell_.window(), binding.control_id), panel != nullptr);
  for (const int id :
       {IDC_BAR_FILL, IDC_BAR_BACKGROUND, IDC_BAR_BORDER, IDC_BAR_TEXT})
    EnableWindow(GetDlgItem(shell_.window(), id), panel != nullptr);
  if (panel != nullptr) {
    const ProfileFieldContext context{
        shell_.window(), document_.profile(), selected_cooldown(),
        &document_.profile().panels[panel_preview_.controls_panel()]};
    write_profile_fields(context, ProfileFieldOwner::panel);
    set_text(GetDlgItem(shell_.window(), IDC_POSITION_STATUS), panel->cooldown_ids.size() > 1
      ? L"Legacy grouped bar. Split it to give each cooldown its own position."
      : L"Drag the selected bar in the preview to save normalized position.");
    set_text(GetDlgItem(shell_.window(), IDC_BAR_FILL), L"Fill");
    set_text(GetDlgItem(shell_.window(), IDC_BAR_BACKGROUND), L"BG");
    set_text(GetDlgItem(shell_.window(), IDC_BAR_BORDER), L"Border");
    set_text(GetDlgItem(shell_.window(), IDC_BAR_TEXT), L"Text");
  } else set_text(GetDlgItem(shell_.window(), IDC_POSITION_STATUS), L"Add a cooldown to create a bar.");
  InvalidateRect(panel_preview_.window(), nullptr, TRUE);
}


void EditorWindow::split_independent_bars() {
  sync_profile_from_controls();
  std::vector<layerc::core::Panel> split;
  split.reserve(document_.profile().panels.size() + document_.profile().cooldowns.size());
  bool changed{}; std::size_t first_split = SIZE_MAX;
  const auto target_height = std::max(1L, preview_target_size().cy);
  for (const auto &panel : document_.profile().panels) {
    if (panel.style != layerc::core::PanelStyle::bar || panel.cooldown_ids.size() <= 1) {
      split.push_back(panel); continue;
    }
    changed = true;
    const double offset =
        (static_cast<double>(panel.bar.height) +
         layerc::core::overlay_metrics.spacing) /
        target_height;
    for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index) {
      auto bar = panel;
      const auto &cooldown = panel.cooldown_ids[index];
      bar.id = unique_panel_id(panel.id + "-" + cooldown, document_.profile().panels, &split);
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
  document_.profile().panels = std::move(split);
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
  if (!choose_color(shell_.window(), *value)) return;
  refresh_bar_controls();
  record_profile_change(ChangeKind::immediate);
}

SIZE EditorWindow::preview_target_size() const {
  const auto expected_path = to_wide(document_.profile().game.executable_path);
  const auto expected_name = to_wide(document_.profile().game.executable_name);
  for (const auto &candidate : task_picker_.candidates()) {
    const bool path_matches = !expected_path.empty() &&
                              _wcsicmp(candidate.executable_path.c_str(), expected_path.c_str()) == 0;
    const bool name_matches = !expected_name.empty() &&
                              _wcsicmp(candidate.executable_name.c_str(), expected_name.c_str()) == 0;
    if (!path_matches && !name_matches) continue;
    RECT client{};
    const auto dpi = GetDpiForWindow(candidate.window);
    if (dpi != 0 && GetClientRect(candidate.window, &client) &&
        client.right > client.left && client.bottom > client.top)
      return {std::max(1, MulDiv(client.right - client.left, 96, static_cast<int>(dpi))),
              std::max(1, MulDiv(client.bottom - client.top, 96, static_cast<int>(dpi)))};
  }
  // No live target: use a documented 1920 x 1080 DIP reference.
  return {1920, 1080};
}

RECT EditorWindow::preview_canvas() const {
  RECT client{}; GetClientRect(panel_preview_.window(), &client);
  constexpr int inset = 12;
  const int available_width = std::max(1L, client.right - client.left - inset * 2);
  const int available_height = std::max(1L, client.bottom - client.top - inset * 2);
  const auto target = preview_target_size();
  const double aspect = static_cast<double>(target.cx) / std::max(1L, target.cy);
  int width = available_width;
  int height = static_cast<int>(std::lround(width / aspect));
  if (height > available_height) { height = available_height; width = static_cast<int>(std::lround(height * aspect)); }
  const int left = (client.right - width) / 2;
  const int top = (client.bottom - height) / 2;
  return {left, top, left + width, top + height};
}

RECT EditorWindow::bar_preview_rect(const layerc::core::Panel &panel, const RECT &canvas) const {
  const auto placement = preview_placement(
      panel, panel.cooldown_ids.size(), canvas, preview_target_size());
  const auto shape = layerc::core::bar_geometry(
      panel, 0, panel.cooldown_ids.size(), 3.0 / 5.0);
  return preview_rect(shape.outline, placement);
}

void EditorWindow::paint_preview(const HWND window) {
  PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
  const HGDIOBJ previous_font = SelectObject(dc, shell_.font());
  const auto preview_colors = colors(layerc::platform::EditorControlRole::preview);
  RECT client{}; GetClientRect(window, &client);
  fill_solid_rect(dc, client, preview_colors.fill);
  const RECT canvas = preview_canvas();
  fill_solid_rect(dc, canvas,
                  colors(layerc::platform::EditorControlRole::window).fill);
  outline_rect(dc, canvas, 1, preview_colors.border);
  const auto *selected = selected_bar();
  const auto target = preview_target_size();
  for (const auto &panel : document_.profile().panels) {
    const auto count = panel.cooldown_ids.size();
    const auto placement = preview_placement(panel, count, canvas, target);
    for (std::size_t index = 0; index < count; ++index) {
      layerc::core::PanelRect outline = placement.geometry.items[index];
      layerc::core::PanelRect fill_rect{};
      layerc::core::PanelRect text_rect = layerc::core::inset_panel_rect(
          outline, layerc::core::overlay_metrics.card_text_inset);
      int radius = layerc::core::overlay_metrics.fill_radius;
      bool draw_fill{};
      if (panel.style == layerc::core::PanelStyle::bar) {
        const auto shape = layerc::core::bar_geometry(
            panel, index, count, 3.0 / 5.0);
        outline = shape.outline;
        fill_rect = shape.fill;
        text_rect = shape.text;
        radius = shape.corner_radius;
        draw_fill = shape.grouped;
      }

      const RECT item = preview_rect(outline, placement);
      const HPEN border = CreatePen(
          PS_SOLID, selected == &panel ? 2 : 1,
          selected == &panel ? preview_colors.selection
                             : color_ref(panel.bar.border_color));
      const HBRUSH background = CreateSolidBrush(
          color_ref(panel.style == layerc::core::PanelStyle::bar && count == 1
                        ? panel.bar.fill_color
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
                        color_ref(panel.bar.fill_color));
      }
      if (panel.style == layerc::core::PanelStyle::bar &&
          !panel.bar.show_text)
        continue;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, color_ref(panel.bar.text_color));
      const auto label = to_wide(count == 1 ? panel.name
                                            : panel.cooldown_ids[index]);
      RECT text = preview_rect(text_rect, placement);
      DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
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
    auto bar = bar_preview_rect(*panel, preview_canvas());
    InflateRect(&bar, std::max(0L, (28 - (bar.right - bar.left)) / 2),
                std::max(0L, (14 - (bar.bottom - bar.top)) / 2));
    if (!PtInRect(&bar, point)) return false;
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
  }
  case WM_LBUTTONDOWN: {
    const auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return true;
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    auto bar = bar_preview_rect(*panel, preview_canvas());
    InflateRect(&bar, std::max(0L, (28 - (bar.right - bar.left)) / 2),
                std::max(0L, (14 - (bar.bottom - bar.top)) / 2));
    if (PtInRect(&bar, point)) {
      panel_preview_.begin_drag(
          point, {point.x - (bar.left + bar.right) / 2,
                  point.y - (bar.top + bar.bottom) / 2});
      SetCapture(window);
    }
    return true;
  }
  case WM_MOUSEMOVE:
    if (panel_preview_.dragging()) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != panel_preview_.drag_start().x || point.y != panel_preview_.drag_start().y)
        panel_preview_.mark_drag_moved();
      if (panel_preview_.drag_moved()) update_dragged_bar(point, false);
    }
    return panel_preview_.dragging();
  case WM_LBUTTONUP:
    if (panel_preview_.dragging()) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != panel_preview_.drag_start().x || point.y != panel_preview_.drag_start().y)
        panel_preview_.mark_drag_moved();
      if (panel_preview_.drag_moved()) update_dragged_bar(point, true);
      panel_preview_.finish_drag();
      if (GetCapture() == window) ReleaseCapture();
    }
    return true;
  case WM_CAPTURECHANGED:
    if (panel_preview_.dragging() && panel_preview_.drag_moved()) finalize_dragged_bar();
    panel_preview_.finish_drag();
    return true;
  default: return false;
  }
}

void EditorWindow::update_dragged_bar(const POINT point, const bool final) {
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto canvas = preview_canvas();
  const LONG center_x = point.x - panel_preview_.drag_offset().x;
  const LONG center_y = point.y - panel_preview_.drag_offset().y;
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
  InvalidateRect(panel_preview_.window(), nullptr, TRUE);
}

void EditorWindow::finalize_dragged_bar() {
  record_profile_change(ChangeKind::drag);
  set_status(document_.path().empty()
                 ? L"Bar position updated — Save once to create the profile."
                 : L"Bar position updated and autosaving.");
  send_live_anchor(true);
  InvalidateRect(panel_preview_.window(), nullptr, TRUE);
}

} // namespace layerc::editor
