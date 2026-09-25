#pragma once

#include "overlay_window.hpp"

#include "layerc/core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {

struct OverlayEditMode {
  bool direct_drag{};
  std::string_view selected_panel;
  OverlayWindow::DragCallback drag_callback;
};

class OverlayCollection {
public:
  [[nodiscard]] bool empty() const noexcept { return windows_.empty(); }
  [[nodiscard]] std::size_t object_count() const noexcept {
    return windows_.size();
  }
  [[nodiscard]] std::size_t window_count() const noexcept;
  void append_screen_bounds(std::vector<RECT> &output) const;
  void ensure(const layerc::core::GameProfile &profile,
              bool capture_exclusion);
  void rebuild(const layerc::core::GameProfile &profile,
               bool capture_exclusion,
               const OverlayEditMode *edit_mode = nullptr);
  void update_panel(const layerc::core::Panel &panel);
  void apply_session_override(const layerc::core::Panel &panel,
                              std::optional<bool> visible,
                              std::optional<std::wstring> text);
  void set_capture_exclusion(bool enabled);
  void hide() noexcept;
  void release() noexcept;
  void render_for(HWND target,
                  const std::vector<layerc::core::CooldownState> &states,
                  layerc::core::TimePoint now, bool always_show = false);
  void render_on_rect(const RECT &target,
                      const std::vector<layerc::core::CooldownState> &states,
                      layerc::core::TimePoint now);

private:
  std::vector<layerc::core::CooldownDefinition> cooldowns_;
  std::vector<std::unique_ptr<OverlayWindow>> windows_;
};

} // namespace layerc::daemon
