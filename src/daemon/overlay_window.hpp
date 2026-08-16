#pragma once

#include "layerc/core/types.hpp"

#include <memory>
#include <optional>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

struct OverlayCooldown {
  std::string id;
  std::wstring name;
  layerc::core::Milliseconds duration{};
};

[[nodiscard]] std::vector<OverlayCooldown> make_overlay_cooldowns(
    std::span<const layerc::core::CooldownDefinition> definitions);

class OverlayWindow {
public:
  using DragCallback =
      std::function<void(std::string_view, double, double, bool)>;

  OverlayWindow(const layerc::core::Panel &panel,
                std::span<const OverlayCooldown> cooldowns);
  ~OverlayWindow();

  OverlayWindow(const OverlayWindow &) = delete;
  OverlayWindow &operator=(const OverlayWindow &) = delete;

  void show_for(HWND game_window);
  void show_on_rect(const RECT &target);
  void set_capture_exclusion(bool enabled);
  void update_snapshot(const layerc::core::Panel &panel,
                       std::span<const OverlayCooldown> cooldowns);
  void set_session_override(std::optional<bool> visible,
                            std::optional<std::wstring> text);
  void set_edit_mode(bool enabled, bool direct_drag, bool selected,
                     DragCallback callback = {});
  [[nodiscard]] std::string_view panel_id() const noexcept { return panel_.id; }
  [[nodiscard]] bool created() const noexcept {
    return window_ != nullptr && IsWindow(window_) != FALSE;
  }
  [[nodiscard]] bool screen_bounds(RECT &bounds) const noexcept;
  [[nodiscard]] bool
  should_show(const std::vector<layerc::core::CooldownState> &states) const;
  void hide();
  void render(const std::vector<layerc::core::CooldownState> &states,
              layerc::core::TimePoint now);

private:
  static constexpr LONG item_width = 78;
  static constexpr LONG item_height = 72;
  static constexpr LONG spacing = 6;
  static constexpr LONG padding = 6;
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
  void set_direct_drag(bool enabled);
  void update_drag(POINT screen_point, bool final);
  void clear_window_state();

  layerc::core::Panel panel_;
  std::vector<OverlayCooldown> cooldowns_;
  std::vector<std::size_t> cooldown_indices_;
  std::wstring label_buffer_;
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
  std::optional<bool> visibility_override_;
  std::optional<std::wstring> text_override_;
  bool dragging_{};
  bool drag_moved_{};
  int last_region_width_{-1};
  POINT drag_start_{};
  POINT drag_offset_{};
  DragCallback drag_callback_;

  HDC buffer_dc_{};
  HBITMAP buffer_bitmap_{};
  HGDIOBJ previous_bitmap_{};
  HFONT font_{};
  HBRUSH window_brush_{};
  HBRUSH panel_brush_{};
  HBRUSH fill_brush_{};
  HPEN border_pen_{};
  COLORREF text_color_{};
};

} // namespace layerc::daemon
