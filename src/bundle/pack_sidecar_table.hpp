#pragma once

#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::bundle::detail {

// Bundles fold over core::ProfileModuleTable; they keep no module list.
using SidecarDesc = layerc::core::ModuleDescriptor;
inline constexpr const auto &kSidecars =
    layerc::core::ProfileModuleTable::descriptors;

using SidecarPathLists =
    std::array<std::vector<std::string>, kSidecars.size()>;

[[nodiscard]] constexpr std::size_t
sidecar_index(const SidecarDesc &desc) noexcept {
  for (std::size_t index = 0; index < kSidecars.size(); ++index)
    if (kSidecars[index].kind == desc.kind) return index;
  return kSidecars.size();
}

// Pack entry suffix: `modules/<ordinal>.<name>.json`.
[[nodiscard]] inline std::string sidecar_suffix(const SidecarDesc &desc) {
  return "." + std::string(desc.name) + ".json";
}

// A core module codec plus the platform store I/O the bundle needs.
template <typename Module> struct SidecarTraits : Module {
  static auto load(const std::filesystem::path &path,
                   const layerc::core::GameProfile &base) {
    return layerc::platform::load_module_sidecar<Module>(path, base);
  }
  static auto read(const std::filesystem::path &path) {
    return layerc::platform::read_durable_text(path,
                                               Module::descriptor.max_bytes);
  }
  static auto parse(const layerc::core::json::Value &root,
                    const layerc::core::GameProfile &base) {
    return Module::parse_value(root, base);
  }
};

// Calls `visitor(slot, traits)` for the module described by `desc`; every
// arm must return the same type.
template <typename Pack, typename Visitor>
decltype(auto) visit_sidecar(Pack &&profile, const SidecarDesc &desc,
                             Visitor &&visitor) {
  return layerc::core::ProfileModuleTable::visit(
      desc.kind, [&]<typename Module>(Module) -> decltype(auto) {
        return visitor(profile.template module<Module>(),
                       SidecarTraits<Module>{});
      });
}

[[nodiscard]] inline const SidecarDesc *
sidecar_desc_for_kind(const std::string_view kind) noexcept {
  return layerc::core::find_module(kind);
}

} // namespace layerc::bundle::detail
