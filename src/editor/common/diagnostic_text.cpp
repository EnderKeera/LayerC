#include "diagnostic_text.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace layerc::editor {
namespace {

// `$.cooldownGroups[N].<key>` or `$.cooldownGroups[N].<key>[M]`.
struct GroupPath {
  std::size_t index{};
  std::string key;
  std::size_t member_index{};
  bool member{};
};

std::optional<GroupPath> group_path(const std::string &path) {
  constexpr std::string_view prefix = "$.cooldownGroups[";
  if (!path.starts_with(prefix)) return std::nullopt;
  const auto close = path.find(']', prefix.size());
  if (close == std::string::npos || close == prefix.size())
    return std::nullopt;
  std::size_t index{};
  for (std::size_t at = prefix.size(); at < close; ++at) {
    if (path[at] < '0' || path[at] > '9') return std::nullopt;
    index = index * 10U + static_cast<std::size_t>(path[at] - '0');
  }
  GroupPath result;
  result.index = index;
  if (close + 1 >= path.size()) return result;
  if (path[close + 1] != '.') return std::nullopt;
  const auto key_end = path.find_first_of(".[", close + 2);
  result.key = path.substr(close + 2, key_end == std::string::npos
                                         ? std::string::npos
                                         : key_end - (close + 2));
  if (key_end == std::string::npos || path[key_end] != '[') return result;
  const auto member_close = path.find(']', key_end + 1);
  if (member_close == std::string::npos || member_close == key_end + 1)
    return result;
  std::size_t member{};
  for (std::size_t at = key_end + 1; at < member_close; ++at) {
    if (path[at] < '0' || path[at] > '9') return result;
    member = member * 10U + static_cast<std::size_t>(path[at] - '0');
  }
  result.member = true;
  result.member_index = member;
  return result;
}

std::string_view field_label(const std::string_view key) {
  if (key == "id") return "ID";
  if (key == "name") return "Name";
  if (key == "cooldowns") return "Members";
  if (key == "durationMs") return "Duration";
  if (key == "maxCharges") return "Charges";
  if (key == "recovery") return "Recovery";
  if (key == "retrigger") return "Retrigger";
  if (key == "mode") return "Mode";
  return key;
}

std::string group_label(const GroupPath &path,
                        const std::span<const layerc::core::CooldownGroup> groups) {
  std::string label = "Group";
  if (path.index < groups.size() && !groups[path.index].name.empty())
    label += " \"" + groups[path.index].name + "\"";
  label += " (";
  if (path.index < groups.size() && !groups[path.index].id.empty())
    label += groups[path.index].id;
  else
    label += "row " + std::to_string(path.index + 1);
  label += ")";
  return label;
}

} // namespace

std::string join_sentences(std::string first, const std::string_view second) {
  if (!first.empty() && !first.ends_with('.') && !first.ends_with('!') &&
      !first.ends_with('?'))
    first += '.';
  if (!first.empty() && !second.empty()) first += ' ';
  first += second;
  return first;
}

std::string
cooldown_group_error_text(const layerc::core::ProfileError &error,
                          const std::span<const layerc::core::CooldownGroup> groups) {
  const auto parsed = group_path(error.path);
  if (!parsed) return error.message;
  const std::string label = group_label(*parsed, groups);
  if (parsed->key.empty()) return label + ": " + error.message;
  std::string field{field_label(parsed->key)};
  if (parsed->member && parsed->index < groups.size() &&
      parsed->member_index < groups[parsed->index].cooldown_ids.size())
    field += " \"" + groups[parsed->index].cooldown_ids[parsed->member_index] + "\"";
  return label + " " + field + ": " + error.message;
}

} // namespace layerc::editor
