#include "editor_app.hpp"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <cstdlib>
#include <numeric>
#include <variant>

namespace layerc::editor {
namespace {
std::optional<std::filesystem::path> choose_file(bool save, const char *title, const char *filter) {
  // Start where the user last chose a file, else in their home directory,
  // never in the process's incidental working directory.
  static std::string directory;
  if (const char *home = std::getenv("HOME"); directory.empty() && home && *home) directory = home;
  Fl_Native_File_Chooser chooser;
  chooser.title(title);
  if (!directory.empty()) chooser.directory(directory.c_str());
  chooser.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.filter(filter);
  if (save) chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (chooser.show() != 0 || !chooser.filename() || !*chooser.filename()) return std::nullopt;
  std::filesystem::path chosen(chooser.filename());
  directory = chosen.parent_path().string();
  return chosen;
}
}
void EditorApp::import_profiles() {
  const auto source = choose_file(false, "Import JSON or LayerC profile pack",
                                 "LayerC profiles\t*.{json,layercprofile}");
  if (!source) return;
  auto result = session.plan_import(*source);
  auto *plan = std::get_if<ImportPlan>(&result);
  if (!plan) { report(std::get<std::string>(result)); return; }
  for (auto &row : plan->rows) {
    const auto message = row.name + "\n" + (row.collides ? "Existing profile collision." : "New profile.") +
                         "\nCopy preserves the original. Replace uses the reviewed destination.";
    const auto include = fl_choice("%s", "Cancel import", "Skip", "Include", message.c_str());
    if (include == 0) { report("Import canceled. No files changed."); return; }
    row.action = layerc::bundle::ImportAction::skip;
    if (include == 2) {
      row.action = layerc::bundle::ImportAction::copy;
      if (row.collides) {
        const auto action = fl_choice("%s", "Cancel import", "Copy", "Replace", message.c_str());
        if (action == 0) { report("Import canceled. No files changed."); return; }
        if (action == 2) row.action = layerc::bundle::ImportAction::replace;
      }
    }
  }
  result = session.review_import(std::move(*plan));
  plan = std::get_if<ImportPlan>(&result);
  if (!plan) { report(std::get<std::string>(result)); return; }
  std::string summary = "Reviewed destinations:\n";
  for (const auto &row : plan->rows) {
    summary += row.filename + " — " +
        (row.action == layerc::bundle::ImportAction::copy ? "copy" :
         row.action == layerc::bundle::ImportAction::replace ? "replace" : "skip") + "\n";
  }
  if (fl_choice("%s", "Cancel", "Import", nullptr, summary.c_str()) != 1) {
    report("Import canceled. No files changed."); return;
  }
  // The reviewed transaction itself rejects replacement of an unsaved open
  // document; do not discard it implicitly or commit a silently rebound plan.
  const auto outcome = session.import_pack(*plan);
  if (outcome.ok) {
    if (!session.has_document() && !session.profiles().profiles.empty()) {
      if (const auto error = session.open(0)) { report(*error); refresh(); return; }
    }
    refresh();
  }
  report(outcome.text); query_status();
}
void EditorApp::export_profile(const bool pack) {
  if (!may_leave()) return;
  const auto selected = session.current_row();
  if (!selected) { report("Select a saved profile before exporting."); return; }
  std::vector<std::size_t> rows{*selected};
  if (pack) {
    const auto decision = fl_choice("Export the selected saved profile or every saved profile?",
                                    "Cancel", "Selected", "All");
    if (decision == 0) return;
    if (decision == 2) {
      rows.resize(session.profiles().profiles.size());
      std::iota(rows.begin(), rows.end(), std::size_t{0});
    }
  }
  const auto destination = choose_file(true, pack ? "Export complete profile pack" : "Export base JSON only",
                                       pack ? "LayerC profile pack\t*.layercprofile" : "JSON\t*.json");
  if (!destination) return;
  const auto outcome = pack ? session.export_pack(*destination, rows)
                            : session.export_json(*destination, *selected);
  report(outcome.text);
}
} // namespace layerc::editor
