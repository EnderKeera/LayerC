#include "editor_app.hpp"
#include "layerc/core/profile.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Multiline_Output.H>
#include <utility>

namespace layerc::editor {
namespace core = layerc::core;

EditorApp::~EditorApp() {
  stop_edit_services();
  stop_opener();
  if (live_draft_) Fl::remove_fd(live_draft_->notify_fd());
  live_draft_.reset(); // the only join: bounded, and only at shutdown
}

std::string EditorApp::edit_status() const {
  if (!session.has_unsaved_changes()) return "No pending changes.";
  return session.is_draft() ? "Draft changed — use Save once to create the profile."
                            : "Editing — autosave pending.";
}
void EditorApp::update_daemon_status() {
  if (!daemon_status) return;
  const auto text = daemon_text_ +
      (live_draft_text_.empty() ? "" : "\nLive draft: " + live_draft_text_);
  daemon_status->value(text.c_str());
  daemon_status->redraw();
}
void EditorApp::end_live_draft() {
  // Queued, never joined here: the worker ends any lease off the UI thread.
  // Unconditional: a stale failure must not hide a newer live lease.
  if (live_draft_) live_draft_->end();
  live_draft_sequence_ = 0;
}
void EditorApp::stop_edit_services() {
  Fl::remove_timeout(autosave_tick, this);
  end_live_draft();
  live_draft_text_.clear();
}
void EditorApp::schedule_autosave(const bool text_change) {
  if (saving_ || !session.has_document() || session.is_draft()) return;
  Fl::remove_timeout(autosave_tick, this);
  Fl::add_timeout(text_change ? 0.4 : 0.001, autosave_tick, this);
}
void EditorApp::sync_edit_services() {
  if (saving_ || observed_generation_ == session.edit_generation()) return;
  observed_generation_ = session.edit_generation();
  if (!session.has_document() || session.is_draft() || !session.has_unsaved_changes()) {
    stop_edit_services();
    update_daemon_status();
    return;
  }
  schedule_autosave(session.last_edit_was_text());
  const auto &document = session.document();
  const bool allowed = (!document.legacy_base_schema() && !document.legacy_visual_schema()) ||
                       document.upgrade_acknowledged();
  if (!allowed) {
    end_live_draft();
    live_draft_text_ = "paused — use Save to approve the profile format upgrade.";
    update_daemon_status();
    return;
  }
  if (!live_draft_) {
    live_draft_ = std::make_unique<LiveDraftClient>();
    Fl::add_fd(live_draft_->notify_fd(), FL_READ, live_draft_ready, this);
  }
  // The worker ends another profile's lease before publishing this one.
  live_draft_sequence_ =
      live_draft_->publish(document.profile().id, core::serialize_profile_json(document.profile()));
  live_draft_text_ = "publishing accepted base-profile edits; modules apply after autosave.";
  update_daemon_status();
}
void EditorApp::live_draft_ready(int, void *data) {
  auto &app = *static_cast<EditorApp *>(data);
  if (!app.live_draft_) return;
  // Always drain first: a readable descriptor would otherwise spin the loop.
  auto update = app.live_draft_->take_update();
  if (!app.window.shown()) { app.stop_edit_services(); return; }
  // Only the latest publish describes the current draft; older or ended
  // results (including stale failures) are ignored.
  if (update && update->sequence != 0 && update->sequence == app.live_draft_sequence_) {
    app.live_draft_text_ = update->reply
        ? "base-profile edits applied in memory, revision " +
              std::to_string(update->reply->revision) + "; modules apply after autosave."
        : "unavailable — " + update->error + ". Autosave still writes locally.";
    app.update_daemon_status();
    // The worker idles after a fault; the next accepted edit publishes anew.
  }
}
void EditorApp::autosave_tick(void *data) {
  auto &app = *static_cast<EditorApp *>(data);
  if (!app.window.shown()) { app.stop_edit_services(); return; }
  // Never mutate the document beneath a modal confirmation or calibration.
  if (Fl::modal() || app.saving_) {
    Fl::repeat_timeout(0.1, autosave_tick, data);
    return;
  }
  (void)app.flush_autosave();
}
bool EditorApp::flush_autosave() {
  Fl::remove_timeout(autosave_tick, this);
  if (saving_) return false;
  if (!session.has_document()) return true;
  if (session.is_draft()) return false;
  saving_ = true;
  if (!apply_fields()) {
    saving_ = false;
    observed_generation_ = session.edit_generation();
    report("Not autosaved. " + status_text_);
    return false;
  }
  session.end_text_edit();
  if (!session.has_unsaved_changes()) {
    saving_ = false;
    stop_edit_services();
    update_daemon_status();
    return true;
  }
  const auto outcome = session.save({}, false);
  saving_ = false;
  observed_generation_ = session.edit_generation();
  if (outcome.saved()) {
    stop_edit_services();
    // Keep the focused field and text buffer intact; only lists and preview
    // need refreshing. Full refresh would disturb text entry every 400 ms.
    fill_lists();
    preview->redraw();
  }
  std::string text = outcome.text;
  if (outcome.commit.result == CommitResult::upgrade_declined)
    text = "Autosave paused — use Save to approve the profile format upgrade.";
  else if (text.starts_with("Saved")) text.replace(0, 5, "Autosaved");
  else if (text.starts_with("Not saved")) text.replace(0, 9, "Not autosaved");
  else if (text.starts_with("Save unconfirmed")) text.replace(0, 16, "Autosave unconfirmed");
  report(std::move(text));
  query_status();
  return outcome.saved();
}
} // namespace layerc::editor
