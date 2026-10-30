#include "overlay_collection.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace {

auto find_panel_window(std::vector<std::unique_ptr<OverlayWindow>> &windows,
                       const std::string_view panel_id) {
  return std::ranges::find_if(windows, [&](const auto &candidate) {
    return candidate != nullptr && candidate->panel_id() == panel_id;
  });
}

} // namespace

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
    const auto found = find_panel_window(windows_, panel.id);
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
  const auto found = find_panel_window(windows_, panel.id);
  if (found != windows_.end()) (*found)->update_snapshot(panel, cooldowns_);
}

void OverlayCollection::apply_overrides(
    const layerc::core::GameProfile &profile,
    const std::span<const layerc::core::OverlayPropertyOverride> overrides) {
  for (const auto &base_panel : profile.panels) {
    const auto found = find_panel_window(windows_, base_panel.id);
    if (found == windows_.end()) continue;
    const auto resolved =
        layerc::core::resolve_panel_overrides(base_panel, overrides);
    (*found)->update_snapshot(resolved.panel, cooldowns_);
    (*found)->set_session_override(resolved.visible, resolved.text, resolved.items);
  }
}

const OverlayWindow *
OverlayCollection::find(const std::string_view panel_id) const {
  const auto found = std::ranges::find_if(windows_, [&](const auto &candidate) {
    return candidate != nullptr && candidate->panel_id() == panel_id;
  });
  return found == windows_.end() ? nullptr : found->get();
}
void OverlayCollection::set_capture_exclusion(const bool enabled) {
  for (auto &overlay : windows_) overlay->set_capture_exclusion(enabled);
}

void OverlayCollection::hide() noexcept {
  frames_.clear();
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
  frames_.shrink_to_fit();
}

void OverlayCollection::render_for(
    const HWND target, const std::vector<layerc::core::CooldownState> &states,
    const layerc::core::TimePoint now,
    const layerc::core::AutomationRuntimeSnapshot *automation,
    const layerc::core::TimePoint epoch) {
  frames_.clear();
  frames_.reserve(windows_.size());
  for (auto &overlay : windows_) {
    auto &presentation = frames_.emplace_back(overlay->present(states, now, automation, epoch));
    if (!presentation.visible) {
      overlay->hide();
      continue;
    }
    (void)overlay->sync_rows(presentation);
    overlay->show_for(target);
    overlay->render(presentation);
  }
}

void OverlayCollection::render_on_rect(
    const RECT &target, const std::vector<layerc::core::CooldownState> &states,
    const layerc::core::TimePoint now,
    const layerc::core::AutomationRuntimeSnapshot *automation,
    const layerc::core::TimePoint epoch) {
  frames_.clear();
  frames_.reserve(windows_.size());
  for (auto &overlay : windows_) {
    auto &presentation = frames_.emplace_back(overlay->present(states, now, automation, epoch));
    if (!presentation.visible) {
      overlay->hide();
      continue;
    }
    (void)overlay->sync_rows(presentation);
    overlay->show_on_rect(target);
    overlay->render(presentation);
  }
}

} // namespace layerc::daemon
