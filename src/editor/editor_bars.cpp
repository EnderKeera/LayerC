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

const layerc::core::Panel *EditorWindow::selected_bar() const {
  const LRESULT row = SendMessageW(panel_preview_.list, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value = SendMessageW(panel_preview_.list, LB_GETITEMDATA,
                                     static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 ||
      static_cast<std::size_t>(value) >= document_.profile().panels.size())
    return nullptr;
  return &document_.profile().panels[static_cast<std::size_t>(value)];
}


void EditorWindow::refresh_bar_list(std::size_t preferred_panel) {
  SendMessageW(panel_preview_.list, LB_RESETCONTENT, 0, 0);
  int selected_row = -1;
  const auto &profile = document_.profile();
  if (preferred_panel == SIZE_MAX) {
    if (const auto *cooldown = cooldown_editor_.selected(); cooldown != nullptr) {
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
    const ProfileFieldView context{
        shell_.window, document_.profile(), cooldown_editor_.selected(),
        &document_.profile().panels[panel_preview_.controls_panel]};
    write_profile_fields(context, ProfileFieldOwner::panel);
    set_text(GetDlgItem(shell_.window, IDC_BAR_FILL), L"Fill");
    set_text(GetDlgItem(shell_.window, IDC_BAR_BACKGROUND), L"Background");
    set_text(GetDlgItem(shell_.window, IDC_BAR_BORDER), L"Border");
    set_text(GetDlgItem(shell_.window, IDC_BAR_TEXT), L"Text");
  }
  refresh_position_hint();
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
}

// The hint names only surfaces the window currently shows: narrow windows
// collapse the preview column, so they point at Position on window instead.
void EditorWindow::refresh_position_hint() {
  const auto *panel = selected_bar();
  const bool direct_drag_unavailable =
      IsWindowEnabled(GetDlgItem(shell_.window, IDC_DIRECT_DRAG)) == FALSE;
  const wchar_t *hint =
      panel == nullptr ? L"Add a cooldown to create a bar."
      : panel->cooldown_ids.size() > 1
          ? L"This bar is shared by several cooldowns. Tools > Split "
            L"Grouped Bars gives each its own position."
      : shell_.preview_visible
          ? (direct_drag_unavailable
                 ? L"Drag the bar in the preview to position it. Direct drag "
                   L"works during Position on window."
                 : L"Drag the bar in the preview to position it.")
      : direct_drag_unavailable
          ? L"Use Position on window to place the bar over its window. "
            L"Widen the editor to show the preview."
          : L"Drag the bar over its window to position it. Widen the "
            L"editor to show the preview.";
  set_text(GetDlgItem(shell_.window, IDC_POSITION_STATUS), hint);
}


void EditorWindow::follow_cooldown_bar() {
  const auto *cooldown = cooldown_editor_.selected();
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
  // The split itself is shared with the Linux editor.
  std::optional<std::size_t> first_split;
  (void)apply_profile_change(
      ChangeKind::immediate, [&](layerc::core::GameProfile &profile) {
        first_split = split_grouped_bars(profile, preview_target_size().cy);
      });
  if (!first_split) { set_status(L"Bars are already independent; legacy groups were left unchanged."); return; }
  refresh_bar_list(*first_split);
  set_status(L"Split grouped bars into independently positioned cooldown bars.");
}

void EditorWindow::choose_bar_color(const int id) {
  if (selected_bar() == nullptr) return;
  const auto panel_index = panel_preview_.controls_panel;
  (void)apply_profile_change(
      ChangeKind::immediate, [&](layerc::core::GameProfile &profile) {
        if (panel_index >= profile.panels.size()) return;
        auto &bar = profile.panels[panel_index].bar;
        std::uint32_t *value{};
        switch (id) {
        case IDC_BAR_FILL: value = &bar.fill_color; break;
        case IDC_BAR_BACKGROUND: value = &bar.background_color; break;
        case IDC_BAR_BORDER: value = &bar.border_color; break;
        case IDC_BAR_TEXT: value = &bar.text_color; break;
        default: return;
        }
        std::uint32_t chosen = *value;
        if (!choose_color(shell_.window, chosen)) return;
        *value = chosen;
      });
  refresh_bar_controls();
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
  const int dpi = static_cast<int>(native_window_dpi(panel_preview_.window));
  const int bar_height =
      static_cast<int>(std::lround(geometry.size.height * dpi / 96.0));
  RECT client{}; GetClientRect(panel_preview_.window, &client);
  // Only when it leaves at least half the preview for the scaled canvas.
  const int needed = bar_height + MulDiv(40, dpi, 96);
  return needed * 2 <= client.bottom - client.top ? needed : 0;
}

RECT EditorWindow::bar_preview_rect(const layerc::core::Panel &panel, const RECT &canvas) const {
  const auto effective = preview_panel(panel);
  const auto placement = preview_placement(
      effective, effective.cooldown_ids.size(), canvas, preview_target_size());
  const auto shape = layerc::core::bar_geometry(
      effective, 0, effective.cooldown_ids.size(), 3.0 / 5.0);
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
    const bool is_selected = selected != nullptr && selected->id == panel.id;
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
          PS_SOLID, is_selected ? 2 : 1,
          is_selected ? preview_colors.selection
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
  for (const auto &panel : document_.profile().panels) {
    const auto effective = preview_panel(panel);
    draw_panel(effective,
               preview_placement(effective, effective.cooldown_ids.size(),
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
    const auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return true;
    auto effective = preview_panel(*panel);
    const auto target = preview_target_size();
    const double step = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10.0 : 1.0;
    if (wparam == VK_LEFT || wparam == VK_RIGHT)
      effective.anchor_x = std::clamp(
          effective.anchor_x + (wparam == VK_LEFT ? -step : step) /
                                   std::max(1L, target.cx),
          0.0, 1.0);
    else
      effective.anchor_y = std::clamp(
          effective.anchor_y + (wparam == VK_UP ? -step : step) /
                                   std::max(1L, target.cy),
          0.0, 1.0);
    panel_preview_.pending_anchor = PanelPreview::PendingAnchor{
        effective.id, effective.anchor_x, effective.anchor_y};
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

layerc::core::Panel EditorWindow::preview_panel(
    const layerc::core::Panel &panel) const {
  auto effective = panel;
  if (panel_preview_.pending_anchor &&
      panel_preview_.pending_anchor->panel_id == panel.id) {
    effective.anchor_x = panel_preview_.pending_anchor->x;
    effective.anchor_y = panel_preview_.pending_anchor->y;
  }
  return effective;
}

bool EditorWindow::commit_pending_anchor(const bool publish_final) {
  if (!panel_preview_.pending_anchor) return false;
  const auto pending = *panel_preview_.pending_anchor;
  panel_preview_.pending_anchor.reset();
  const auto existing = std::ranges::find(document_.profile().panels,
                                          pending.panel_id,
                                          &layerc::core::Panel::id);
  if (existing == document_.profile().panels.end() ||
      (existing->anchor_x == pending.x && existing->anchor_y == pending.y)) {
    refresh_bar_controls();
    return false;
  }
  const bool accepted = apply_profile_change(
      DocumentChangeKind::drag,
      [&](layerc::core::GameProfile &candidate) {
        const auto panel = std::ranges::find(candidate.panels, pending.panel_id,
                                             &layerc::core::Panel::id);
        if (panel == candidate.panels.end()) return;
        panel->anchor_x = pending.x;
        panel->anchor_y = pending.y;
      });
  refresh_bar_controls();
  if (!accepted) return false;
  set_status(document_.path().empty()
                 ? L"Bar position updated — Save once to create the profile."
                 : L"Bar position updated and autosaving.");
  if (publish_final) send_live_anchor(true);
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
  return true;
}

void EditorWindow::update_dragged_bar(const POINT point, const bool final) {
  const auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto canvas = preview_canvas();
  const LONG center_x = point.x - panel_preview_.drag_offset.x;
  const LONG center_y = point.y - panel_preview_.drag_offset.y;
  // Transient preview only: the document keeps its committed anchors.
  panel_preview_.pending_anchor = PanelPreview::PendingAnchor{
      panel->id,
      std::clamp((center_x - canvas.left) /
                     static_cast<double>(std::max(1L, canvas.right - canvas.left)),
                 0.0, 1.0),
      std::clamp((center_y - canvas.top) /
                     static_cast<double>(std::max(1L, canvas.bottom - canvas.top)),
                 0.0, 1.0)};
  if (final) {
    finalize_dragged_bar();
  } else {
    send_live_anchor(false);
  }
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
}

void EditorWindow::finalize_dragged_bar() {
  (void)commit_pending_anchor(true);
}

} // namespace layerc::editor
