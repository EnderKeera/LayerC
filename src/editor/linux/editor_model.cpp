#include "editor_model.hpp"

#include "profile_edit.hpp"
#include "layerc/platform/profile_bundle_load.hpp"

#include <charconv>
#include <cmath>
#include <utility>
#include <variant>

namespace layerc::editor {
namespace {

std::optional<double> anchor(const std::string &text) {
  if (text.empty()) return std::nullopt;
  double value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value) || value < 0.0 || value > 1.0)
    return std::nullopt;
  return value;
}

std::string decimal(const double value) {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  return error == std::errc{} ? std::string(buffer, end) : std::string{};
}

} // namespace

LinuxEditorModel::LinuxEditorModel(std::filesystem::path directory)
    : directory_(std::move(directory)) {
  scan();
}

void LinuxEditorModel::scan() {
  scan_ = layerc::platform::scan_profile_directory(directory_);
}

std::optional<std::string> LinuxEditorModel::open(const std::size_t row) {
  if (!scan_.error.empty()) return scan_.error;
  if (row >= scan_.profiles.size()) return "No profile selected.";
  auto loaded = layerc::platform::load_profile_bundle(scan_.profiles[row].path);
  auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded);
  if (!bundle) return "Could not read the selected profile; its files were not changed.";
  document_.assign_bundle(scan_.profiles[row].path, std::move(*bundle));
  document_.set_dirty(false);
  document_.reset_history();
  opened_ = true;
  return std::nullopt;
}

std::optional<std::string> LinuxEditorModel::open_path(
    const std::filesystem::path &path) {
  for (std::size_t row = 0; row < scan_.profiles.size(); ++row)
    if (scan_.profiles[row].path == path) return open(row);
  return "The selected profile is no longer in the profile list.";
}

SliceFields LinuxEditorModel::fields() const {
  SliceFields fields;
  if (!opened_) return fields;
  const auto &profile = document_.profile();
  fields.name = profile.name;
  fields.executable = profile.game.executable_name;
  fields.enabled = profile.enabled;
  if (!profile.cooldowns.empty()) {
    const auto &cooldown = profile.cooldowns.front();
    fields.cooldown_name = cooldown.name;
    fields.duration_seconds =
        decimal(static_cast<double>(cooldown.duration.count()) / 1000.0);
  }
  if (!profile.panels.empty()) {
    fields.panel_x = decimal(profile.panels.front().anchor_x);
    fields.panel_y = decimal(profile.panels.front().anchor_y);
  }
  return fields;
}

std::optional<std::string> LinuxEditorModel::apply(const SliceFields &fields) {
  if (!opened_) return "No profile selected.";
  auto candidate = document_.profile();
  candidate.name = fields.name;
  candidate.game.executable_name = fields.executable;
  candidate.enabled = fields.enabled;
  if (!candidate.cooldowns.empty()) {
    const auto duration = parse_duration_seconds(fields.duration_seconds);
    if (!duration) return "Duration must be positive seconds within the profile limit.";
    candidate.cooldowns.front().name = fields.cooldown_name;
    candidate.cooldowns.front().duration = *duration;
  }
  if (!candidate.panels.empty()) {
    const auto x = anchor(fields.panel_x);
    const auto y = anchor(fields.panel_y);
    if (!x || !y) return "Panel X and Y must be numbers between 0 and 1.";
    candidate.panels.front().anchor_x = *x;
    candidate.panels.front().anchor_y = *y;
  }
  if (const auto error = layerc::core::validate_profile(candidate))
    return profile_error_line(*error);
  if (candidate != document_.profile()) {
    document_.profile() = std::move(candidate);
    (void)document_.history().record(document_.profile(), document_.visual(),
                                      document_.automation());
    document_.set_dirty(true);
  }
  return std::nullopt;
}

SaveOutcome LinuxEditorModel::save(const SliceFields &fields,
                                   const ConfirmUpgrade &confirm_upgrade) {
  if (const auto error = apply(fields))
    return {{CommitResult::invalid_fields, std::nullopt}, std::nullopt,
            "Not saved: " + *error};
  const auto commit = commit_document(document_, document_.path(), {}, confirm_upgrade);
  std::optional<ReloadResult> reload;
  if (commit.ok()) {
    document_.mark_saved();
    reload = request_daemon_reload();
    scan();
  }
  return {commit, reload, save_report_text(commit, reload)};
}

} // namespace layerc::editor
