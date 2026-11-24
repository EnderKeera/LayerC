#include "editor_app.hpp"
#include "export_selection_window.hpp"
#include "appearance_window.hpp"
#include "editor_file_chooser.hpp"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <variant>

namespace layerc::editor {
void EditorApp::import_profiles() {
  const auto source = choose_editor_file({*this, EditorFileRequest::Mode::open,
      "Import JSON or LayerC profile pack", "Import", "LayerC profiles", {"json", "layercprofile"},
      EditorFileRequest::DirectoryMemory::profiles});
  if (!source) return;
  auto result = session.plan_import(*source);
  auto *plan = std::get_if<ImportPlan>(&result);
  if (!plan) { report(StatusKind::failure, std::get<std::string>(result)); return; }
  for (auto &row : plan->rows) {
    const auto message = row.name + "\n" + (row.collides ? "Existing profile collision." : "New profile.") +
                         "\nCopy preserves the original. Replace uses the reviewed destination.";
    fl_message_title("Import profile");
    const auto include = fl_choice("%s", "Cancel import", "Skip", "Include", message.c_str());
    if (include == 0) { report(StatusKind::information, "Import canceled. No files changed."); return; }
    row.action = layerc::bundle::ImportAction::skip;
    if (include == 2) {
      row.action = layerc::bundle::ImportAction::copy;
      if (row.collides) {
        fl_message_title("Import profile");
        const auto action = fl_choice("%s", "Cancel import", "Copy", "Replace", message.c_str());
        if (action == 0) { report(StatusKind::information, "Import canceled. No files changed."); return; }
        if (action == 2) row.action = layerc::bundle::ImportAction::replace;
      }
    }
  }
  result = session.review_import(std::move(*plan));
  plan = std::get_if<ImportPlan>(&result);
  if (!plan) { report(StatusKind::failure, std::get<std::string>(result)); return; }
  std::string summary = "Reviewed destinations:\n";
  for (const auto &row : plan->rows) {
    summary += row.filename + " — " +
        (row.action == layerc::bundle::ImportAction::copy ? "copy" :
         row.action == layerc::bundle::ImportAction::replace ? "replace" : "skip") + "\n";
  }
  fl_message_title("Import profile");
  if (fl_choice("%s", "Cancel", "Import", nullptr, summary.c_str()) != 1) {
    report(StatusKind::information, "Import canceled. No files changed."); return;
  }
  // The reviewed transaction itself rejects replacement of an unsaved open
  // document; do not discard it implicitly or commit a silently rebound plan.
  const auto outcome = session.import_pack(*plan);
  if (outcome.ok) {
    if (!session.has_document() && !session.profiles().profiles.empty()) {
      if (const auto error = session.open(0)) { report(StatusKind::warning, *error); refresh(); return; }
    }
    refresh();
  }
  report(outcome.kind, outcome.text); query_status();
}
void EditorApp::export_profile(const bool pack) {
  if (!may_leave()) return;
  const auto selected = session.current_row();
  if (!selected) { report(StatusKind::failure, "Select a saved profile before exporting."); return; }
  std::vector<std::size_t> rows{*selected};
  if (pack) {
    ExportSelectionDialog dialog(session.profiles().profiles, *selected);
    apply_editor_appearance(appearance_, dialog, nullptr, nullptr);
    dialog.show();
    while (dialog.shown()) Fl::wait();
    if (!dialog.selection) { report(StatusKind::information, "Export canceled. No files changed."); return; }
    rows = std::move(*dialog.selection);
  }
  const auto destination = pack
      ? choose_editor_file({*this, EditorFileRequest::Mode::save, "Export complete profile pack",
          "Export", "LayerC profile pack", {"layercprofile"}, EditorFileRequest::DirectoryMemory::profiles})
      : choose_editor_file({*this, EditorFileRequest::Mode::save, "Export base JSON only",
          "Export", "JSON", {"json"}, EditorFileRequest::DirectoryMemory::profiles});
  if (!destination) return;
  const auto outcome = pack ? session.export_pack(*destination, rows)
                            : session.export_json(*destination, *selected);
  report(outcome.kind, outcome.text);
}
} // namespace layerc::editor
