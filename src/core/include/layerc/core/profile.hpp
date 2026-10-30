#pragma once

#include "layerc/core/document_error.hpp"
#include "layerc/core/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::core {
namespace json { class Value; }

inline constexpr std::size_t max_profile_json_bytes = 256U * 1024U;
inline constexpr std::uint32_t legacy_profile_schema_version = 4;
inline constexpr std::uint32_t profile_schema_version = 5;

[[nodiscard]] std::uint32_t
profile_serialization_schema(const GameProfile &profile) noexcept;

using ProfileError = DocumentError;

using ProfileResult = std::variant<GameProfile, ProfileError>;

// The one owner of profile semantics: bounds, counts, uniqueness, and
// references. Parsing runs it on every document; editors run it on in-memory
// edits instead of a serialize/parse round trip.
[[nodiscard]] std::optional<ProfileError>
validate_profile(const GameProfile &profile);

// True when any input binding triggers the cooldown.
[[nodiscard]] bool has_binding(const GameProfile &profile,
                               std::string_view cooldown_id) noexcept;

[[nodiscard]] ProfileResult parse_profile_json(std::string_view source);
// For a bounded document already accepted by json::parse (including UTF-8).
[[nodiscard]] ProfileResult parse_profile_value(const json::Value &root);
[[nodiscard]] std::string serialize_profile_json(const GameProfile &profile);

} // namespace layerc::core
