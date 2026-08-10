#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

inline constexpr std::uint32_t live_protocol_version = 1;
inline constexpr std::size_t max_live_message_bytes = 512U * 1024U;
inline constexpr std::size_t max_live_buffer_bytes =
    8U * max_live_message_bytes;
using ProfileRevision = std::uint64_t;
using EditSessionId = std::uint64_t;

enum class RevisionDecision : std::uint8_t { accept, duplicate, stale };
[[nodiscard]] RevisionDecision revision_decision(ProfileRevision incoming,
                                                 ProfileRevision current);

struct ProfileDelta {
  bool metadata_changed{};
  bool enablement_changed{};
  bool matcher_changed{};
  bool bindings_changed{};
  bool panels_changed{};
  bool cooldown_visuals_changed{};
  std::vector<std::string> added_cooldown_ids;
  std::vector<std::string> removed_cooldown_ids;
  std::vector<std::string> reset_cooldown_ids;

  [[nodiscard]] bool empty() const noexcept;
};

struct SessionReconcilePlan {
  bool recreate_instances{};
  bool rebuild_input_matcher{};
  bool rebuild_overlays{};
  std::vector<std::string> added_cooldown_ids;
  std::vector<std::string> removed_cooldown_ids;
  std::vector<std::string> reset_cooldown_ids;
  std::vector<std::string> preserved_cooldown_ids;
};

[[nodiscard]] ProfileDelta diff_profiles(const GameProfile &before,
                                         const GameProfile &after);
[[nodiscard]] SessionReconcilePlan
plan_session_reconciliation(const GameProfile &before,
                            const GameProfile &after);

struct NormalizedAnchor {
  double x{0.5};
  double y{0.5};

  bool operator==(const NormalizedAnchor &) const = default;
};

struct PixelRect {
  long left{};
  long top{};
  long right{};
  long bottom{};

  bool operator==(const PixelRect &) const = default;
};

[[nodiscard]] NormalizedAnchor normalized_anchor_for_drag(
    long pointer_x, long pointer_y, long grab_offset_x, long grab_offset_y,
    const PixelRect &target);
[[nodiscard]] PixelRect
overlay_rectangle(NormalizedAnchor anchor, const PixelRect &target, long width,
                  long height);

enum class LiveMessageType : std::uint8_t {
  handshake,
  committed_profile,
  begin_edit,
  live_profile,
  select_panel,
  anchor_changed,
  set_direct_drag,
  end_edit,
  heartbeat,
  runtime_status,
  acknowledgement,
  error,
};

struct LiveMessage {
  LiveMessageType type{LiveMessageType::handshake};
  std::uint32_t protocol_version{live_protocol_version};
  ProfileRevision revision{};
  EditSessionId edit_session{};
  std::uint64_t target_window{};
  std::string profile_key;
  std::string panel_id;
  std::string profile_json;
  std::string text;
  double anchor_x{0.5};
  double anchor_y{0.5};
  bool direct_drag{};
  bool final{};
};

struct LiveProtocolError {
  std::string message;
};

using LiveMessageResult = std::variant<LiveMessage, LiveProtocolError>;
using LiveFrameResult =
    std::variant<std::vector<std::byte>, LiveProtocolError>;

[[nodiscard]] std::string encode_live_message_json(const LiveMessage &message);
[[nodiscard]] LiveMessageResult
decode_live_message_json(std::string_view source);
[[nodiscard]] LiveFrameResult encode_live_frame(const LiveMessage &message);

class LiveFrameDecoder {
public:
  [[nodiscard]] std::vector<LiveMessageResult>
  push(std::span<const std::byte> bytes);
  // A disconnected editor can leave a frame-sized allocation behind. Release
  // it instead of retaining capacity in the always-on daemon.
  void clear() noexcept { std::vector<std::byte>().swap(buffer_); }
  [[nodiscard]] std::size_t buffered_bytes() const noexcept {
    return buffer_.size();
  }
  [[nodiscard]] std::size_t reserved_bytes() const noexcept {
    return buffer_.capacity();
  }

private:
  std::vector<std::byte> buffer_;
};

} // namespace layerc::core
