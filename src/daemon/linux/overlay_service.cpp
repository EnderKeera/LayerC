#include "overlay_service.hpp"

#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::core::TimePoint;

const layerc::platform::WindowSnapshot *
find_snapshot(const layerc::platform::WindowScan &scan,
              const std::uint64_t window) noexcept {
  for (const auto &snapshot : scan.windows)
    if (snapshot.window == window) return &snapshot;
  return nullptr;
}

InputSession *find_session(const std::span<InputSession> sessions,
                           const std::string_view profile_id) noexcept {
  for (auto &session : sessions)
    if (session.automation.profile().id == profile_id) return &session;
  return nullptr;
}

struct PanelOverrides {
  std::optional<bool> visible;
  std::optional<std::string> text;
  std::vector<layerc::core::ItemPresentationOverride> items;
};

// Resolves the panel and its overrides into `frame.panel` and `result`.
// The panel is re-copied only while an override targets it (or to undo one),
// so the common no-override frame does no allocation. false when the panel
// is gone; visibility itself is decided by core::present_panel.
[[nodiscard]] bool apply_automation_overrides(
    layerc::platform::OverlayFrame &frame, bool &overridden,
    const InputSession &session, const std::string_view panel_id,
    const layerc::core::AutomationRuntimeSnapshot *snapshot,
    PanelOverrides &result) {
  const auto base = std::ranges::find(session.automation.profile().panels, panel_id,
                                      &layerc::core::Panel::id);
  if (base == session.automation.profile().panels.end()) return false;
  const auto overrides = snapshot != nullptr
      ? std::span<const layerc::core::OverlayPropertyOverride>(snapshot->overlay_overrides)
      : std::span<const layerc::core::OverlayPropertyOverride>{};
  if (std::ranges::none_of(overrides, [&](const auto &item) {
        return item.panel_id == panel_id;
      })) {
    if (overridden) frame.panel = *base;
    overridden = false;
    result = {};
    return true;
  }
  auto resolved = layerc::core::resolve_panel_overrides(*base, overrides);
  frame.panel = std::move(resolved.panel);
  overridden = true;
  result = {resolved.visible, std::move(resolved.text), std::move(resolved.items)};
  return true;
}

[[nodiscard]] core::PanelPresentation fill_bars(
    platform::OverlayFrame &frame, const InputSession &session,
    const core::AutomationRuntimeSnapshot *snapshot, const core::TimePoint now,
    const PanelOverrides &overrides) {
  auto shown = core::present_panel(frame.panel,
      session.automation.profile().cooldowns, session.automation.snapshot_buffer(),
      snapshot, {.visible = overrides.visible, .text = overrides.text,
                 .edit_mode = false, .presentation_epoch = session.presentation_epoch,
                 .items = overrides.items}, now);
  frame.bars.clear();
  frame.bars.reserve(shown.items.size());
  for (const auto &item : shown.items)
    frame.bars.push_back(platform::overlay_bar_from_presentation(item));
  return shown;
}

} // namespace

void OverlayService::use_backend(OverlayBackend *backend) noexcept {
  if (backend_ == backend) return;
  if (backend_) backend_->retain({});
  for (auto &target : targets_) {
    target.shown = false;
    target.retry_at.reset();
    target.next_presentation.reset();
  }
  backend_ = backend;
}

std::string OverlayService::key(const Target &target) {
  // Length-prefixed so no pair of ids can collide.
  return std::to_string(target.profile_id.size()) + ':' + target.profile_id + target.panel_id;
}

void OverlayService::refresh(
    const std::span<InputSession> sessions,
    const layerc::platform::WindowScan &scan,
    const layerc::platform::MonitorScan &monitors) {
  std::vector<Target> next;
  for (const auto &session : sessions) {
    if (!session.active) continue;
    const auto *snapshot = find_snapshot(scan, session.window);
    if (snapshot == nullptr || snapshot->hidden || snapshot->width <= 0 ||
        snapshot->height <= 0)
      continue;
    const layerc::platform::OverlayRect target{
        snapshot->x, snapshot->y, snapshot->width, snapshot->height};
    const int dpi = layerc::platform::monitor_dpi_for_rect(
        monitors, target.x, target.y, target.width, target.height);
    for (const auto &panel : session.automation.profile().panels) {
      Target entry;
      entry.profile_id = session.automation.profile().id;
      entry.panel_id = panel.id;
      entry.session_window = session.window;
      entry.frame.target = target;
      entry.frame.panel = panel;
      entry.frame.dpi = dpi;
      entry.frame.anchor_active = snapshot->active;
      // Keep the window across rebuilds so panels are not recreated (and
      // re-raised) every time a profile or window list changes.
      const auto previous = std::ranges::find_if(
          targets_, [&](const Target &candidate) {
            return candidate.profile_id == entry.profile_id &&
                   candidate.panel_id == entry.panel_id;
          });
      if (previous != targets_.end()) {
        entry.shown = previous->shown;
        if (previous->session_window == entry.session_window) {
          entry.retry_at = previous->retry_at;
          entry.next_presentation = previous->next_presentation;
        }
      }
      next.push_back(std::move(entry));
    }
  }
  targets_ = std::move(next);
  if (backend_) {
    std::vector<std::string> keys;
    keys.reserve(targets_.size());
    for (const auto &target : targets_) keys.push_back(key(target));
    backend_->retain(keys);
  }
}

void OverlayService::render(const std::span<InputSession> sessions,
                               const TimePoint now) {
  if (!reconciled_ || !backend_) return;
  // refresh() emits each session's panels together, so one snapshot per run
  // of targets covers every panel of that session.
  const InputSession *snapshotted = nullptr;
  core::AutomationRuntimeSnapshot runtime_snapshot;
  const core::AutomationRuntimeSnapshot *snapshot = nullptr;
  for (auto &target : targets_) {
    auto *session = find_session(sessions, target.profile_id);
    if (session == nullptr) continue;
    if (session != snapshotted) {
      (void)session->automation.snapshot(now);
      if (const auto *live = session->automation.presentation_snapshot()) {
        runtime_snapshot = *live;
        snapshot = &runtime_snapshot;
      } else if (session->presentation_defaults) {
        runtime_snapshot = *session->presentation_defaults;
        snapshot = &runtime_snapshot;
      } else {
        snapshot = nullptr;
      }
      snapshotted = session;
    }
    PanelOverrides overrides;
    if (!apply_automation_overrides(target.frame, target.overridden, *session,
                                    target.panel_id, snapshot, overrides)) {
      backend_->hide(key(target));
      target.shown = false;
      target.retry_at.reset();
      target.next_presentation.reset();
      continue;
    }
    auto shown = fill_bars(target.frame, *session, snapshot, now, overrides);
    if (!shown.visible) {
      backend_->hide(key(target));
      target.shown = false;
      target.retry_at.reset();
      target.next_presentation.reset();
      continue;
    }
    target.next_presentation = shown.next_wakeup;
    if (std::ranges::any_of(shown.items, [](const auto &item) {
          return (item.kind == core::OverlayItemKind::cooldown && !item.ready) ||
                 (item.kind == core::OverlayItemKind::timer && item.active);
        }))
      target.next_presentation = core::earliest_deadline(
          target.next_presentation, now + overlay_repaint_interval);
    if (target.retry_at && now < *target.retry_at) continue;
    target.shown = backend_->show(key(target), target.frame);
    target.retry_at = target.shown
                          ? std::nullopt
                          : std::optional{now + overlay_repaint_interval};
  }
}

std::optional<TimePoint>
OverlayService::next_render_deadline(const TimePoint) const {
  if (!reconciled_ || !backend_) return std::nullopt;
  std::optional<TimePoint> next;
  for (const auto &target : targets_) {
    next = core::earliest_deadline(next, target.retry_at);
    if (target.shown)
      next = core::earliest_deadline(next, target.next_presentation);
  }
  return next;
}

std::size_t OverlayService::window_count() const noexcept {
  std::size_t count = 0;
  for (const auto &target : targets_)
    if (target.shown) ++count;
  return count;
}

std::size_t
OverlayService::window_count(const std::string_view profile_id,
                                const std::uint64_t session_window) const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(targets_, [&](const Target &target) {
        return target.profile_id == profile_id &&
               target.session_window == session_window &&
               target.shown;
      }));
}

std::vector<std::string> OverlayService::unavailable() const {
  auto reasons = backend_
                     ? backend_->unavailable()
                     : std::vector<std::string>{
                           "overlay: no X display for the overlay service"};
  for (const auto &target : targets_)
    if (target.retry_at)
      reasons.push_back("overlay: could not show panel " + target.panel_id +
                        " for profile " + target.profile_id);
  return reasons;
}

layerc::runtime::ServiceReconcileResult
OverlayService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::overlay))
    return layerc::runtime::ServiceReconcileResult::failed(
        "overlay: service invoked without demand");
  reconciled_ = true;
  if (!backend_) {
    const std::string reason = "overlay: no X display for the overlay service";
    return layerc::runtime::ServiceReconcileResult::degraded(reason);
  }
  const auto reasons = backend_->unavailable();
  if (!reasons.empty()) {
    const std::string reason = reasons.front().starts_with("overlay: ")
                                   ? reasons.front()
                                   : "overlay: " + reasons.front();
    return layerc::runtime::ServiceReconcileResult::degraded(reason);
  }
  return {};
}

void OverlayService::quiesce() noexcept {
  reconciled_ = false;
  if (backend_) backend_->retain({});
  for (auto &target : targets_) {
    target.retry_at.reset();
    target.next_presentation.reset();
    target.shown = false;
  }
  targets_.clear();
}

layerc::runtime::RuntimeResourceStats
OverlayService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  if (!reconciled_) return stats;
  stats.windows = static_cast<std::uint32_t>(
      std::min<std::size_t>(window_count(), 0xFFFFFFFFU));
  stats.objects = static_cast<std::uint32_t>(
      std::min<std::size_t>(targets_.size(), 0xFFFFFFFFU));
  return stats;
}

} // namespace layerc::daemon
