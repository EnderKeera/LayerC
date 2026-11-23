#pragma once

#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <memory>
#include <map>
#include <optional>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

class OverlayWindow {
public:
  using DragCallback =
      std::function<void(std::string_view, double, double, bool)>;
  // Direct drag ended because another window took the pointer capture during
  // a drag. The overlay is already click-through again when this runs.
  using DirectDragLost = std::function<void(std::string_view)>;

  OverlayWindow(const layerc::core::Panel &panel,
                std::span<const layerc::core::CooldownDefinition> cooldowns);
  ~OverlayWindow();

  OverlayWindow(const OverlayWindow &) = delete;
  OverlayWindow &operator=(const OverlayWindow &) = delete;

  void show_for(HWND game_window);
  void show_on_rect(const RECT &target);
  void set_capture_exclusion(bool enabled);
  void update_snapshot(const layerc::core::Panel &panel,
                       std::span<const layerc::core::CooldownDefinition> cooldowns);
  void set_session_override(std::optional<bool> visible,
                            std::optional<std::string> text,
                            std::vector<layerc::core::ItemPresentationOverride> items = {});
  [[nodiscard]] bool sync_rows(const layerc::core::PanelPresentation &presentation);
  void set_edit_mode(bool enabled, bool direct_drag, bool selected,
                     DragCallback callback = {}, DirectDragLost lost = {});
  [[nodiscard]] bool direct_drag() const noexcept { return direct_drag_; }
  [[nodiscard]] std::string_view panel_id() const noexcept { return panel_.id; }
  [[nodiscard]] bool created() const noexcept {
    return window_ != nullptr && IsWindow(window_) != FALSE;
  }
  [[nodiscard]] bool screen_bounds(RECT &bounds) const noexcept;
  // The shared core presentation of this panel for one frame.
  [[nodiscard]] layerc::core::PanelPresentation
  present(std::span<const layerc::core::CooldownState> states,
          layerc::core::TimePoint now,
          const layerc::core::AutomationRuntimeSnapshot *automation = nullptr,
          layerc::core::TimePoint epoch = {}) const;
  void hide();
  void render(const layerc::core::PanelPresentation &presentation);

private:
  friend struct OverlayWindowTestAccess;
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam);
  bool create_window();
  bool ensure_render_resources();
  void release_render_resources();
  void paint();
  void calculate_size();
  void update_dpi(UINT dpi);
  [[nodiscard]] LONG scaled(LONG value) const noexcept;
  void update_position(const RECT &target);
  [[nodiscard]] bool
  assign_rows(std::span<const layerc::core::CooldownDefinition> cooldowns);
  void set_direct_drag(bool enabled);
  void lose_direct_drag_capture();
  void update_drag(POINT screen_point, bool final);
  void clear_window_state();

  struct Row {
    std::string item_id;
    // Converted only when the core-provided label changes.
    std::string label;
    std::wstring wide_label;
  };

  layerc::core::Panel panel_;
  // The profile's definitions, positioned like engine snapshots.
  std::vector<layerc::core::CooldownDefinition> cooldowns_;
  std::vector<Row> rows_;
  HWND window_{};
  HWND owner_{};
  RECT target_{};
  bool bring_to_front_{};
  UINT dpi_{96};
  SIZE size_{};
  bool visible_{};
  bool capture_exclusion_{true};
  bool edit_mode_{};
  bool direct_drag_{};
  bool edit_selected_{};
  layerc::core::PanelPresentationOverrides overrides_;
  bool dragging_{};
  bool drag_moved_{};
  int last_region_width_{-1};
  POINT drag_start_{};
  POINT drag_offset_{};
  DragCallback drag_callback_;
  DirectDragLost direct_drag_lost_;

  HDC buffer_dc_{};
  HBITMAP buffer_bitmap_{};
  HGDIOBJ previous_bitmap_{};
  HFONT font_{};
  HBRUSH window_brush_{};
  HBRUSH panel_brush_{};
  HBRUSH fill_brush_{};
  HPEN border_pen_{};
  COLORREF text_color_{};
  // Cleared after each frame, so changing automation colors cannot grow GDI use.
  std::map<std::uint32_t, HBRUSH> row_brushes_;
};

} // namespace layerc::daemon
