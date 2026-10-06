// X11 overlay capability (V2.6.1): one click-through window per panel,
// painted from the input sessions' cooldown states and registered with the
// daemon kernel as Capability::overlay.
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

const layerc::platform::X11WindowSnapshot *
find_snapshot(const layerc::platform::X11WindowScan &scan,
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
};

// Resolves the panel and its overrides into `frame.panel` and `result`.
// The panel is re-copied only while an override targets it (or to undo one),
// so the common no-override frame does no allocation. false when the panel
// is gone; visibility itself is decided by core::present_panel.
[[nodiscard]] bool apply_automation_overrides(
    layerc::platform::X11OverlayFrame &frame, bool &overridden,
    const InputSession &session, const std::string_view panel_id,
    PanelOverrides &result) {
  const auto base = std::ranges::find(session.automation.profile().panels, panel_id,
                                      &layerc::core::Panel::id);
  if (base == session.automation.profile().panels.end()) return false;
  const auto overrides =
      session.automation.runtime()
          ? std::span(session.automation.runtime()->snapshot().overlay_overrides)
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
  result = {resolved.visible, std::move(resolved.text)};
  return true;
}

// Fills `frame.bars` from the shared panel presentation. false means the
// panel is hidden right now.
[[nodiscard]] bool
fill_bars(layerc::platform::X11OverlayFrame &frame, const InputSession &session,
          const TimePoint now, const PanelOverrides &overrides) {
  const auto presentation = layerc::core::present_panel(
      frame.panel, session.automation.profile().cooldowns, session.automation.snapshot_buffer(),
      {.visible = overrides.visible, .text = overrides.text, .edit_mode = false},
      now);
  frame.bars.clear();
  for (const auto &item : presentation.bars) {
    layerc::platform::X11OverlayBar bar;
    bar.label = item.label;
    bar.running = !item.ready;
    bar.remaining = item.remaining;
    frame.bars.push_back(std::move(bar));
  }
  return presentation.visible;
}

} // namespace

void X11OverlayService::attach(layerc::platform::X11Display &display) noexcept {
  display_ = &display;
}

void X11OverlayService::refresh(
    const std::span<InputSession> sessions,
    const layerc::platform::X11WindowScan &scan,
    const layerc::platform::X11MonitorScan &monitors) {
  std::vector<Target> next;
  for (const auto &session : sessions) {
    if (!session.active) continue;
    const auto *snapshot = find_snapshot(scan, session.window);
    if (snapshot == nullptr || snapshot->hidden || snapshot->width <= 0 ||
        snapshot->height <= 0)
      continue;
    const layerc::platform::X11OverlayRect target{
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
      entry.active = snapshot->active;
      // Keep the window across rebuilds so panels are not recreated (and
      // re-raised) every time a profile or window list changes.
      const auto previous = std::ranges::find_if(
          targets_, [&](const Target &candidate) {
            return candidate.profile_id == entry.profile_id &&
                   candidate.panel_id == entry.panel_id;
          });
      if (previous != targets_.end()) entry.window = std::move(previous->window);
      next.push_back(std::move(entry));
    }
  }
  targets_ = std::move(next);
}

void X11OverlayService::render(const std::span<InputSession> sessions,
                               const TimePoint now) {
  if (display_ == nullptr) return;
  // refresh() emits each session's panels together, so one snapshot per run
  // of targets covers every panel of that session.
  const InputSession *snapshotted = nullptr;
  for (auto &target : targets_) {
    if (!target.window.valid()) continue;
    auto *session = find_session(sessions, target.profile_id);
    if (session == nullptr) continue;
    if (session != snapshotted) {
      (void)session->automation.snapshot(now);
      snapshotted = session;
    }
    PanelOverrides overrides;
    if (!apply_automation_overrides(target.frame, target.overridden, *session,
                                    target.panel_id, overrides) ||
        !fill_bars(target.frame, *session, now, overrides)) {
      target.window.hide();
      continue;
    }
    // Raise before painting: overlays have no backing store, so drawing
    // into a window another overlay still covers is lost when it is raised
    // afterwards, and overlapping panels would show unpainted black.
    if (target.active) target.window.raise();
    if (!target.window.update(target.frame)) {
      // The server rejected the window; drop it so the next refresh rebuilds.
      target.window = layerc::platform::X11OverlayWindow{};
      continue;
    }
  }
}

std::optional<TimePoint>
X11OverlayService::next_render_deadline(const TimePoint now) const {
  for (const auto &target : targets_)
    if (target.window.visible()) return now + overlay_repaint_interval;
  return std::nullopt;
}

std::size_t X11OverlayService::window_count() const noexcept {
  std::size_t count = 0;
  for (const auto &target : targets_)
    if (target.window.valid()) ++count;
  return count;
}

std::size_t
X11OverlayService::window_count(const std::string_view profile_id,
                                const std::uint64_t session_window) const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(targets_, [&](const Target &target) {
        return target.profile_id == profile_id &&
               target.session_window == session_window &&
               target.window.valid();
      }));
}

std::vector<std::uint64_t> X11OverlayService::windows() const {
  std::vector<std::uint64_t> result;
  for (const auto &target : targets_)
    if (target.window.valid()) result.push_back(target.window.window());
  return result;
}

std::vector<std::string> X11OverlayService::unavailable() const {
  if (display_ == nullptr || !display_->valid())
    return {"overlay: no X display for the overlay service"};
  auto reasons = layerc::platform::overlay_unavailable(display_->probe());
  if (!reasons.empty()) return reasons;
  if (!fault_.empty()) return {fault_};
  // No targets with no fault is a healthy idle service, not unavailability:
  // the capability layer already reports undemanded overlays as inactive via
  // the demand gate, and a real per-session failure lands in fault_.
  return {};
}

layerc::runtime::ServiceReconcileResult
X11OverlayService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::overlay))
    return {false, "overlay: service invoked without demand"};
  if (display_ == nullptr || !display_->valid()) {
    // Never fault: an overlay failure must not unwind the input service the
    // kernel activated in the same pass. Publish the reason instead.
    fault_ = "overlay: no X display for the overlay service";
    return {true, fault_};
  }
  if (const auto reasons =
          layerc::platform::overlay_unavailable(display_->probe());
      !reasons.empty()) {
    fault_ = reasons.front();
    return {true, fault_};
  }
  std::string error;
  for (auto &target : targets_) {
    if (target.window.valid()) continue;
    auto window =
        layerc::platform::X11OverlayWindow::create(*display_, error);
    if (!window.has_value()) {
      fault_ = error;
      return {};
    }
    target.window = std::move(*window);
  }
  fault_.clear();
  return {};
}

void X11OverlayService::quiesce() noexcept {
  targets_.clear();
  fault_.clear();
}

layerc::runtime::RuntimeResourceStats
X11OverlayService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  stats.windows = static_cast<std::uint32_t>(
      std::min<std::size_t>(window_count(), 0xFFFFFFFFU));
  stats.objects = static_cast<std::uint32_t>(
      std::min<std::size_t>(targets_.size(), 0xFFFFFFFFU));
  return stats;
}

} // namespace layerc::daemon
