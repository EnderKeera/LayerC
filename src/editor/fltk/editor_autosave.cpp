#include "editor_app.hpp"
#include "platform/product_text.hpp"
#include "layerc/core/profile.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Multiline_Output.H>
#include <utility>

namespace layerc::editor {
namespace core = layerc::core;

using namespace std::chrono_literals;

EditorApp::EditorApp()
    : EditorApp(&SteadyTimeout::Clock::now, {steady_wake_add, steady_wake_remove}) {}

EditorApp::EditorApp(const SteadyTimeout::Now now, const SteadyTimeout::Scheduler scheduler)
    : timer_now_(now), timer_scheduler_(scheduler),
      autosave_timer_([this] { autosave_tick(); }, now, scheduler) {
  window.copy_label(editor_window_title().c_str());
  // An accepted edit is the only autosave signal; presentation never is.
  session.set_change_listener([this] { sync_edit_services(); });
}

EditorApp::~EditorApp() {
  set_steady_timer_fault_handler(nullptr, this);
  Fl::remove_timeout(announce_steady_timer_fault, this);
  session.set_change_listener({});
  stop_edit_services();
  stop_opener();
  live_draft_.reset(); // the only join: bounded, and only at shutdown
  shutdown_daemon_link(); // fails pending requests, joins the link's worker
}

std::string EditorApp::edit_status() const {
  if (!session.has_unsaved_changes()) return "No pending changes.";
  return session.is_draft() ? "Draft changed — use Save once to create the profile."
                            : "Editing — autosave pending.";
}
void EditorApp::update_daemon_status() {
  const auto text = daemon_text_ +
      (live_draft_text_.empty() ? "" : "\nLive draft: " + live_draft_text_);
  status_pane_->set_daemon(most_severe(daemon_kind_, live_draft_kind_), text);
}
void EditorApp::end_live_draft() {
  // Queued, never joined here: the worker ends any lease off the UI thread.
  // Unconditional: a stale failure must not hide a newer live lease.
  if (live_draft_) live_draft_->end();
  live_draft_sequence_ = 0;
}
void EditorApp::stop_edit_services() {
  autosave_timer_.cancel();
  end_live_draft();
  live_draft_text_.clear();
  live_draft_kind_ = StatusKind::none;
}
void EditorApp::schedule_autosave(const bool text_change) {
  if (saving_ || !session.has_document() || session.is_draft()) return;
  autosave_timer_.start(text_change ? 400ms : 1ms);
}
void EditorApp::sync_edit_services() {
  if (saving_ || observed_generation_ == session.edit_generation()) return;
  observed_generation_ = session.edit_generation();
  // Undo back to the saved profile is a change LayerD must mirror too.
  publish_on_window();
  if (!session.has_document() || session.is_draft() || !session.has_unsaved_changes()) {
    stop_edit_services();
    update_daemon_status();
    return;
  }
  schedule_autosave(session.last_edit_was_text());
  if (!standalone_live_drafts()) {
    end_live_draft();
    live_draft_text_.clear();
    live_draft_kind_ = StatusKind::none;
    update_daemon_status();
    return;
  }
  const auto &document = session.document();
  const bool allowed = (!document.legacy_base_schema() && !document.legacy_visual_schema()) ||
                       document.upgrade_acknowledged();
  if (!allowed) {
    end_live_draft();
    live_draft_kind_ = StatusKind::warning;
    live_draft_text_ = "paused — use Save to approve the profile format upgrade.";
    update_daemon_status();
    return;
  }
  if (!live_draft_) {
    auto created = make_standalone_live_drafts([this] { live_draft_ready(); });
    if (const auto *unavailable = std::get_if<StandaloneLiveDraftsUnavailable>(&created)) {
      live_draft_kind_ = StatusKind::warning;
      live_draft_text_ = "unavailable — " + unavailable->reason;
      update_daemon_status();
      return;
    }
    live_draft_ = std::get<std::unique_ptr<StandaloneLiveDrafts>>(std::move(created));
  }
  // The worker ends another profile's lease before publishing this one.
  live_draft_sequence_ =
      live_draft_->publish(document.profile().id, core::serialize_profile_json(document.profile()));
  live_draft_kind_ = StatusKind::information;
  live_draft_text_ = "publishing accepted base-profile edits; modules apply after autosave.";
  update_daemon_status();
}
void EditorApp::live_draft_ready() {
  auto &app = *this;
  if (!app.live_draft_) return;
  // Always drain first: a readable descriptor would otherwise spin the loop.
  auto update = app.live_draft_->take_update();
  if (!app.window.shown()) { app.stop_edit_services(); return; }
  // Only the latest publish describes the current draft; older or ended
  // results (including stale failures) are ignored.
  if (update && update->sequence != 0 && update->sequence == app.live_draft_sequence_) {
    app.live_draft_kind_ = update->reply ? StatusKind::success : StatusKind::warning;
    app.live_draft_text_ = update->reply
        ? "base-profile edits applied in memory, revision " +
              std::to_string(update->reply->revision) + "; modules apply after autosave."
        : "unavailable — " + update->error + ". Autosave still writes locally.";
    app.update_daemon_status();
    // The worker idles after a fault; the next accepted edit publishes anew.
  }
}
void EditorApp::autosave_tick() {
  if (!window.shown()) { stop_edit_services(); return; }
  // Never mutate the document beneath a modal confirmation or calibration.
  if (Fl::modal() || saving_) {
    autosave_timer_.start(100ms);
    return;
  }
  (void)flush_autosave();
}
bool EditorApp::flush_autosave() {
  autosave_timer_.cancel();
  if (saving_) return false;
  if (!session.has_document()) return true;
  if (session.is_draft()) return false;
  saving_ = true;
  if (const auto error = apply_field_buffers()) {
    saving_ = false;
    observed_generation_ = session.edit_generation();
    // The same failure repeats on every autosave while its cause stays.
    report(StatusKind::failure, "Not autosaved. " + *error, StatusVoice::changed);
    return false;
  }
  session.end_text_edit();
  if (!session.has_unsaved_changes()) {
    saving_ = false;
    stop_edit_services();
    update_daemon_status();
    return true;
  }
  const auto outcome = session.save({}, SaveMode::autosave);
  saving_ = false;
  observed_generation_ = session.edit_generation();
  if (outcome.saved()) {
    stop_edit_services();
    // Keep the focused field and text buffer intact; only lists and preview
    // need refreshing. Full refresh would disturb text entry every 400 ms.
    fill_lists();
    preview->redraw();
  }
  // A saved autosave follows every edit and stays silent; a refusal is a
  // failure that repeats until the cause goes.
  report(outcome.kind, outcome.text,
         outcome.saved() ? StatusVoice::silent : StatusVoice::changed);
  query_status();
  return outcome.saved();
}
} // namespace layerc::editor
