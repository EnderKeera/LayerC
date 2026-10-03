#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/types.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::platform {

// The persistent automation state of every profile, keyed by profile path
// and owned independently of the sessions that match it (layer45 F-D1).
// Sessions attach to read the current values and publish their mutations;
// the debounced checkpoint, its retry, and checkpoint/state-load faults live
// here, so a session may end before its last write succeeds and a later
// session of the same profile continues from the unsaved values.
class AutomationPersistence {
public:
  struct Entry {
    layerc::core::AutomationProfile profile;
    layerc::core::AutomationPersistentValues values;
    // Debounced checkpoint deadline; set while values are not yet durable.
    std::optional<layerc::core::TimePoint> due;
    std::optional<std::string> state_load_error;
    std::optional<std::string> checkpoint_error;

    [[nodiscard]] bool pending() const noexcept { return due.has_value(); }
  };

  // The state of `path` under `profile`. First use loads the durable state.
  // A known entry keeps its values; `reload` re-reads the store when nothing
  // is pending. A changed profile keeps the unsaved values its persistent
  // variables still accept and otherwise reloads.
  const Entry &attach(const std::filesystem::path &path,
                      const layerc::core::AutomationProfile &profile,
                      layerc::core::TimePoint now, bool reload = false);

  // A session's runtime changed its persistent values: record them and arm
  // the shared debounce. Unknown paths are ignored.
  void publish(const std::filesystem::path &path,
               const layerc::core::AutomationPersistentValues &values,
               layerc::core::TimePoint now);

  // Writes every due entry (`force`: every pending one), or only `only`.
  // Returns the paths whose values are now durable.
  std::vector<std::filesystem::path>
  checkpoint(layerc::core::TimePoint now, bool force = false,
             const std::filesystem::path *only = nullptr);

  // Forgets settled entries whose path is not live; pending ones stay until
  // their write succeeds.
  void retain(std::span<const std::filesystem::path> live);

  [[nodiscard]] const Entry *find(const std::filesystem::path &path) const;
  [[nodiscard]] std::size_t pending_count() const noexcept;
  [[nodiscard]] std::vector<std::filesystem::path> pending_paths() const;
  [[nodiscard]] std::optional<layerc::core::TimePoint> next_deadline() const;
  // Current checkpoint and state-load faults, one line each.
  [[nodiscard]] std::vector<std::string> diagnostics() const;

private:
  std::map<std::filesystem::path, Entry> entries_;
};

} // namespace layerc::platform
