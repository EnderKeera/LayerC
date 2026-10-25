#pragma once

#include "layerc/core/live_draft_control.hpp"
#include "layerc/core/types.hpp"
#include "layerc/platform/profile_store.hpp"

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace layerc::daemon {

using LiveDraftResult =
    std::variant<layerc::core::LiveDraftReply, layerc::core::LiveDraftFailure>;

// The one unsaved editor draft LayerD applies in memory only. A draft names a
// stored profile by id, replaces only its base profile, and lives while its
// client renews the lease; ending or expiry restores the committed profile.
// Nothing here touches the store, sockets or display.
class LiveDraftHost {
public:
  // `changed` is set when the effective profiles differ afterwards.
  [[nodiscard]] LiveDraftResult handle(const layerc::core::LiveDraftRequest &request,
                                       std::span<const layerc::platform::StoredProfile> stored,
                                       layerc::core::TimePoint now, bool &changed);
  // Drops a lapsed lease or a draft whose stored profile disappeared; true
  // when the effective profiles changed.
  [[nodiscard]] bool expire(layerc::core::TimePoint now,
                            std::span<const layerc::platform::StoredProfile> stored);
  [[nodiscard]] std::optional<layerc::core::TimePoint> next_deadline() const noexcept;
  [[nodiscard]] std::vector<layerc::platform::StoredProfile>
  effective(std::vector<layerc::platform::StoredProfile> stored) const;
  [[nodiscard]] bool active() const noexcept { return draft_.has_value(); }
  // "profile-id rN" for status reporting, or empty.
  [[nodiscard]] std::string describe() const;

private:
  struct Draft {
    std::string client_id;
    std::string profile_id;
    std::uint64_t revision{};
    layerc::core::GameProfile profile;
    layerc::core::TimePoint expires{};
  };
  std::optional<Draft> draft_;
};

} // namespace layerc::daemon
