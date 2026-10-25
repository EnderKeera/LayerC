#include "live_draft.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/profile.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace core = layerc::core;
namespace platform = layerc::platform;

namespace {
bool stored_id(std::span<const platform::StoredProfile> stored, const std::string_view id) {
  return std::ranges::any_of(stored, [&](const auto &item) { return item.profile.id == id; });
}
} // namespace

LiveDraftResult LiveDraftHost::handle(const core::LiveDraftRequest &request,
                                      std::span<const platform::StoredProfile> stored,
                                      const core::TimePoint now, bool &changed) {
  changed = false;
  if (!core::live_draft_request_valid(request)) return core::live_draft_failure(core::LiveDraftError::invalid_request);
  changed = expire(now, stored);
  const bool owned = draft_ && draft_->client_id == request.client_id &&
                     draft_->profile_id == request.profile_id;
  if (request.operation == core::LiveDraftOperation::end) {
    if (owned) {
      const auto revision = draft_->revision;
      draft_.reset();
      changed = true;
      return core::LiveDraftReply{revision, core::LiveDraftState::ended};
    }
    return core::LiveDraftReply{0, core::LiveDraftState::ended};
  }
  if (draft_ && !owned) return core::live_draft_failure(core::LiveDraftError::owned_elsewhere);
  const auto expires = core::automation_time_after(now, core::live_draft_lease);
  if (!expires) return core::live_draft_failure(core::LiveDraftError::clock_limit);
  if (request.operation == core::LiveDraftOperation::renew) {
    if (!owned) return core::live_draft_failure(core::LiveDraftError::expired);
    if (request.revision != draft_->revision) return core::live_draft_failure(core::LiveDraftError::revision_mismatch);
    draft_->expires = *expires;
    return core::LiveDraftReply{draft_->revision, core::LiveDraftState::renewed};
  }
  if (owned) {
    const auto decision = core::revision_decision(request.revision, draft_->revision);
    if (decision == core::RevisionDecision::stale) return core::live_draft_failure(core::LiveDraftError::stale_revision);
    if (decision == core::RevisionDecision::duplicate) {
      draft_->expires = *expires;
      return core::LiveDraftReply{draft_->revision, core::LiveDraftState::duplicate};
    }
  }
  auto parsed = core::parse_profile_json(request.profile_json);
  if (const auto *error = std::get_if<core::ProfileError>(&parsed))
    return core::LiveDraftFailure{core::LiveDraftError::invalid_profile,
                                  core::live_draft_failure(core::LiveDraftError::invalid_profile)
                                          .message +
                                      error->describe()};
  auto &profile = std::get<core::GameProfile>(parsed);
  if (profile.id != request.profile_id) return core::live_draft_failure(core::LiveDraftError::id_mismatch);
  if (!stored_id(stored, request.profile_id))
    return core::live_draft_failure(core::LiveDraftError::unknown_profile);
  draft_ = Draft{request.client_id, request.profile_id, request.revision, std::move(profile), *expires};
  changed = true;
  return core::LiveDraftReply{request.revision, core::LiveDraftState::applied};
}

bool LiveDraftHost::expire(const core::TimePoint now,
                           std::span<const platform::StoredProfile> stored) {
  if (!draft_ || (draft_->expires > now && stored_id(stored, draft_->profile_id))) return false;
  draft_.reset();
  return true;
}

std::optional<core::TimePoint> LiveDraftHost::next_deadline() const noexcept {
  if (!draft_) return std::nullopt;
  return draft_->expires;
}

std::vector<platform::StoredProfile>
LiveDraftHost::effective(std::vector<platform::StoredProfile> stored) const {
  if (!draft_) return stored;
  for (auto &item : stored)
    if (item.profile.id == draft_->profile_id) item.profile = draft_->profile;
  return stored;
}

std::string LiveDraftHost::describe() const {
  if (!draft_) return {};
  return draft_->profile_id + " r" + std::to_string(draft_->revision);
}

} // namespace layerc::daemon
