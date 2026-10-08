// Toolkit-neutral model shared by the Linux editor toolkit prototypes
// (docs/LinuxEditorPlan.md, Change 1). Both prototypes drive the same
// layerc_editor_core document, store, and commit pipeline so their resource
// measurements differ only by toolkit.
#pragma once

#include "document_commit.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "proto_fixture.hpp"
#include "layerc/core/json.hpp"

#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::prototype {

struct Row {
  std::filesystem::path path;
  std::string label;
};

class Model {
public:
  // Called once, single-threaded, before toolkit initialization: it confines
  // the whole process to the pinned fixture before any profile is read.
  [[nodiscard]] bool scan() {
    const auto fixture = confined_fixture_store();
    if (const auto *error = std::get_if<FixtureError>(&fixture)) {
      std::fprintf(stderr, "Prototype refused: %s\n", error->reason.c_str());
      return false;
    }
    store_ = std::get<std::filesystem::path>(fixture);
    rows_.clear();
    const auto scan = layerc::platform::scan_profile_directory(store_);
    for (const auto &entry : scan.profiles)
      rows_.push_back({entry.path, layerc::editor::profile_list_text(entry.profile)});
    std::ranges::sort(rows_, {}, &Row::label);
    // The scan read by pathname: confirm the fixture still has no link escapes.
    const auto confirmed = checked_fixture_store();
    if (const auto *error = std::get_if<FixtureError>(&confirmed)) {
      std::fprintf(stderr, "Prototype refused: %s\n", error->reason.c_str());
      return false;
    }
    return true;
  }
  void announce_fixture() const {
    namespace json = layerc::core::json;
    std::string report = "fixture={\"store\":";
    json::append_quoted(report, store_.string());
    report += ",\"profiles\":"; json::append_unsigned(report, rows_.size());
    report += ",\"cooldowns\":"; json::append_unsigned(report, document_.profile().cooldowns.size());
    report += ",\"panels\":"; json::append_unsigned(report, document_.profile().panels.size());
    report += "}";
    std::printf("%s\n", report.c_str());
    std::fflush(stdout);
  }
  [[nodiscard]] const std::vector<Row> &rows() const noexcept { return rows_; }

  // Opens a row into the document; false keeps the previous document.
  bool open(const std::size_t row) {
    if (row >= rows_.size() || !permitted(rows_[row].path, false)) return false;
    // The opened inode must be a single-link regular file before and after
    // the pathname load, so a hard-link alias is rejected rather than read.
    const ValidatedDocument validated(rows_[row].path);
    if (!validated.valid()) return false;
    auto loaded = layerc::platform::load_profile_bundle(rows_[row].path);
    auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded);
    if (bundle == nullptr || !validated.identity_matches()) return false;
    document_.assign_bundle(rows_[row].path, std::move(*bundle));
    document_.set_dirty(false);
    document_.reset_history();
    selected_ = row;
    return true;
  }
  [[nodiscard]] layerc::editor::ProfileDocument &document() noexcept {
    return document_;
  }
  [[nodiscard]] std::size_t selected() const noexcept { return selected_; }

  // Saves through the shared pipeline; returns the user-visible outcome.
  std::string save() {
    if (!permitted(document_.path(), true)) return "Refused: document is outside the disposable fixture";
    if (const auto error = layerc::core::validate_profile(document_.profile()))
      return "Invalid: " + layerc::editor::profile_error_line(*error);
    const auto outcome = layerc::editor::commit_document(
        document_, document_.path(), {}, [] { return true; });
    if (!outcome.ok())
      return layerc::editor::write_failure_text(outcome.write_error);
    document_.mark_saved();
    return "Saved";
  }

private:
  // Reads need the store validated by scan(); writes re-validate the whole
  // fixture first, so a later link or marker change cannot redirect a commit.
  [[nodiscard]] bool permitted(const std::filesystem::path &path, const bool write) const {
    if (store_.empty()) return false;
    if (write) {
      const auto fixture = checked_fixture_store();
      const auto *store = std::get_if<std::filesystem::path>(&fixture);
      if (!store || *store != store_) return false;
    }
    std::error_code error;
    const auto parent = std::filesystem::canonical(path.parent_path(), error);
    return !error && parent == store_ && path.extension() == ".json" &&
           !std::filesystem::is_symlink(path, error) && !error;
  }
  std::filesystem::path store_;
  std::vector<Row> rows_;
  layerc::editor::ProfileDocument document_;
  std::size_t selected_{static_cast<std::size_t>(-1)};
};

[[nodiscard]] inline std::string chord_text(const layerc::core::InputChord &chord) {
  std::string text;
  for (const auto &token : chord.tokens) {
    if (!text.empty()) text += " + ";
    text += token.device == layerc::core::InputDevice::keyboard ? "Key " : "Button ";
    text += std::to_string(token.code);
  }
  return text.empty() ? "(unbound)" : text;
}

[[nodiscard]] inline std::string binding_text(const layerc::core::GameProfile &profile,
                                              const std::string &cooldown) {
  for (const auto &binding : profile.bindings)
    if (binding.cooldown_id == cooldown) return chord_text(binding.chord);
  return "(unbound)";
}

inline void announce(const char *text) {
  std::printf("%s\n", text);
  std::fflush(stdout);
}

// LAYERC_PROTO_CYCLES=N: after startup, 10 warm-up cycles (switch profile and
// open/close a modal dialog), print "warm", and wait for the resume file when
// LAYERC_PROTO_RESUME_FILE is set (otherwise pause 5 s). Only after the harness
// captures a baseline, run N cycles and print "cycles-done", then remain idle.
class CycleDriver {
public:
  enum class Step { cycle, pause, done };
  static constexpr double cycle_seconds = 0.02;
  static constexpr double pause_seconds = 5.0;

  CycleDriver() {
    const char *value = std::getenv("LAYERC_PROTO_CYCLES");
    cycles_ = value == nullptr ? 0 : std::max(0, std::atoi(value));
    const char *resume = std::getenv("LAYERC_PROTO_RESUME_FILE");
    if (resume != nullptr) resume_file_ = resume;
  }
  [[nodiscard]] bool active() const noexcept { return cycles_ > 0; }
  [[nodiscard]] Step next() {
    if (warmup_ > 0) { --warmup_; return Step::cycle; }
    if (!warm_) { warm_ = true; announce("warm"); return Step::pause; }
    if (!resume_file_.empty() && !std::filesystem::exists(resume_file_)) return Step::pause;
    if (cycles_ > 0) { --cycles_; measuring_ = true; return Step::cycle; }
    const auto validated = "cycles-validated=" + std::to_string(validated_);
    announce(validated.c_str());
    announce("cycles-done");
    return Step::done;
  }
  // Called once the cycle's validation dialog was rendered and destroyed.
  void completed() noexcept { if (measuring_) ++validated_; }

private:
  int cycles_{};
  int warmup_{10};
  int validated_{};
  bool warm_{};
  bool measuring_{};
  std::filesystem::path resume_file_;
};

} // namespace layerc::prototype
