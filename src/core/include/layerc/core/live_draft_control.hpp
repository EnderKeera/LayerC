#pragma once

#include "layerc/core/enum_wire.hpp"
#include "layerc/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::core {
namespace json {
class Value;
}

// A typed, versioned live-draft payload for authenticated local transports:
// an editor publishes an unsaved base profile that the daemon applies in
// memory only, for the lifetime of a renewed lease. The codec owns no
// sockets, profiles or platform handles.
inline constexpr std::uint32_t live_draft_version = 1;
inline constexpr std::size_t live_draft_profile_bytes = 256U * 1024U;
inline constexpr std::size_t live_draft_client_bytes = 64;
inline constexpr Milliseconds live_draft_lease{2500};

enum class LiveDraftOperation : std::uint8_t { publish, renew, end };
inline constexpr std::array live_draft_operation_wire{
    EnumWireEntry{LiveDraftOperation::publish, "publish"},
    EnumWireEntry{LiveDraftOperation::renew, "renew"},
    EnumWireEntry{LiveDraftOperation::end, "end"},
};
enum class LiveDraftState : std::uint8_t { applied, duplicate, renewed, ended };
inline constexpr std::array live_draft_state_wire{
    EnumWireEntry{LiveDraftState::applied, "applied"},
    EnumWireEntry{LiveDraftState::duplicate, "duplicate"},
    EnumWireEntry{LiveDraftState::renewed, "renewed"},
    EnumWireEntry{LiveDraftState::ended, "ended"},
};

// Machine-readable control failures; the text stays presentation only.
enum class LiveDraftError : std::uint8_t {
  invalid_request,
  unavailable,
  owned_elsewhere,
  clock_limit,
  expired,
  revision_mismatch,
  stale_revision,
  invalid_profile,
  id_mismatch,
  unknown_profile,
};
inline constexpr std::array live_draft_error_wire{
    EnumWireEntry{LiveDraftError::invalid_request, "invalid-request"},
    EnumWireEntry{LiveDraftError::unavailable, "unavailable"},
    EnumWireEntry{LiveDraftError::owned_elsewhere, "owned-elsewhere"},
    EnumWireEntry{LiveDraftError::clock_limit, "clock-limit"},
    EnumWireEntry{LiveDraftError::expired, "expired"},
    EnumWireEntry{LiveDraftError::revision_mismatch, "revision-mismatch"},
    EnumWireEntry{LiveDraftError::stale_revision, "stale-revision"},
    EnumWireEntry{LiveDraftError::invalid_profile, "invalid-profile"},
    EnumWireEntry{LiveDraftError::id_mismatch, "id-mismatch"},
    EnumWireEntry{LiveDraftError::unknown_profile, "unknown-profile"},
};
struct LiveDraftFailure {
  LiveDraftError code{};
  std::string message;
  bool operator==(const LiveDraftFailure &) const = default;
};
[[nodiscard]] LiveDraftFailure live_draft_failure(LiveDraftError code);
struct LiveDraftRequest {
  LiveDraftOperation operation{LiveDraftOperation::publish};
  std::string profile_id;
  std::string client_id;
  // Monotonic per client; publish and renew require a nonzero revision.
  std::uint64_t revision{};
  // The complete base profile JSON for publish; empty otherwise.
  std::string profile_json;
  bool operator==(const LiveDraftRequest &) const = default;
};
struct LiveDraftReply {
  std::uint64_t revision{};
  LiveDraftState state{LiveDraftState::applied};
  bool operator==(const LiveDraftReply &) const = default;
};

[[nodiscard]] bool live_draft_request_valid(const LiveDraftRequest &) noexcept;
[[nodiscard]] std::string serialize_live_draft_request(const LiveDraftRequest &);
[[nodiscard]] std::optional<LiveDraftRequest> parse_live_draft_request(std::string_view);
[[nodiscard]] std::optional<LiveDraftRequest> parse_live_draft_request(const json::Value &value);
[[nodiscard]] std::string serialize_live_draft_reply(const LiveDraftReply &);
[[nodiscard]] std::optional<LiveDraftReply> parse_live_draft_reply(std::string_view);
[[nodiscard]] std::optional<LiveDraftReply> parse_live_draft_reply(const json::Value &value);
} // namespace layerc::core
