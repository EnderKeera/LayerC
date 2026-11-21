#include "daemon_link.hpp"
#include "status_kind.hpp"

#include "document_commit.hpp"

#include <optional>
#include <string>

// The save report both platforms word identically: a disk commit and the
// runtime reload that followed it are distinct outcomes.
namespace layerc::editor {

// Follows save_report_text's branches: a write LayerD loaded, or loads when it
// starts, is a success; unconfirmed durability or reload and a paused autosave
// need a look; a declined manual upgrade was the user's choice; any other
// refusal wrote nothing.
StatusKind save_report_kind(const CommitOutcome &commit,
                            const std::optional<ReloadResult> &reload,
                            const SaveMode mode) {
  switch (commit.result) {
  case CommitResult::ok:
    break;
  case CommitResult::upgrade_declined:
    return mode == SaveMode::autosave ? StatusKind::warning : StatusKind::information;
  case CommitResult::write_failed:
    if (commit.write_error == layerc::platform::ProfileWriteError::durability_unconfirmed)
      return StatusKind::warning;
    return StatusKind::failure;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
  case CommitResult::blocked_by_sidecar:
    return StatusKind::failure;
  }
  return reload && reload->state == ReloadState::failed ? StatusKind::warning
                                                        : StatusKind::success;
}

std::string save_report_text(const CommitOutcome &commit,
                             const std::optional<ReloadResult> &reload,
                             const SaveMode mode) {
  const bool autosave = mode == SaveMode::autosave;
  const std::string not_saved = autosave ? "Not autosaved: " : "Not saved: ";
  switch (commit.result) {
  case CommitResult::ok:
    break;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    return not_saved + "finish the invalid field first.";
  case CommitResult::upgrade_declined:
    return autosave
               ? "Autosave paused \u2014 use Save to approve the profile format upgrade."
               : "Not saved: the profile format upgrade was declined.";
  case CommitResult::blocked_by_sidecar:
    return not_saved + "repair the invalid visual or automation settings first.";
  case CommitResult::write_failed:
    // The final durability barrier can fail after every file was published.
    if (commit.write_error == layerc::platform::ProfileWriteError::durability_unconfirmed)
      return (autosave ? "Autosave unconfirmed" : "Save unconfirmed") +
             std::string(": the files may already be on disk, but their durability "
                         "could not be confirmed. These edits are kept; Save again.");
    return (autosave ? "Not autosaved. " : "Not saved. ") +
           write_failure_text(commit.write_error) + ".";
  }
  if (!reload) return autosave ? "Autosaved." : "Saved.";
  switch (reload->state) {
  case ReloadState::acknowledged:
    return autosave ? "Autosaved \u2014 LayerD acknowledged the reload request."
                    : "Saved \u2014 LayerD acknowledged the reload request.";
  case ReloadState::signalled:
    return autosave ? "Autosaved \u2014 LayerD was asked to reload."
                    : "Saved \u2014 LayerD was asked to reload.";
  case ReloadState::daemon_unavailable:
    return autosave ? "Autosaved. LayerD is not running; the change applies when it starts."
                    : "Saved. LayerD is not running; the change applies when it starts.";
  case ReloadState::failed:
    break;
  }
  return (autosave ? "Autosaved; LayerD's reload could not be confirmed"
                   : "Saved; LayerD's reload could not be confirmed") +
         (reload->error.empty() ? std::string(".") : ": " + reload->error + ".");
}

} // namespace layerc::editor
