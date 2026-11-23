// The on-window edit session where LayerD draws the profile's bars over the
// game window (on_window_edit_sessions(); Windows). The preview drives it:
// every accepted edit is mirrored as the live profile and preview drags as
// anchors. With Direct Drag the user drags the bars in game instead; LayerD's
// final anchor commits once, locally, and is never echoed back.
#include "editor_app.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <FL/fl_ask.H>
#include <algorithm>
#include <utility>

namespace layerc::editor {
namespace {

using Event = OnWindowEditEvent;

// A one-way schema migration is not shown to LayerD before the user approves it.
bool live_publish_allowed(const ProfileDocument &document) {
  return (!document.legacy_base_schema() && !document.legacy_visual_schema()) ||
         document.upgrade_acknowledged();
}

StatusKind end_kind(const Event::EndReason reason) noexcept {
  switch (reason) {
  case Event::EndReason::daemon_error:
  case Event::EndReason::unavailable: return StatusKind::failure;
  case Event::EndReason::connection_lost:
  case Event::EndReason::direct_drag_unsent:
  case Event::EndReason::direct_drag_unconfirmed: return StatusKind::warning;
  case Event::EndReason::daemon_ended:
  case Event::EndReason::target_closed: return StatusKind::information;
  }
  return StatusKind::information;
}

std::string end_text(const Event &event) {
  switch (event.reason) {
  case Event::EndReason::daemon_error: return "LayerD ended editing: " + event.text;
  case Event::EndReason::daemon_ended:
    return event.text.empty() ? "LayerD ended the on-window edit session."
                              : "LayerD ended editing: " + event.text;
  case Event::EndReason::connection_lost:
    return "Live editor connection lost; normal overlays restored.";
  case Event::EndReason::target_closed: return "Edit target closed or minimized.";
  case Event::EndReason::unavailable: return "On-window editing is unavailable: " + event.text;
  case Event::EndReason::direct_drag_unsent:
    return "Direct drag could not reach LayerD, so on-window editing ended. LayerD makes "
           "the bars click-through again within two seconds.";
  case Event::EndReason::direct_drag_unconfirmed:
    return "LayerD did not confirm that Direct drag turned off, so on-window editing "
           "ended. LayerD makes the bars click-through again within two seconds.";
  }
  return "On-window editing ended.";
}

} // namespace

bool EditorApp::begin_on_window(const TaskCandidate &task) {
  const auto bundle = session.document().bundle();
  if (bundle.upgrade_needed) {
    const bool approved = bundle.items_upgrade_needed ? confirm_items_profile_upgrade()
                                                      : confirm_profile_upgrade();
    if (!approved) {
      report(StatusKind::failure,
             "On-window editing needs the profile format upgrade, which was not approved.");
      return false;
    }
    session.acknowledge_upgrade();
  }
  if (!on_window_) on_window_ = make_on_window_edit([this] { on_window_ready(); });
  if (!on_window_) {
    report(StatusKind::failure,
           "On-window editing is unavailable: the LayerD link could not start.");
    return false;
  }
  const auto &profile = session.document().profile();
  const auto selected = panel_index();
  on_window_->begin(task.window,
      {.profile_key = platform::canonical_profile_key(session.document().path()),
       .panel_id = selected ? profile.panels[*selected].id : std::string{},
       .profile_json = core::serialize_profile_json(profile)});
  forget_direct_drag();
  on_window_live_ = true;
  on_window_started_ = false;
  direct_drag_notice_shown_ = false;
  remote_anchor_.reset();
  if (panel_page_) panel_page_->show_direct_drag(false, false);
  return true;
}

void EditorApp::on_window_ready() {
  if (!on_window_) return;
  for (const auto &event : on_window_->take_events()) on_window_event(event);
}

void EditorApp::on_window_event(const OnWindowEditEvent &event) {
  if (!on_window_live_) return;
  switch (event.kind) {
  case Event::Kind::awaiting:
    report(StatusKind::information, "Waiting for LayerD's live-edit service...",
           StatusVoice::silent);
    return;
  case Event::Kind::started:
    on_window_started_ = true;
    if (panel_page_) panel_page_->show_direct_drag(true, false);
    report(StatusKind::information,
           "Starting mirrored on-window editing; game overlay stays click-through.");
    return;
  case Event::Kind::acknowledged:
    report(StatusKind::information, "Editing bars on the selected window.",
           StatusVoice::silent);
    return;
  case Event::Kind::anchor: {
    const auto &panels = session.document().profile().panels;
    if (std::none_of(panels.begin(), panels.end(),
                     [&](const auto &panel) { return panel.id == event.panel_id; }))
      return;
    if (panel_id_ != event.panel_id) {
      panel_id_ = event.panel_id;
      refresh();
    }
    if (!event.final) {
      remote_anchor_ = RemoteAnchor{event.panel_id, event.x, event.y};
      preview->redraw();
      return;
    }
    remote_anchor_.reset();
    session.end_text_edit();
    const auto error = session.edit([&](core::GameProfile &profile) {
      for (auto &panel : profile.panels)
        if (panel.id == event.panel_id) {
          panel.anchor_x = event.x;
          panel.anchor_y = event.y;
        }
    });
    session.end_text_edit();
    changed(error);
    return;
  }
  case Event::Kind::direct_drag_ended:
    // An off request already out is confirmed on its own; only Direct drag
    // still shown as on needs the end echoed, so a toggle crossing it in
    // flight cannot leave LayerD on.
    if (!on_window_started_ || !on_window_ || !panel_page_ ||
        !panel_page_->direct_drag_enabled())
      return;
    on_window_->direct_drag(false);
    direct_drag_disarming_ = true;
    direct_drag_lost_ = true;
    panel_page_->show_direct_drag(true, false);
    report(StatusKind::warning,
           "Direct drag ended: LayerD no longer has pointer capture. Waiting for LayerD to "
           "confirm click-through editing.",
           StatusVoice::silent);
    return;
  case Event::Kind::direct_drag_off:
    if (!on_window_started_ || !direct_drag_disarming_) return;
    direct_drag_disarming_ = false;
    if (std::exchange(direct_drag_lost_, false))
      report(StatusKind::warning, "Direct drag ended: LayerD no longer has pointer capture. "
                                  "Mirrored click-through editing restored.");
    else
      report(StatusKind::information, "Mirrored click-through editing restored.");
    return;
  case Event::Kind::protocol_error:
    report(StatusKind::failure, "Live channel error: " + event.text);
    return;
  case Event::Kind::ended:
    end_on_window(false);
    end_placement(end_kind(event.reason), end_text(event));
    return;
  case Event::Kind::expired:
    end_on_window(false);
    end_placement(StatusKind::failure,
                  "Live editing is unavailable: LayerD did not expose the live-edit "
                  "service. Exit the older LayerD from its tray icon, then try "
                  "Position on window again.");
    return;
  }
}

void EditorApp::end_on_window(const bool notify) {
  // Pick window ends with the edit it served, before its status is replaced.
  if (on_window_live_ && tasks_window) tasks_window->cancel_active_pick();
  if (on_window_live_ && on_window_) on_window_->end(notify);
  forget_direct_drag();
  on_window_live_ = false;
  on_window_started_ = false;
  remote_anchor_.reset();
  if (panel_page_) panel_page_->show_direct_drag(false, false);
}

void EditorApp::publish_on_window() {
  if (!on_window_live_ || !on_window_ || !placement_active()) return;
  const auto &document = session.document();
  if (!live_publish_allowed(document) || core::validate_profile(document.profile()))
    return;
  on_window_->live_profile(core::serialize_profile_json(document.profile()));
}

void EditorApp::send_on_window_anchor(const std::string_view panel_id, const bool final) {
  if (!on_window_started_ || !on_window_ || !placement_active()) return;
  for (const auto &panel : session.document().profile().panels)
    if (panel.id == panel_id) {
      on_window_->anchor(panel.id, panel.anchor_x, panel.anchor_y, final);
      return;
    }
}

void EditorApp::direct_drag_changed(const bool enabled) {
  if (!on_window_started_ || !on_window_) {
    if (panel_page_) panel_page_->show_direct_drag(false, false);
    return;
  }
  if (enabled && direct_drag_disarming_) {
    if (panel_page_) panel_page_->show_direct_drag(true, false);
    report(StatusKind::information,
           "Direct drag is still turning off. Turn it on again once LayerD confirms.");
    return;
  }
  if (enabled && !direct_drag_notice_shown_) {
    fl_message_title("Enable direct drag");
    const int choice = confirm_direct_drag_for_test_ ? confirm_direct_drag_for_test_() :
        fl_choice("Direct drag temporarily lets LayerD receive clicks only inside visible "
                  "bar rectangles. It uses no injection or game hooks, but anti-cheat "
                  "compatibility cannot be guaranteed. Continue?",
                  "Cancel", "Continue", nullptr);
    if (!on_window_started_ || !on_window_ || !placement_active()) {
      if (panel_page_) panel_page_->show_direct_drag(false, false);
      return;
    }
    if (choice != 1) {
      if (panel_page_) panel_page_->show_direct_drag(true, false);
      return;
    }
    direct_drag_notice_shown_ = true;
  }
  on_window_->direct_drag(enabled);
  if (panel_page_) panel_page_->show_direct_drag(true, enabled);
  if (enabled) {
    report(StatusKind::information, "Direct drag enabled for this edit session.");
    return;
  }
  // Restored only once LayerD confirms every bar is click-through again.
  direct_drag_disarming_ = true;
  direct_drag_lost_ = false;
  report(StatusKind::information, "Turning Direct drag off. Waiting for LayerD to confirm.",
         StatusVoice::silent);
}

void EditorApp::forget_direct_drag() {
  // Without LayerD's confirmation, its lease ends Direct drag within this bound.
  if (direct_drag_disarming_ || (panel_page_ && panel_page_->direct_drag_enabled()))
    direct_drag_release_by_ = std::chrono::steady_clock::now() + core::live_edit_lease_release;
  direct_drag_disarming_ = false;
  direct_drag_lost_ = false;
}

bool EditorApp::direct_drag_settled() const {
  return !direct_drag_disarming_ && !(panel_page_ && panel_page_->direct_drag_enabled()) &&
         std::chrono::steady_clock::now() >= direct_drag_release_by_;
}

bool EditorApp::cancel_direct_drag() {
  if (!on_window_started_ || !on_window_ || !panel_page_ ||
      !panel_page_->direct_drag_enabled())
    return false;
  direct_drag_changed(false);
  return true;
}

const EditorApp::RemoteAnchor *EditorApp::remote_anchor(const std::string_view panel_id) const {
  return remote_anchor_ && remote_anchor_->panel_id == panel_id ? &*remote_anchor_ : nullptr;
}

} // namespace layerc::editor
