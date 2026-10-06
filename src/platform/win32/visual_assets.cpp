#include "layerc/platform/visual_assets_windows.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/template_png.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_handle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include <windows.h>
#include <winioctl.h>

#include <cstring>

namespace layerc::platform {
namespace {

VisualAssetError failure(std::string message) {
  return {.message = std::move(message)};
}

std::optional<bool> visual_bundle_exists(
    const std::filesystem::path &profile_path, VisualAssetGcReport &report) {
  const auto paths = sidecar_paths(profile_path, core::ProfileModuleKind::visual);
  for (const auto &path : {paths.primary, paths.backup, paths.marker}) {
    const auto read =
        read_durable_file(path, layerc::core::max_visual_json_bytes);
    if (read.status == FileReadStatus::ok ||
        read.status == FileReadStatus::too_large)
      return true;
    if (read.status == FileReadStatus::io_error) {
      report.blocked_by = path;
      report.fault = "visual sidecar location could not be inspected";
      return std::nullopt;
    }
  }
  return false;
}

// Content-addressed stage name: 64 hex digits, ".png.", then the unique suffix.
bool is_asset_stage_filename(const std::filesystem::path &filename) {
  const auto name = path_to_utf8(filename);
  return name.has_value() && name->size() > 64U + 5U &&
         name->compare(64U, 5U, ".png.") == 0 &&
         layerc::core::is_lowercase_sha256(std::string_view(*name).substr(0, 64));
}

void collect_asset_references(const layerc::core::VisualProfile &profile,
                              std::vector<std::string> &references) {
  for (const auto &state : layerc::core::observations_of<layerc::core::TemplateState>(
           profile.observations)) {
    for (const auto &reference : state.references)
      references.push_back(reference.asset_sha256);
  }
}

bool collect_active_asset_references(std::vector<std::string> &references,
                                     VisualAssetGcReport &report) {
  const auto directory = profile_directory();
  const auto scan = scan_profile_directory(directory);
  if (!scan.error.empty()) {
    report.blocked_by = directory;
    report.fault = scan.error;
    return false;
  }
  for (const auto &fault : scan.faults) {
    const auto has_visual = visual_bundle_exists(fault.path, report);
    if (!has_visual.has_value()) return false;
    if (*has_visual) {
      report.blocked_by = fault.path;
      report.fault =
          "a malformed base profile still has visual sidecar state";
      return false;
    }
  }
  for (const auto &stored : scan.profiles) {
    const auto visual = load_visual_sidecar(stored.path, stored.profile);
    if (visual.state() == ModuleSidecarState::invalid) {
      report.blocked_by = visual.source_path();
      report.fault = (visual.error() == nullptr || visual.error()->message.empty())
                         ? "an active visual sidecar is invalid"
                         : visual.error()->message;
      return false;
    }
    if (visual.usable())
      collect_asset_references(*visual.profile(), references);
    // A valid recovery copy must retain its assets even while the primary is
    // healthy. A malformed stale backup is unusable and can be ignored.
    if (visual.state() == ModuleSidecarState::valid) {
      const auto paths =
          sidecar_paths(stored.path, core::ProfileModuleKind::visual);
      const auto backup = read_durable_file(
          paths.backup, layerc::core::max_visual_json_bytes);
      if (backup.status == FileReadStatus::io_error) {
        report.blocked_by = paths.backup;
        report.fault = "visual recovery sidecar could not be read";
        return false;
      }
      if (backup.has_value()) {
        const auto backup_json = layerc::core::as_text(*backup);
        auto parsed_backup = layerc::core::parse_visual_profile_json(
            backup_json, stored.profile);
        if (const auto *backup_profile =
                std::get_if<layerc::core::VisualProfile>(&parsed_backup))
          collect_asset_references(*backup_profile, references);
      }
    }
  }
  return true;
}

bool write_asset_atomically(const std::filesystem::path &path,
                            const std::span<const std::uint8_t> bytes) {
  if (!file_guard_ensure_directory(path.parent_path().parent_path()) ||
      !file_guard_ensure_directory(path.parent_path()) ||
      !file_guard_absent_or_regular(path)) return false;
  return write_durable_file(path, std::as_bytes(bytes)) ==
         DurablePublishOutcome::published_durable;
}

// An already-vanished file counts as collected; any other failure records
// `fault` and tells the caller to stop.
bool delete_asset_file(const std::filesystem::path &path,
                       VisualAssetGcReport &report, const char *const fault) {
  if (DeleteFileW(path.c_str()) != FALSE) {
    ++report.deleted;
    return true;
  }
  if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
  report.blocked_by = path;
  report.fault = fault;
  return false;
}

} // namespace

// The Windows store is unified: every profile shares one asset directory.
std::filesystem::path visual_asset_directory() {
  return unified_visual_asset_directory(profile_directory());
}

std::filesystem::path visual_asset_path(const std::string_view sha256_value) {
  return visual_asset_file(visual_asset_directory(), sha256_value);
}

VisualAssetSignature asset_signature(const std::string_view hash) {
  const auto path = visual_asset_path(hash);
  if (path.empty())
    return {};
  const OwnedHandle owned_file(
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  const HANDLE file = owned_file.get();
  if (!owned_file.valid())
    return {};
  BY_HANDLE_FILE_INFORMATION information{};
  if (GetFileInformationByHandle(file, &information) == FALSE)
    return {};
  const auto combine = [](const DWORD high, const DWORD low) {
    return (static_cast<std::uint64_t>(high) << 32U) | low;
  };
  // The change-journal USN binds the signature to content history: any
  // write bumps it, so a replacement that preserves size and mtime — and
  // even one whose MFT record reuses the same file index — still produces a
  // different signature and forces the worker to re-decode. Volumes without
  // a journal keep 0 and fall back to the metadata fields.
  std::uint64_t change_usn{};
  std::array<unsigned char, 128> journal{};
  DWORD journal_bytes{};
  if (DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0,
                      journal.data(), static_cast<DWORD>(journal.size()),
                      &journal_bytes, nullptr) &&
      journal_bytes >= sizeof(DWORD) + sizeof(WORD)) {
    WORD version{};
    std::memcpy(&version, journal.data() + sizeof(DWORD), sizeof(version));
    std::size_t usn_offset = std::size_t(-1);
    // V2: RecordLength(4) + versions(4) + two 8-byte file IDs = 24.
    // V3 replaces those file IDs with two 16-byte IDs, moving Usn to 40.
    // USN_RECORD is the V2-compatible alias; spell V3 manually for MinGW.
    static_assert(offsetof(USN_RECORD, Usn) == 24,
                  "USN_RECORD Usn offset changed — update V3 manual offset");
    if (version == 2)
      usn_offset = offsetof(USN_RECORD, Usn);
    else if (version == 3)
      usn_offset = sizeof(DWORD) + 2U * sizeof(WORD) +
                   2U * 2U * sizeof(std::uint64_t); // 4+4+32 = 40
    if (usn_offset != std::size_t(-1) &&
        journal_bytes >= usn_offset + sizeof(change_usn))
      std::memcpy(&change_usn, journal.data() + usn_offset, sizeof(change_usn));
  }
  return {
      .present = true,
      .volume_serial = information.dwVolumeSerialNumber,
      .file_index =
          combine(information.nFileIndexHigh, information.nFileIndexLow),
      .file_size = combine(information.nFileSizeHigh, information.nFileSizeLow),
      .last_write_ticks = combine(information.ftLastWriteTime.dwHighDateTime,
                                  information.ftLastWriteTime.dwLowDateTime),
      .change_usn = change_usn};
}

TemplateAssetResult
store_template_asset(const layerc::core::TemplateImage &image) {
  if (!layerc::core::template_image_has_structure(image))
    return failure(
        "template needs visible light/color variation so its position can be recognized");
  auto encoded = encode_template_png(image);
  if (const auto *error = std::get_if<VisualAssetError>(&encoded))
    return *error;
  auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
  const auto digest = layerc::core::sha256(std::as_bytes(std::span(bytes)));
  const auto destination = visual_asset_path(digest);
  if (destination.empty())
    return failure("content-addressed asset path is invalid");

  bool needs_write = true;
  if (auto existing = read_template_asset_bytes(destination); existing.has_value()) {
    needs_write = layerc::core::sha256(std::as_bytes(std::span(*existing))) != digest;
  }
  if (needs_write && !write_asset_atomically(destination, bytes))
    return failure("template asset could not be stored atomically");

  auto stored = image;
  stored.asset_sha256 = digest;
  return ImportedTemplateAsset{
      .reference = {.asset_sha256 = digest},
      .image = std::move(stored),
      .path = destination};
}

TemplateAssetResult
import_template_asset(const std::filesystem::path &source) {
  auto bytes = read_template_asset_bytes(source);
  if (!bytes.has_value())
    return failure("PNG is missing, unreadable, empty, or larger than 4 MiB");
  auto decoded = decode_template_png(*bytes);
  if (const auto *error = std::get_if<VisualAssetError>(&decoded))
    return *error;
  return store_template_asset(
      std::get<layerc::core::TemplateImage>(std::move(decoded)));
}

TemplateImageResult load_template_asset(const std::string_view sha256_value) {
  return verified_template_from_bytes(visual_asset_path(sha256_value),
                                      sha256_value);
}

namespace {

VisualAssetGcReport garbage_collect_visual_assets(
    const std::span<const std::string> referenced_sha256) {
  VisualAssetGcReport report;
  std::set<std::string, std::less<>> referenced(referenced_sha256.begin(),
                                                 referenced_sha256.end());
  std::error_code error;
  const auto directory = visual_asset_directory();
  const bool directory_exists = std::filesystem::exists(directory, error);
  if (error) {
    report.blocked_by = directory;
    report.fault = "visual asset directory could not be inspected";
    return report;
  }
  if (!directory_exists) return report;

  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      if (error) {
        report.blocked_by = iterator->path();
        report.fault = "visual asset entry could not be inspected";
        return report;
      }
      continue;
    }
    const auto path = iterator->path();
    if (_wcsicmp(path.extension().c_str(), L".tmp") == 0) {
      if (is_asset_stage_filename(path.filename())) {
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) {
          report.blocked_by = path;
          report.fault = "temporary visual asset timestamp could not be read";
          return report;
        }
        if (std::filesystem::file_time_type::clock::now() - modified >=
                std::chrono::hours(24) &&
            !delete_asset_file(
                path, report,
                "stale temporary visual asset could not be deleted"))
          return report;
      }
      continue;
    }
    if (path.extension() != L".png") continue;
    const auto stem = path_to_utf8(path.stem());
    if (!stem || !layerc::core::is_lowercase_sha256(*stem)) continue;
    if (referenced.contains(*stem)) {
      ++report.retained;
      continue;
    }
    if (!delete_asset_file(path, report,
                           "unreferenced visual asset could not be deleted"))
      return report;
  }
  if (error) {
    report.blocked_by = directory;
    report.fault = "visual asset directory enumeration failed";
  }
  return report;
}

} // namespace

VisualAssetGcReport garbage_collect_unreferenced_visual_assets() {
  VisualAssetGcReport report;
  const auto directory = profile_directory();
  // Offline maintenance is an empty profile-mutation plan. Reuse the existing
  // recovered writer boundary so recovery, reference scanning and deletion
  // cannot interleave with another bundle commit. No profile files are changed.
  const auto owned = commit_profile_transaction(
      directory, [&](std::vector<ProfileFileMutation> &) {
        std::vector<std::string> references;
        // Keep the same primary/backup sidecar recovery rules as LayerD. An
        // invalid active bundle aborts before any deletion, while an unusable
        // stale backup does not conceal an otherwise valid primary.
        if (collect_active_asset_references(references, report)) {
          std::ranges::sort(references);
          const auto unique = std::ranges::unique(references);
          references.erase(unique.begin(), unique.end());
          report = garbage_collect_visual_assets(references);
        }
        return ProfileWriteResult{};
      });
  if (!owned && report.completed()) {
    report.blocked_by = directory;
    report.fault = profile_write_error_text(*owned.failure);
  }
  return report;
}

} // namespace layerc::platform
