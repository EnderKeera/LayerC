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
[[nodiscard]] std::string serialize_live_draft_reply(const LiveDraftReply &);
[[nodiscard]] std::optional<LiveDraftReply> parse_live_draft_reply(std::string_view);
} // namespace layerc::core
