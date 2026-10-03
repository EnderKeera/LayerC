#include "layerc/platform/automation_persistence.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/platform/profile_store.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace layerc::platform {
namespace {

void load_entry(AutomationPersistence::Entry &entry,
                const std::filesystem::path &path,
                const layerc::core::AutomationProfile &profile,
                const layerc::core::TimePoint now) {
  auto state = load_automation_state(path, profile);
  entry.profile = profile;
  entry.values = std::move(state.values);
  entry.due.reset();
  entry.checkpoint_error.reset();
  entry.state_load_error.reset();
  if (state.error.has_value()) {
    entry.state_load_error = path.string() + ": " + state.error->message;
    // The next successful checkpoint rewrites the unreadable store.
    layerc::core::automation_checkpoint_arm(entry.due, now);
  }
}

// Unsaved values carried across a profile change: only those the new
// profile's persistent variables still accept.
layerc::core::AutomationPersistentValues
accepted_values(const layerc::core::AutomationProfile &profile,
                const layerc::core::AutomationPersistentValues &values) {
  layerc::core::AutomationPersistentValues accepted;
  for (const auto &variable : profile.variables) {
    const auto found = values.find(variable.id);
    if (found != values.end() &&
        layerc::core::automation_persistent_value_accepted(variable,
                                                           found->second))
      accepted.emplace(variable.id, found->second);
  }
  return accepted;
}

} // namespace

const AutomationPersistence::Entry &
AutomationPersistence::attach(const std::filesystem::path &path,
                              const layerc::core::AutomationProfile &profile,
                              const layerc::core::TimePoint now,
                              const bool reload) {
  const auto [found, inserted] = entries_.try_emplace(path);
  auto &entry = found->second;
  if (inserted) {
    load_entry(entry, path, profile, now);
  } else if (entry.profile != profile) {
    if (entry.pending()) {
      entry.values = accepted_values(profile, entry.values);
      entry.profile = profile;
    } else {
      load_entry(entry, path, profile, now);
    }
  } else if (reload && !entry.pending()) {
    load_entry(entry, path, profile, now);
  }
  return entry;
}

void AutomationPersistence::publish(
    const std::filesystem::path &path,
    const layerc::core::AutomationPersistentValues &values,
    const layerc::core::TimePoint now) {
  const auto found = entries_.find(path);
  if (found == entries_.end() || found->second.values == values) return;
  found->second.values = values;
  // Shared debounce: the first mutation in a burst arms the deadline and
  // later ones keep it, so continuous input cannot postpone the write.
  layerc::core::automation_checkpoint_arm(found->second.due, now);
}

std::vector<std::filesystem::path>
AutomationPersistence::checkpoint(const layerc::core::TimePoint now,
                                  const bool force,
                                  const std::filesystem::path *only) {
  std::vector<std::filesystem::path> saved;
  for (auto &[path, entry] : entries_) {
    if (only != nullptr && path != *only) continue;
    const auto written = checkpoint_automation_state(
        path, entry.profile, entry.values, entry.due, now, force);
    if (const auto *error = std::get_if<ProfileWriteError>(&written)) {
      entry.checkpoint_error = std::string("automation checkpoint failed (") +
                               profile_write_error_text(*error) +
                               "): " + path.string();
    } else if (std::holds_alternative<CheckpointSaved>(written)) {
      entry.checkpoint_error.reset();
      entry.state_load_error.reset();
      saved.push_back(path);
    }
  }
  return saved;
}

void AutomationPersistence::retain(
    const std::span<const std::filesystem::path> live) {
  std::erase_if(entries_, [&](const auto &item) {
    return !item.second.pending() &&
           std::ranges::find(live, item.first) == live.end();
  });
}

const AutomationPersistence::Entry *
AutomationPersistence::find(const std::filesystem::path &path) const {
  const auto found = entries_.find(path);
  return found == entries_.end() ? nullptr : &found->second;
}

std::size_t AutomationPersistence::pending_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      entries_, [](const auto &item) { return item.second.pending(); }));
}

std::vector<std::filesystem::path>
AutomationPersistence::pending_paths() const {
  std::vector<std::filesystem::path> paths;
  for (const auto &[path, entry] : entries_)
    if (entry.pending()) paths.push_back(path);
  return paths;
}

std::optional<layerc::core::TimePoint>
AutomationPersistence::next_deadline() const {
  std::optional<layerc::core::TimePoint> next;
  for (const auto &[path, entry] : entries_)
    if (entry.due.has_value() && (!next.has_value() || *entry.due < *next))
      next = entry.due;
  return next;
}

std::vector<std::string> AutomationPersistence::diagnostics() const {
  std::vector<std::string> lines;
  for (const auto &[path, entry] : entries_) {
    if (entry.checkpoint_error) lines.push_back(*entry.checkpoint_error);
    if (entry.state_load_error) lines.push_back(*entry.state_load_error);
  }
  return lines;
}

} // namespace layerc::platform
