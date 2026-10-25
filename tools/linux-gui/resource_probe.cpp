// Test-only validator/history probe. Never installed or linked into layere.
// Its sampled process footprint is supplemental, not a substitute for GUI USS.
#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/core/visual.hpp"
#include "editor_history.hpp"
#include "automation_simulator.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace core = layerc::core;
namespace {
std::string bounded_read(const std::filesystem::path &path, const std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open fixture");
  std::string bytes(limit + 1, '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.bad()) throw std::runtime_error("cannot read fixture");
  bytes.resize(static_cast<std::size_t>(input.gcount()));
  return bytes;
}
void number(std::string &out, core::json::Join &join,
            const std::string_view name, const std::size_t value) {
  join(out);
  core::json::append_quoted(out, name);
  out += ':';
  core::json::append_unsigned(out, value);
}
void limits() {
  std::string out{"{"};
  core::json::Join join;
  number(out, join, "profileBytes", core::max_profile_json_bytes);
  number(out, join, "automationBytes", core::max_automation_json_bytes);
  number(out, join, "visualBytes", core::max_visual_json_bytes);
  number(out, join, "jsonDepth", core::json::maximum_nesting_depth);
  number(out, join, "historyCapacity", layerc::editor::EditorProfileHistory::capacity);
  number(out, join, "rules", core::max_automation_rules);
  number(out, join, "variables", core::max_automation_variables);
  number(out, join, "nodesPerRule", core::max_automation_nodes_per_rule);
  number(out, join, "astDepth", core::max_automation_ast_depth);
  number(out, join, "actionsPerRule", core::max_automation_actions_per_rule);
  number(out, join, "collectionEntries", core::max_automation_collection_entries);
  number(out, join, "stringBytes", core::max_automation_string_bytes);
  number(out, join, "traceEntries", core::max_automation_trace_entries);
  number(out, join, "traceBytes", core::max_automation_trace_bytes);
  number(out, join, "traceImportJsonValues",
         layerc::editor::max_trace_import_json_values);
  number(out, join, "pixelRules", core::max_pixel_rules);
  number(out, join, "templateStates", core::max_template_states);
  number(out, join, "referencesPerTemplate", core::max_template_references);
  number(out, join, "templateAxis", core::max_template_axis);
  out += '}';
  std::cout << out << '\n';
}
void rejected(const std::string_view path, const std::string_view message) {
  std::string out{"{\"accepted\":false,\"path\":"};
  core::json::append_quoted(out, path);
  out += ",\"message\":";
  core::json::append_quoted(out, message);
  out += '}';
  std::cout << out << '\n';
}
int inspect_trace(const std::filesystem::path &path) {
  auto result = core::parse_layerc_trace(
      bounded_read(path, core::max_automation_trace_bytes));
  if (const auto *error = std::get_if<core::AutomationError>(&result)) {
    rejected(error->path, error->message);
    return 2;
  }
  const auto &events = std::get<std::vector<core::AutomationEvent>>(result);
  std::string out{"{\"accepted\":true"};
  core::json::Join join;
  join(out);
  number(out, join, "rootEvents", events.size());
  out += '}';
  std::cout << out << '\n';
  return 0;
}
int compare_automation(const std::filesystem::path &base_path,
                       const std::filesystem::path &before_path,
                       const std::filesystem::path &after_path) {
  const auto base_result = core::parse_profile_json(
      bounded_read(base_path, core::max_profile_json_bytes));
  const auto *base = std::get_if<core::GameProfile>(&base_result);
  if (base == nullptr) {
    const auto &error = std::get<core::ProfileError>(base_result);
    rejected(error.path, error.message);
    return 2;
  }
  const auto before_result = core::parse_automation_profile_json(
      bounded_read(before_path, core::max_automation_json_bytes), *base);
  const auto after_result = core::parse_automation_profile_json(
      bounded_read(after_path, core::max_automation_json_bytes), *base);
  for (const auto *result : {&before_result, &after_result}) {
    if (const auto *error = std::get_if<core::AutomationError>(result)) {
      rejected(error->path, error->message);
      return 2;
    }
  }
  const bool equivalent = std::get<core::AutomationProfile>(before_result) ==
                          std::get<core::AutomationProfile>(after_result);
  std::cout << "{\"accepted\":true,\"equivalent\":"
            << (equivalent ? "true" : "false") << "}\n";
  return equivalent ? 0 : 2;
}

int inspect(const std::string_view mode, const std::filesystem::path &path,
            const std::optional<std::filesystem::path> &module_path) {
  auto result = core::parse_profile_json(bounded_read(path, core::max_profile_json_bytes));
  if (const auto *error = std::get_if<core::ProfileError>(&result)) {
    rejected(error->path, error->message);
    return 2;
  }
  auto profile = std::get<core::GameProfile>(std::move(result));
  std::optional<core::AutomationProfile> automation;
  std::optional<core::VisualProfile> visual;
  if (mode == "--automation" && module_path) {
    auto loaded = core::parse_automation_profile_json(
        bounded_read(*module_path, core::max_automation_json_bytes), profile);
    if (const auto *error = std::get_if<core::AutomationError>(&loaded)) {
      rejected(error->path, error->message);
      return 2;
    }
    automation = std::get<core::AutomationProfile>(std::move(loaded));
  } else if (mode == "--visual" && module_path) {
    auto loaded = core::parse_visual_profile_json(
        bounded_read(*module_path, core::max_visual_json_bytes), profile);
    if (const auto *error = std::get_if<core::VisualError>(&loaded)) {
      rejected(error->path, error->message);
      return 2;
    }
    visual = std::get<core::VisualProfile>(std::move(loaded));
  }
  const auto original = profile;
  layerc::editor::EditorProfileHistory history;
  history.reset(profile, visual, automation);
  if (mode == "--history") {
    for (std::size_t index{}; index < history.capacity; ++index) {
      profile.enabled = !profile.enabled;
      if (!history.record(profile, visual, automation))
        throw std::runtime_error("history did not record distinct valid edit");
    }
    // Hold the maximum retained history while the external owner samples it.
    std::cout << "{\"stage\":\"history-full\"}" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    for (std::size_t index{}; index < history.capacity; ++index) {
      auto snapshot = history.undo(profile, visual, automation);
      if (!snapshot) throw std::runtime_error("history lost authored undo state");
      profile = std::move(snapshot->profile);
    }
    if (profile != original || history.can_undo())
      throw std::runtime_error("history restore did not preserve the complete valid profile");
  }
  const auto serialized = core::serialize_profile_json(profile);
  if (serialized.size() <= core::max_profile_json_bytes) {
    auto reparsed = core::parse_profile_json(serialized);
    if (!std::holds_alternative<core::GameProfile>(reparsed) ||
        std::get<core::GameProfile>(reparsed) != original)
      throw std::runtime_error("validated profile did not round-trip without data loss");
  }
  // An accepted compact input can expand beyond the durable writer's byte
  // bound. Record that fact; never trim opaque data to make it writable.
  std::string out{"{\"accepted\":true"};
  core::json::Join join;
  join(out); // The accepted field is already present.
  number(out, join, "cooldowns", profile.cooldowns.size());
  number(out, join, "panels", profile.panels.size());
  number(out, join, "bindings", profile.bindings.size());
  number(out, join, "canonicalBytes", serialized.size());
  out += '}';
  std::cout << out << '\n';
  return 0;
}
} // namespace
int main(const int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--limits") {
      limits();
      return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "--compare-automation") {
      return compare_automation(argv[2], argv[3], argv[4]);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--trace") {
      return inspect_trace(argv[2]);
    }
    if ((argc == 3 && (std::string_view(argv[1]) == "--profile" ||
                       std::string_view(argv[1]) == "--history")) ||
        (argc == 4 && (std::string_view(argv[1]) == "--automation" ||
                       std::string_view(argv[1]) == "--visual"))) {
      return inspect(argv[1], argv[2], argc == 4
          ? std::optional<std::filesystem::path>{argv[3]} : std::nullopt);
    }
    throw std::runtime_error("expected --limits, --profile/--history/--trace PATH, --automation/--visual BASE MODULE or --compare-automation BASE BEFORE AFTER");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
