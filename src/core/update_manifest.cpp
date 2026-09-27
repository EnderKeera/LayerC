#include "layerc/core/update_manifest.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <array>

namespace layerc::core {
namespace {

constexpr std::array<std::string_view, 22> reserved_device_names{
    "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
    "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
    "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

[[nodiscard]] char ascii_lower(const char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a')
                                      : value;
}

[[nodiscard]] bool is_path_char(const char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '_' ||
         value == '-';
}

// Windows resolves "nul.txt" to the device too, so compare the part before
// the first dot.
[[nodiscard]] bool is_reserved_device(const std::string_view segment) noexcept {
  const auto stem = segment.substr(0, segment.find('.'));
  return std::ranges::any_of(reserved_device_names, [&](std::string_view name) {
    return stem.size() == name.size() &&
           std::equal(stem.begin(), stem.end(), name.begin(),
                      [](char left, char right) {
                        return ascii_lower(left) == right;
                      });
  });
}

[[nodiscard]] bool is_safe_segment(const std::string_view segment) noexcept {
  return !segment.empty() && segment.front() != '.' && segment.back() != '.' &&
         std::ranges::all_of(segment, is_path_char) &&
         !is_reserved_device(segment);
}

[[nodiscard]] bool is_commit_hex(const std::string_view commit) noexcept {
  return commit.size() == 40 &&
         std::ranges::all_of(commit, [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

[[nodiscard]] UpdateManifestError reader_error(const json::Reader &reader) {
  return {reader.error().path, reader.error().message};
}

void read_file(json::Reader &reader, const json::Value &entry,
               const std::string &path, UpdateFile &file) {
  if (!entry.is_object()) {
    reader.fail(path, "file entry must be an object");
    return;
  }
  if (!reader.exact_fields(entry, {"path", "module", "size", "sha256"}, path))
    return;
  reader.string(entry, "path", path, file.path);
  reader.string(entry, "module", path, file.module);
  if (const auto size = reader.unsigned_integer(entry, "size", path,
                                                max_update_file_bytes,
                                                json::Required::yes))
    file.size = *size;
  reader.string(entry, "sha256", path, file.sha256);
}

} // namespace

bool is_safe_update_path(const std::string_view path) noexcept {
  if (path.empty() || path.size() > max_update_path_bytes) return false;
  std::size_t depth = 0;
  std::size_t start = 0;
  while (true) {
    const auto end = path.find('/', start);
    const auto segment = path.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    if (!is_safe_segment(segment) || ++depth > max_update_path_depth)
      return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
}

bool is_update_name(const std::string_view name) noexcept {
  return !name.empty() && name.size() <= max_update_name_bytes &&
         std::ranges::all_of(name, [](char value) {
           return (value >= 'a' && value <= 'z') ||
                  (value >= '0' && value <= '9') || value == '-';
         });
}

std::optional<UpdateManifestError>
validate_update_manifest(const UpdateManifest &manifest) {
  if (manifest.schema_version != update_manifest_schema_version)
    return UpdateManifestError{"$.schemaVersion",
                               "unsupported update manifest schemaVersion"};
  if (!is_update_name(manifest.channel))
    return UpdateManifestError{"$.channel",
                               "channel must be 1-32 characters of [a-z0-9-]"};
  if (manifest.sequence == 0)
    return UpdateManifestError{"$.sequence", "sequence must be positive"};
  if (!is_commit_hex(manifest.commit))
    return UpdateManifestError{"$.commit",
                               "commit must be 40 lowercase hex digits"};
  if (manifest.files.empty())
    return UpdateManifestError{"$.files", "manifest lists no files"};
  if (manifest.files.size() > max_update_manifest_files)
    return UpdateManifestError{"$.files", "manifest lists too many files"};

  // Case-insensitive file systems would map two paths differing only in case
  // to one file, so uniqueness is checked on the lowercased path.
  std::vector<std::string> folded;
  folded.reserve(manifest.files.size());
  for (std::size_t index = 0; index < manifest.files.size(); ++index) {
    const auto &file = manifest.files[index];
    const std::string path = "$.files[" + std::to_string(index) + "]";
    if (!is_safe_update_path(file.path))
      return UpdateManifestError{path + ".path", "unsafe install path"};
    if (index != 0 && !(manifest.files[index - 1].path < file.path))
      return UpdateManifestError{path + ".path",
                                 "files must be sorted by path without duplicates"};
    if (!is_update_name(file.module))
      return UpdateManifestError{path + ".module",
                                 "module must be 1-32 characters of [a-z0-9-]"};
    if (file.size > max_update_file_bytes)
      return UpdateManifestError{path + ".size", "file exceeds the size limit"};
    if (!is_lowercase_sha256(file.sha256))
      return UpdateManifestError{path + ".sha256",
                                 "sha256 must be 64 lowercase hex digits"};
    std::string lowered = file.path;
    std::ranges::transform(lowered, lowered.begin(), ascii_lower);
    folded.push_back(std::move(lowered));
  }
  std::ranges::sort(folded);
  if (std::ranges::adjacent_find(folded) != folded.end())
    return UpdateManifestError{"$.files",
                               "two paths differ only in letter case"};
  return std::nullopt;
}

std::variant<UpdateManifest, UpdateManifestError>
parse_update_manifest_json(const std::string_view json) {
  const auto parsed = json::parse_bounded(json, max_update_manifest_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return UpdateManifestError{error->path, error->message};
  const auto &root = std::get<json::Value>(parsed);
  if (!root.is_object())
    return UpdateManifestError{"$", "update manifest root must be an object"};

  json::Reader reader;
  UpdateManifest manifest;
  if (!reader.exact_fields(
          root, {"schemaVersion", "channel", "sequence", "commit", "files"},
          "$"))
    return reader_error(reader);
  if (const auto schema = reader.integer(root, "schemaVersion", "$", 1,
                                         UINT32_MAX))
    manifest.schema_version = static_cast<std::uint32_t>(*schema);
  if (reader.ok() && manifest.schema_version != update_manifest_schema_version)
    return UpdateManifestError{"$.schemaVersion",
                               "unsupported update manifest schemaVersion"};
  reader.string(root, "channel", "$", manifest.channel);
  if (const auto sequence = reader.unsigned_integer(
          root, "sequence", "$", json::max_exact_json_integer,
          json::Required::yes))
    manifest.sequence = *sequence;
  reader.string(root, "commit", "$", manifest.commit);
  if (const auto *files = reader.array(root, "files", "$")) {
    if (files->size() > max_update_manifest_files)
      return UpdateManifestError{"$.files", "manifest lists too many files"};
    manifest.files.resize(files->size());
    std::size_t index = 0;
    reader.each(*files, "$.files",
                [&](const json::Value &entry, const std::string &path) {
                  read_file(reader, entry, path, manifest.files[index++]);
                });
  }
  if (!reader.ok()) return reader_error(reader);
  if (auto error = validate_update_manifest(manifest)) return *error;
  return manifest;
}

std::string serialize_update_manifest_json(const UpdateManifest &manifest) {
  std::string out = "{\"schemaVersion\":";
  json::append_unsigned(out, manifest.schema_version);
  out += ",\"channel\":";
  json::append_quoted(out, manifest.channel);
  out += ",\"sequence\":";
  json::append_unsigned(out, manifest.sequence);
  out += ",\"commit\":";
  json::append_quoted(out, manifest.commit);
  out += ",\"files\":[";
  json::Join join;
  for (const auto &file : manifest.files) {
    join(out);
    out += "{\"path\":";
    json::append_quoted(out, file.path);
    out += ",\"module\":";
    json::append_quoted(out, file.module);
    out += ",\"size\":";
    json::append_unsigned(out, file.size);
    out += ",\"sha256\":";
    json::append_quoted(out, file.sha256);
    out.push_back('}');
  }
  out += "]}";
  return out;
}

UpdateOrder compare_update_sequence(const std::uint64_t installed,
                                    const std::uint64_t candidate) noexcept {
  if (candidate < installed) return UpdateOrder::older;
  return candidate == installed ? UpdateOrder::same : UpdateOrder::newer;
}

UpdatePlan plan_update(const UpdateManifest *installed,
                       const UpdateManifest &target) {
  UpdatePlan plan;
  static const std::vector<UpdateFile> none;
  const auto &current = installed ? installed->files : none;
  // Both lists are sorted by path, so one merge pass classifies every file.
  std::size_t old_index = 0;
  for (std::size_t index = 0; index < target.files.size(); ++index) {
    const auto &file = target.files[index];
    while (old_index < current.size() && current[old_index].path < file.path)
      plan.remove.push_back(old_index++);
    const bool same_path =
        old_index < current.size() && current[old_index].path == file.path;
    if (same_path && current[old_index].size == file.size &&
        current[old_index].sha256 == file.sha256) {
      plan.reuse.push_back(index);
    } else {
      plan.download.push_back(index);
      plan.download_bytes += file.size;
    }
    if (same_path) ++old_index;
  }
  while (old_index < current.size()) plan.remove.push_back(old_index++);
  return plan;
}

} // namespace layerc::core
