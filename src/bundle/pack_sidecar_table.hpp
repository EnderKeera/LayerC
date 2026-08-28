#pragma once

#include "layerc/core/companion.hpp"
#include "layerc/core/profile.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace layerc::bundle::detail {

enum class SidecarKind { visual, automation, companion };

struct SidecarDesc {
  SidecarKind type;
  std::string_view kind;
  std::string_view suffix; // ".visual.json" etc.
  std::size_t max_bytes;
  std::uint32_t max_schema;
};

inline constexpr std::array<SidecarDesc, 3> kSidecars{{
    {SidecarKind::visual, "visual", ".visual.json",
     layerc::core::max_visual_json_bytes, 2U},
    {SidecarKind::automation, "automation", ".automation.json",
     layerc::core::max_automation_json_bytes, 1U},
    {SidecarKind::companion, "companion", ".companion.json",
     layerc::core::max_companion_sidecar_bytes,
     layerc::core::companion_schema_version},
}};

inline std::optional<SidecarDesc>
sidecar_desc_for_kind(const std::string_view kind) {
  for (const auto &desc : kSidecars)
    if (desc.kind == kind)
      return desc;
  return std::nullopt;
}

static_assert(layerc::core::companion_schema_version == 1U,
              "bump kSidecars when companion schema changes");

} // namespace layerc::bundle::detail
