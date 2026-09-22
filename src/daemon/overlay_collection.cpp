#include "overlay_collection.hpp"

#include <algorithm>

namespace layerc::daemon {

std::size_t OverlayCollection::window_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      windows_, [](const auto &overlay) {
        return overlay != nullptr && overlay->created();
      }));
}

void OverlayCollection::append_screen_bounds(std::vector<RECT> &output) const {
  for (const auto &overlay : windows_) {
    RECT bounds{};
    if (overlay != nullptr && overlay->screen_bounds(bounds))
      output.push_back(bounds);
  }
}

void OverlayCollection::ensure(const layerc::core::GameProfile &profile,
                               const bool capture_exclusion) {
  if (windows_.empty() && !profile.panels.empty())
    rebuild(profile, capture_exclusion);
}

void OverlayCollection::rebuild(const layerc::core::GameProfile &profile,
                                const bool capture_exclusion,
                                const OverlayEditMode *const edit_mode) {
  cooldowns_ = profile.cooldowns;
  std::vector<std::unique_ptr<OverlayWindow>> next;
  next.reserve(profile.panels.size());
  for (const auto &panel : profile.panels) {
    const auto found = std::ranges::find_if(
        windows_, [&](const auto &candidate) {
          return candidate != nullptr && candidate->panel_id() == panel.id;
        });
    std::unique_ptr<OverlayWindow> overlay;
    if (found == windows_.end())
      overlay = std::make_unique<OverlayWindow>(panel, cooldowns_);
    else {
      overlay = std::move(*found);
      overlay->update_snapshot(panel, cooldowns_);
    }
    overlay->set_capture_exclusion(capture_exclusion);
    if (edit_mode != nullptr) {
      overlay->set_edit_mode(true, edit_mode->direct_drag,
                             panel.id == edit_mode->selected_panel,
                             edit_mode->drag_callback);
    }
    next.push_back(std::move(overlay));
  }
  windows_ = std::move(next);
}

void OverlayCollection::update_panel(const layerc::core::Panel &panel) {
  const auto found = std::ranges::find_if(
      windows_, [&](const auto &candidate) {
        return candidate != nullptr && candidate->panel_id() == panel.id;
      });
  if (found != windows_.end()) (*found)->update_snapshot(panel, cooldowns_);
}

void OverlayCollection::apply_session_override(
    const layerc::core::Panel &panel, const std::optional<bool> visible,
    std::optional<std::wstring> text) {
  const auto found = std::ranges::find_if(
      windows_, [&](const auto &candidate) {
        return candidate != nullptr && candidate->panel_id() == panel.id;
      });
  if (found == windows_.end()) return;
  (*found)->update_snapshot(panel, cooldowns_);
  (*found)->set_session_override(visible, std::move(text));
}
void OverlayCollection::set_capture_exclusion(const bool enabled) {
  for (auto &overlay : windows_) overlay->set_capture_exclusion(enabled);
}

void OverlayCollection::hide() noexcept {
  for (auto &overlay : windows_) {
    if (overlay) overlay->hide();
  }
}

void OverlayCollection::release() noexcept {
  hide();
  windows_.clear();
  windows_.shrink_to_fit();
  cooldowns_.clear();
  cooldowns_.shrink_to_fit();
}

void OverlayCollection::render_for(
    const HWND target, const std::vector<layerc::core::CooldownState> &states,
    const layerc::core::TimePoint now, const bool always_show) {
  for (auto &overlay : windows_) {
    if (!always_show && !overlay->should_show(states)) {
      overlay->hide();
      continue;
    }
    overlay->show_for(target);
    overlay->render(states, now);
  }
}

void OverlayCollection::render_on_rect(
    const RECT &target, const std::vector<layerc::core::CooldownState> &states,
    const layerc::core::TimePoint now) {
  for (auto &overlay : windows_) {
    if (!overlay->should_show(states)) {
      overlay->hide();
      continue;
    }
    overlay->show_on_rect(target);
    overlay->render(states, now);
  }
}

} // namespace layerc::daemon
