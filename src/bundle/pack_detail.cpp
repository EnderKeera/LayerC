#include "pack_detail.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/version.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include <algorithm>
#include <cwctype>
#include <limits>
#include <ranges>
#include <set>

namespace layerc::bundle::detail {
namespace {

// The manifest records each packed document's own schema. The parsed model can
// be newer because legacy sidecars are migrated in memory.
std::uint32_t document_schema(const std::string_view json) {
  const auto parsed = core::json::parse(json);
  const auto *root = std::get_if<core::json::Value>(&parsed);
  if (root == nullptr)
    return 0U;
  core::json::Reader fields;
  const auto schema = fields.unsigned_integer(
      *root, "schemaVersion", "$", std::numeric_limits<std::uint32_t>::max(),
      core::json::Required::yes);
  return schema.has_value() ? static_cast<std::uint32_t>(*schema) : 0U;
}

struct ModuleSource {
  std::optional<std::string_view> json;
  const std::string &path;
  std::uint32_t schema;
};

ModuleSource module_source(const PackProfile &profile, const SidecarDesc &desc,
                           const std::size_t index,
                           const SidecarPathLists &sidecar_paths) {
  return visit_sidecar(
      profile, desc, [&](const auto &module, const auto &) -> ModuleSource {
        return {module ? std::optional<std::string_view>(module->json)
                       : std::nullopt,
                sidecar_paths[desc.index][index],
                module ? document_schema(module->json) : 0U};
      });
}

// Appends the "path", "schemaVersion", "size", and "sha256" fields shared by
// base and module descriptors.
void append_source_fields(std::string &output, const std::string_view path,
                          const std::string_view json, const std::uint32_t schema) {
  output += "\"path\":";
  core::json::append_quoted(output, path);
  output += ",\"schemaVersion\":";
  core::json::append_unsigned(output, schema);
  output += ",\"size\":";
  core::json::append_unsigned(output, json.size());
  output += ",\"sha256\":";
  core::json::append_quoted(output, core::sha256(core::text_bytes(json)));
}

void append_module(std::string &output, bool &first,
                   const std::string_view kind, const std::string_view path,
                   const std::string_view json, const std::uint32_t schema) {
  if (!first)
    output.push_back(',');
  first = false;
  output += "{\"kind\":\"";
  output += kind;
  output += "\",";
  append_source_fields(output, path, json, schema);
  output += "}";
}

} // namespace

namespace {

bool is_safe_path_component(const std::string_view component) {
  const auto wide = core::try_utf8_to_wide(component);
  if (!wide.has_value() || wide->empty() || wide->size() > 240 ||
      wide->back() == L'.' || wide->back() == L' ')
    return false;
  for (const auto character : *wide) {
    if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) !=
                              std::wstring_view::npos)
      return false;
  }
  auto stem = wide->substr(0, wide->find(L'.'));
  std::ranges::transform(stem, stem.begin(), towupper);
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL")
    return false;
  if (stem.size() == 4 &&
      (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
      stem[3] >= L'1' && stem[3] <= L'9')
    return false;
  return true;
}

} // namespace

bool safe_entry_path(const std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos ||
      !core::try_utf8_to_wide(path).has_value())
    return false;
  std::size_t start{};
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto part =
        path.substr(start, end == std::string_view::npos ? path.size() - start
                                                         : end - start);
    if (part.empty() || part == "." || part == ".." ||
        !is_safe_path_component(part))
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1U;
  }
  return true;
}

std::filesystem::path
asset_path_for_profile(const std::filesystem::path &profile_path,
                       const std::string_view hash) {
  return platform::profile_visual_asset_path(profile_path, hash);
}

std::string
manifest_json(const std::vector<PackProfile> &profiles,
              const std::map<std::string, PackAsset, std::less<>> &assets,
              const std::vector<std::string> &base_paths,
              const SidecarPathLists &sidecar_paths) {
  std::string output =
      "{\n  \"schemaVersion\": 1,\n  \"kind\": \"layerc-profile-pack\",\n  "
      "\"producerVersion\": \"" + std::string(core::product_version) + "\",\n  \"profiles\": [\n";
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto &profile = profiles[index];
    output += "    {\"id\":";
    core::json::append_quoted(output, profile.profile.id);
    output += ",\"suggestedFilename\":";
    core::json::append_quoted(output, profile.suggested_filename);
    output += ",\"base\":{";
    append_source_fields(output, base_paths[index], profile.base_json,
                         document_schema(profile.base_json));
    output += "},\"modules\":[";
    bool first_module = true;
    for (const auto &desc : kSidecars) {
      const auto source = module_source(profile, desc, index, sidecar_paths);
      if (source.json.has_value())
        append_module(output, first_module, desc.kind, source.path,
                      *source.json, source.schema);
    }
    output += "],\"assets\":[";
    for (std::size_t asset = 0; asset < profile.asset_sha256.size(); ++asset) {
      if (asset != 0)
        output += ',';
      core::json::append_quoted(output, profile.asset_sha256[asset]);
    }
    output += "]}";
    output += index + 1U == profiles.size() ? "\n" : ",\n";
  }
  output += "  ],\n  \"assets\": [\n";
  std::size_t index{};
  for (const auto &[hash, asset] : assets) {
    output += "    {\"sha256\":";
    core::json::append_quoted(output, hash);
    output += ",\"path\":";
    core::json::append_quoted(output, "assets/" + hash + ".png");
    output += ",\"size\":";
    core::json::append_unsigned(output, asset.bytes.size());
    output += "}";
    output += ++index == assets.size() ? "\n" : ",\n";
  }
  output += "  ]\n}\n";
  return output;
}

std::vector<std::string>
VisualSidecarTraits::assets(const Profile &profile) {
  std::set<std::string, std::less<>> unique;
  for (const auto &state :
       layerc::core::observations_of<layerc::core::TemplateState>(
           profile.observations))
    for (const auto &reference : state.references)
      unique.insert(reference.asset_sha256);
  return {unique.begin(), unique.end()};
}

} // namespace layerc::bundle::detail
