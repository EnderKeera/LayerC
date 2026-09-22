#pragma once

#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/json.hpp"
#include "layerc/platform/profile_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::bundle::detail {

enum class SidecarKind { visual, automation, companion };

struct SidecarDesc {
  std::size_t index;
  SidecarKind type;
  std::string_view kind;
  std::string_view suffix; // ".visual.json" etc.
  std::size_t max_bytes;
  std::uint32_t max_schema;
  std::uint32_t minimum_profile_schema;
  layerc::core::ProfileModuleKind module_kind;
  layerc::platform::ModuleSidecarKind platform_kind;
};

inline constexpr std::array<SidecarDesc, 3> kSidecars{{
    {0U, SidecarKind::visual, "visual", ".visual.json",
     layerc::core::max_visual_json_bytes, 2U, 3U,
     layerc::core::ProfileModuleKind::visual,
     layerc::platform::ModuleSidecarKind::visual},
    {1U, SidecarKind::automation, "automation", ".automation.json",
     layerc::core::max_automation_json_bytes, 1U, 4U,
     layerc::core::ProfileModuleKind::automation,
     layerc::platform::ModuleSidecarKind::automation},
    {2U, SidecarKind::companion, "companion", ".companion.json",
     layerc::core::max_companion_sidecar_bytes,
     layerc::core::companion_schema_version, 1U,
     layerc::core::ProfileModuleKind::companion,
     layerc::platform::ModuleSidecarKind::companion},
}};

using SidecarPathLists =
    std::array<std::vector<std::string>, kSidecars.size()>;

struct VisualSidecarTraits {
  using Profile = layerc::core::VisualProfile;
  static auto load(const std::filesystem::path &path,
                   const layerc::core::GameProfile &base) {
    return layerc::platform::load_visual_sidecar(path, base);
  }
  static auto read(const std::filesystem::path &path) {
    return layerc::platform::read_sidecar_text(
        path, layerc::core::max_visual_json_bytes);
  }
  static auto parse(const layerc::core::json::Value &root,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_visual_profile_value(root, base);
  }
  static auto serialize(const Profile &profile) {
    return layerc::core::serialize_visual_profile_json(profile);
  }
};

struct AutomationSidecarTraits {
  using Profile = layerc::core::AutomationProfile;
  static auto load(const std::filesystem::path &path,
                   const layerc::core::GameProfile &base) {
    return layerc::platform::load_automation_sidecar(path, base);
  }
  static auto read(const std::filesystem::path &path) {
    return layerc::platform::read_sidecar_text(
        path, layerc::core::max_automation_json_bytes);
  }
  static auto parse(const layerc::core::json::Value &root,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_automation_profile_value(root, base);
  }
  static auto serialize(const Profile &profile) {
    return layerc::core::serialize_automation_profile_json(profile);
  }
};

struct CompanionSidecarTraits {
  using Profile = layerc::core::CompanionProfile;
  static auto load(const std::filesystem::path &path,
                   const layerc::core::GameProfile &base) {
    return layerc::platform::load_companion_sidecar(path, base);
  }
  static auto read(const std::filesystem::path &path) {
    return layerc::platform::read_sidecar_text(
        path, layerc::core::max_companion_sidecar_bytes);
  }
  static auto parse(const layerc::core::json::Value &root,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_companion_profile_value(root, base);
  }
  static auto serialize(const Profile &profile) {
    return layerc::core::serialize_companion_profile_json(profile);
  }
};

// This is the only SidecarKind dispatch. Consumers supply one typed operation;
// adding a sidecar requires one descriptor row, one traits type, and one arm.
template <typename Pack, typename Visitor>
decltype(auto) visit_sidecar(Pack &&profile, const SidecarDesc &desc,
                             Visitor &&visitor) {
  switch (desc.type) {
  case SidecarKind::visual:
    return std::forward<Visitor>(visitor)(
        std::forward<Pack>(profile).visual, VisualSidecarTraits{});
  case SidecarKind::automation:
    return std::forward<Visitor>(visitor)(
        std::forward<Pack>(profile).automation, AutomationSidecarTraits{});
  case SidecarKind::companion:
    return std::forward<Visitor>(visitor)(
        std::forward<Pack>(profile).companion, CompanionSidecarTraits{});
  }
  std::terminate();
}

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
