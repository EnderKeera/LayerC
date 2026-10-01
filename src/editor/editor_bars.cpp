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
  // Arrow keys nudge the selected bar; Tab still leaves the preview.
  if (message == WM_GETDLGCODE)
    return DefSubclassProc(window, message, wparam, lparam) | DLGC_WANTARROWS;
  if (editor != nullptr && editor->handle_preview_message(window, message, wparam, lparam)) return 0;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &EditorWindow::preview_subclass, subclass_id);
  return DefSubclassProc(window, message, wparam, lparam);
}

layerc::core::Panel *EditorWindow::selected_bar() {
  const LRESULT row = SendMessageW(panel_preview_.list, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value = SendMessageW(panel_preview_.list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 || static_cast<std::size_t>(value) >= document_.profile().panels.size()) return nullptr;
  return &document_.profile().panels[static_cast<std::size_t>(value)];
}

const layerc::core::Panel *EditorWindow::selected_bar() const {
  return const_cast<EditorWindow *>(this)->selected_bar();
}


void EditorWindow::refresh_bar_list(std::size_t preferred_panel) {
  SendMessageW(panel_preview_.list, LB_RESETCONTENT, 0, 0);
  int selected_row = -1;
  const auto &profile = document_.profile();
  if (preferred_panel == SIZE_MAX) {
    if (const auto *cooldown = selected_cooldown(); cooldown != nullptr) {
      const auto owner = std::ranges::find_if(
          profile.panels, [&](const layerc::core::Panel &panel) {
            return std::ranges::find(panel.cooldown_ids, cooldown->id) !=
                   panel.cooldown_ids.end();
          });
      if (owner != profile.panels.end())
        preferred_panel =
            static_cast<std::size_t>(owner - profile.panels.begin());
    }
  }
  for (std::size_t index = 0; index < profile.panels.size(); ++index) {
    const auto &panel = profile.panels[index];
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    // Rows name the cooldowns a bar shows, never their internal ids.
    std::wstring label = to_wide(panel.name);
    std::wstring shown;
    for (const auto &id : panel.cooldown_ids) {
      const auto cooldown = std::ranges::find(
          profile.cooldowns, id, &layerc::core::CooldownDefinition::id);
      if (cooldown == profile.cooldowns.end())
        continue;
      if (!shown.empty())
        shown += L", ";
      shown += to_wide(cooldown->name);
    }
    if (!shown.empty() && shown != label)
      label += L"  ·  " + shown;
    const auto row = SendMessageW(panel_preview_.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (row != LB_ERR && row != LB_ERRSPACE) {
      SendMessageW(panel_preview_.list, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(index));
      if (index == preferred_panel) selected_row = static_cast<int>(row);
    }
  }
  if (selected_row < 0 && SendMessageW(panel_preview_.list, LB_GETCOUNT, 0, 0) > 0) selected_row = 0;
  if (selected_row >= 0) SendMessageW(panel_preview_.list, LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
  refresh_bar_controls();
}

void EditorWindow::refresh_bar_controls() {
  auto sync = shell_.sync_scope();
  const auto *panel = selected_bar();
  panel_preview_.controls_panel = panel == nullptr
                            ? SIZE_MAX
                            : static_cast<std::size_t>(panel - document_.profile().panels.data());
  for (const auto &binding : profile_field_bindings())
    if (binding.owner == ProfileFieldOwner::panel)
      EnableWindow(GetDlgItem(shell_.window, binding.control_id), panel != nullptr);
  for (const int id :
       {IDC_BAR_FILL, IDC_BAR_BACKGROUND, IDC_BAR_BORDER, IDC_BAR_TEXT})
    EnableWindow(GetDlgItem(shell_.window, id), panel != nullptr);
  if (panel != nullptr) {
    const ProfileFieldContext context{
        shell_.window, document_.profile(), selected_cooldown(),
        &document_.profile().panels[panel_preview_.controls_panel]};
    write_profile_fields(context, ProfileFieldOwner::panel);
    const bool direct_drag_unavailable =
        IsWindowEnabled(GetDlgItem(shell_.window, IDC_DIRECT_DRAG)) == FALSE;
    set_text(GetDlgItem(shell_.window, IDC_POSITION_STATUS),
             panel->cooldown_ids.size() > 1
                 ? L"This bar is shared by several cooldowns. Tools > Split "
                   L"Grouped Bars gives each its own position."
             : direct_drag_unavailable
                 ? L"Drag the bar in the preview to position it. Direct drag "
                   L"works during Position on window."
                 : L"Drag the bar in the preview to position it.");
    set_text(GetDlgItem(shell_.window, IDC_BAR_FILL), L"Fill");
    set_text(GetDlgItem(shell_.window, IDC_BAR_BACKGROUND), L"Background");
    set_text(GetDlgItem(shell_.window, IDC_BAR_BORDER), L"Border");
    set_text(GetDlgItem(shell_.window, IDC_BAR_TEXT), L"Text");
  } else set_text(GetDlgItem(shell_.window, IDC_POSITION_STATUS), L"Add a cooldown to create a bar.");
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
}


void EditorWindow::follow_cooldown_bar() {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || panel_preview_.list == nullptr)
    return;
  const auto &panels = document_.profile().panels;
  const LRESULT count = SendMessageW(panel_preview_.list, LB_GETCOUNT, 0, 0);
  for (LRESULT row = 0; row < count; ++row) {
    const LRESULT index = SendMessageW(panel_preview_.list, LB_GETITEMDATA,
                                       static_cast<WPARAM>(row), 0);
    if (index < 0 || static_cast<std::size_t>(index) >= panels.size())
      continue;
    const auto &ids = panels[static_cast<std::size_t>(index)].cooldown_ids;
    if (std::ranges::find(ids, cooldown->id) == ids.end())
      continue;
    if (SendMessageW(panel_preview_.list, LB_GETCURSEL, 0, 0) != row) {
      SendMessageW(panel_preview_.list, LB_SETCURSEL, static_cast<WPARAM>(row),
                   0);
      refresh_bar_controls();
    }
    return;
  }
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
  if (!choose_color(shell_.window, *value)) return;
  refresh_bar_controls();
  record_profile_change(ChangeKind::immediate);
}

SIZE EditorWindow::preview_target_size() const {
  const auto expected_path = to_wide(document_.profile().game.executable_path);
  const auto expected_name = to_wide(document_.profile().game.executable_name);
  for (const auto &candidate : task_picker_.candidates) {
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
  RECT client{}; GetClientRect(panel_preview_.window, &client);
  constexpr int inset = 12;
  const int available_width = std::max(1L, client.right - client.left - inset * 2);
  // The strip under the canvas shows the selected bar at actual size.
  const int reserved = preview_actual_size_height();
  const int available_height =
      std::max(1L, client.bottom - client.top - inset * 2 - reserved);
  const auto target = preview_target_size();
  const double aspect = static_cast<double>(target.cx) / std::max(1L, target.cy);
  int width = available_width;
  int height = static_cast<int>(std::lround(width / aspect));
  if (height > available_height) { height = available_height; width = static_cast<int>(std::lround(height * aspect)); }
  const int left = (client.right - width) / 2;
  const int top = inset + (available_height - height) / 2;
  return {left, top, left + width, top + height};
}

int EditorWindow::preview_actual_size_height() const {
  const auto *panel = selected_bar();
  if (panel == nullptr || panel_preview_.window == nullptr)
    return 0;
  const auto geometry =
      layerc::core::panel_geometry(*panel, panel->cooldown_ids.size());
  const int dpi = static_cast<int>(GetDpiForWindow(panel_preview_.window));
  const int bar_height = static_cast<int>(
      std::lround(geometry.size.height * (dpi == 0 ? 96 : dpi) / 96.0));
  RECT client{}; GetClientRect(panel_preview_.window, &client);
  // Only when it leaves at least half the preview for the scaled canvas.
  const int needed = bar_height + MulDiv(40, dpi == 0 ? 96 : dpi, 96);
  return needed * 2 <= client.bottom - client.top ? needed : 0;
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
    const auto count = panel.cooldown_ids.size();
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
                             : native_color_ref(panel.bar.border_color));
      const HBRUSH background = CreateSolidBrush(
          native_color_ref(panel.style == layerc::core::PanelStyle::bar && count == 1
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
                        native_color_ref(panel.bar.fill_color));
      }
      if (panel.style == layerc::core::PanelStyle::bar &&
          !panel.bar.show_text)
        continue;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, native_color_ref(panel.bar.text_color));
      const auto label = to_wide(count == 1 ? panel.name
                                            : panel.cooldown_ids[index]);
      RECT text = preview_rect(text_rect, placement);
      DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    }
  };
  for (const auto &panel : document_.profile().panels)
    draw_panel(panel, preview_placement(panel, panel.cooldown_ids.size(),
                                        canvas, target));
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
    PreviewPlacement actual;
    actual.geometry = layerc::core::panel_geometry(
        *selected, selected->cooldown_ids.size());
    actual.scale = (dpi == 0 ? 96.0 : static_cast<double>(dpi)) / 96.0;
    actual.left = client.left + pad;
    actual.top = label.bottom + MulDiv(4, static_cast<int>(dpi), 96);
    HRGN clip = CreateRectRgn(client.left, label.bottom, client.right - pad,
                              client.bottom);
    SelectClipRgn(dc, clip);
    draw_panel(*selected, actual);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
  }
  if (GetFocus() == window)
    layerc::platform::paint_focus_ring(dc, client, preview_colors.accent,
                                       GetDpiForWindow(window));
  SelectObject(dc, previous_font);
  EndPaint(window, &paint);
}

bool EditorWindow::handle_preview_message(const HWND window, const UINT message,
                                          const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_PAINT: paint_preview(window); return true;
  case WM_ERASEBKGND: return true;
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    InvalidateRect(window, nullptr, FALSE);
    return false;

  case WM_KEYDOWN: {
    if (wparam != VK_LEFT && wparam != VK_RIGHT && wparam != VK_UP &&
        wparam != VK_DOWN)
      return false;
    auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return true;
    const auto target = preview_target_size();
    const double step = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10.0 : 1.0;
    if (wparam == VK_LEFT || wparam == VK_RIGHT)
      panel->anchor_x = std::clamp(
          panel->anchor_x + (wparam == VK_LEFT ? -step : step) /
                                std::max(1L, target.cx),
          0.0, 1.0);
    else
      panel->anchor_y = std::clamp(
          panel->anchor_y + (wparam == VK_UP ? -step : step) /
                                std::max(1L, target.cy),
          0.0, 1.0);
    finalize_dragged_bar();
    return true;
  }
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
    if (panel_preview_.dragging) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != panel_preview_.drag_start.x || point.y != panel_preview_.drag_start.y)
        panel_preview_.mark_drag_moved();
      if (panel_preview_.drag_moved) update_dragged_bar(point, false);
    }
    return panel_preview_.dragging;
  case WM_LBUTTONUP:
    if (panel_preview_.dragging) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != panel_preview_.drag_start.x || point.y != panel_preview_.drag_start.y)
        panel_preview_.mark_drag_moved();
      if (panel_preview_.drag_moved) update_dragged_bar(point, true);
      panel_preview_.finish_drag();
      if (GetCapture() == window) ReleaseCapture();
    }
    return true;
  case WM_CAPTURECHANGED:
    if (panel_preview_.dragging && panel_preview_.drag_moved) finalize_dragged_bar();
    panel_preview_.finish_drag();
    return true;
  default: return false;
  }
}

void EditorWindow::update_dragged_bar(const POINT point, const bool final) {
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto canvas = preview_canvas();
  const LONG center_x = point.x - panel_preview_.drag_offset.x;
  const LONG center_y = point.y - panel_preview_.drag_offset.y;
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
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
}

void EditorWindow::finalize_dragged_bar() {
  record_profile_change(ChangeKind::drag);
  set_status(document_.path().empty()
                 ? L"Bar position updated — Save once to create the profile."
                 : L"Bar position updated and autosaving.");
  send_live_anchor(true);
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
}

} // namespace layerc::editor
