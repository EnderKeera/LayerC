#include "pack_detail.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"
#include "pack_sidecar_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <system_error>

#include <windows.h>

namespace layerc::bundle::detail {
namespace {

struct ModuleSource {
  const std::optional<std::string> &json;
  const std::string &path;
};

ModuleSource module_source(const PackProfile &profile, const SidecarKind kind,
                           const std::size_t index,
                           const std::vector<std::string> &visual_paths,
                           const std::vector<std::string> &automation_paths,
                           const std::vector<std::string> &companion_paths) {
  switch (kind) {
  case SidecarKind::visual:
    return {profile.visual_json, visual_paths[index]};
  case SidecarKind::automation:
    return {profile.automation_json, automation_paths[index]};
  case SidecarKind::companion:
    return {profile.companion_json, companion_paths[index]};
  }
  std::terminate();
}

void append_module(std::string &output, bool &first,
                   const std::string_view kind, const std::string_view path,
                   const std::string &json) {
  if (!first)
    output.push_back(',');
  first = false;
  output += "{\"kind\":\"";
  output += kind;
  output += "\",\"path\":";
  core::json::append_quoted(output, path);
  output += ",\"schemaVersion\":";
  core::json::append_unsigned(output, source_schema(json).value_or(0));
  output += ",\"size\":";
  core::json::append_unsigned(output, json.size());
  output += ",\"sha256\":";
  core::json::append_quoted(output, core::sha256(json));
  output += "}";
}

} // namespace

PackOperationResult failure(std::string message) {
  return PackError{std::move(message)};
}

PackValidationResult validation_failure(std::string message) {
  return PackError{std::move(message)};
}

std::optional<std::vector<std::byte>>
read_file(const std::filesystem::path &path, const std::size_t maximum) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum ||
      size > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::streamsize>::max()))
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
    return std::nullopt;
  return bytes;
}

std::string as_string(const std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::vector<std::byte> as_bytes(const std::string_view source) {
  std::vector<std::byte> bytes(source.size());
  std::memcpy(bytes.data(), source.data(), source.size());
  return bytes;
}

bool safe_entry_path(const std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos ||
      !platform::try_utf8_to_wide(path).has_value())
    return false;
  const auto safe_component = [](const std::string_view component) {
    const auto wide = platform::try_utf8_to_wide(component);
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
  };
  std::size_t start{};
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto part =
        path.substr(start, end == std::string_view::npos ? path.size() - start
                                                         : end - start);
    if (part.empty() || part == "." || part == ".." || !safe_component(part))
      return false;
    if (end == std::string_view::npos)
      break;
    start = end + 1U;
  }
  return true;
}

std::filesystem::path
asset_path_for_profile_root(const std::filesystem::path &profile_root,
                            const std::string_view hash) {
  if (!core::is_lowercase_sha256(hash))
    return {};
  return profile_root / L"_visual" / L"assets" /
         (std::wstring(hash.begin(), hash.end()) + L".png");
}

const core::json::Value *required(const core::json::Value &object,
                                  const std::string_view key) {
  return object.is_object() ? object.find(key) : nullptr;
}

std::optional<std::string> required_string(const core::json::Value &object,
                                           const std::string_view key) {
  const auto *value = required(object, key);
  if (value == nullptr || !value->is_string())
    return std::nullopt;
  return value->as_string();
}

std::optional<std::uint64_t> required_unsigned(const core::json::Value &object,
                                               const std::string_view key) {
  const auto *value = required(object, key);
  if (value == nullptr || !value->is_number() ||
      !std::isfinite(value->as_number()) || value->as_number() < 0.0 ||
      std::floor(value->as_number()) != value->as_number() ||
      value->as_number() >
          static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
    return std::nullopt;
  return static_cast<std::uint64_t>(value->as_number());
}

bool exact_fields(const core::json::Value &object,
                  const std::initializer_list<std::string_view> allowed) {
  if (!object.is_object())
    return false;
  for (const auto &[key, unused] : object.as_object()) {
    (void)unused;
    if (std::ranges::find(allowed, key) == allowed.end())
      return false;
  }
  return true;
}

std::optional<std::uint32_t> source_schema(const std::string_view json) {
  const auto parsed = core::json::parse(json);
  const auto *root = std::get_if<core::json::Value>(&parsed);
  if (root == nullptr)
    return std::nullopt;
  const auto schema = required_unsigned(*root, "schemaVersion");
  if (!schema.has_value() ||
      *schema > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return static_cast<std::uint32_t>(*schema);
}

std::optional<std::string>
manifest_json(const std::vector<PackProfile> &profiles,
              const std::map<std::string, PackAsset, std::less<>> &assets,
              const std::vector<std::string> &base_paths,
              const std::vector<std::string> &visual_paths,
              const std::vector<std::string> &automation_paths,
              const std::vector<std::string> &companion_paths) {
  std::string output =
      "{\n  \"schemaVersion\": 1,\n  \"kind\": \"layerc-profile-pack\",\n  "
      "\"producerVersion\": \"2.4.0\",\n  \"profiles\": [\n";
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto &profile = profiles[index];
    output += "    {\"id\":";
    core::json::append_quoted(output, profile.profile.id);
    output += ",\"suggestedFilename\":";
    core::json::append_quoted(output, profile.suggested_filename);
    output += ",\"base\":{\"path\":";
    core::json::append_quoted(output, base_paths[index]);
    output += ",\"schemaVersion\":";
    core::json::append_unsigned(output,
                                source_schema(profile.base_json).value_or(0));
    output += ",\"size\":";
    core::json::append_unsigned(output, profile.base_json.size());
    output += ",\"sha256\":";
    core::json::append_quoted(output, core::sha256(profile.base_json));
    output += "},\"modules\":[";
    bool first_module = true;
    for (const auto &desc : kSidecars) {
      const auto source = module_source(profile, desc.type, index, visual_paths,
                                        automation_paths, companion_paths);
      if (source.json.has_value())
        append_module(output, first_module, desc.kind, source.path,
                      *source.json);
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
visual_asset_references(const core::VisualProfile &profile) {
  std::set<std::string, std::less<>> unique;
  for (const auto &state : profile.templates)
    for (const auto &reference : state.references)
      unique.insert(reference.asset_sha256);
  return {unique.begin(), unique.end()};
}

bool write_file_atomically(const std::filesystem::path &destination,
                           const std::span<const std::byte> bytes) {
  std::error_code error;
  if (!destination.parent_path().empty()) {
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
      return false;
  }
  auto temporary = destination;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      return false;
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      std::filesystem::remove(temporary, error);
      return false;
    }
  }
  if (MoveFileExW(temporary.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

} // namespace layerc::bundle::detail
