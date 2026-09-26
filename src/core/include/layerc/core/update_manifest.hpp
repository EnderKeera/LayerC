#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

// Describes one published build for the Windows launcher's launch-time
// updater. Each installed file is its own module entry, so a launcher can
// download only the files whose size or SHA-256 changed. The manifest bytes
// are signed as published; the signature is detached and never embedded.
inline constexpr std::uint32_t update_manifest_schema_version = 1;
inline constexpr std::size_t max_update_manifest_bytes = 256 * 1024;
inline constexpr std::size_t max_update_manifest_files = 256;
inline constexpr std::uint64_t max_update_file_bytes = 256ULL * 1024 * 1024;
inline constexpr std::size_t max_update_path_bytes = 200;
inline constexpr std::size_t max_update_path_depth = 4;
inline constexpr std::size_t max_update_name_bytes = 32;

struct UpdateFile {
  // Relative install path, '/'-separated; see is_safe_update_path.
  std::string path;
  // Lowercase group label ("layerd", "runtime", "docs") for diagnostics.
  std::string module;
  std::uint64_t size{};
  std::string sha256;

  bool operator==(const UpdateFile &) const = default;
};

struct UpdateManifest {
  std::uint32_t schema_version{update_manifest_schema_version};
  std::string channel;
  // Monotonic publish counter; a launcher never installs a lower sequence.
  std::uint64_t sequence{};
  // Full lowercase 40-hex commit the files were built from.
  std::string commit;
  // Strictly ascending by path (byte order), which makes output canonical.
  std::vector<UpdateFile> files;

  bool operator==(const UpdateManifest &) const = default;
};

struct UpdateManifestError {
  std::string path;
  std::string message;

  bool operator==(const UpdateManifestError &) const = default;
};

// A path that is safe to join under an install root on Windows and Linux:
// 1..max_update_path_depth segments of [A-Za-z0-9._-], none empty, none
// starting with '.', none ending with '.', and no Windows device names.
[[nodiscard]] bool is_safe_update_path(std::string_view path) noexcept;
// Lowercase [a-z0-9-], 1..max_update_name_bytes, for channel and module names.
[[nodiscard]] bool is_update_name(std::string_view name) noexcept;

[[nodiscard]] std::optional<UpdateManifestError>
validate_update_manifest(const UpdateManifest &manifest);
[[nodiscard]] std::variant<UpdateManifest, UpdateManifestError>
parse_update_manifest_json(std::string_view json);
// Emits compact, byte-stable JSON. Callers validate first; the serializer
// writes whatever it is given.
[[nodiscard]] std::string
serialize_update_manifest_json(const UpdateManifest &manifest);

enum class UpdateOrder { older, same, newer };
[[nodiscard]] UpdateOrder compare_update_sequence(std::uint64_t installed,
                                                  std::uint64_t candidate) noexcept;

// What a launcher must do to turn the installed build into the target build.
// Indexes point into target.files (download/reuse) and installed.files
// (remove). A file is reused only when path, size, and SHA-256 all match.
struct UpdatePlan {
  std::vector<std::size_t> download;
  std::vector<std::size_t> reuse;
  std::vector<std::size_t> remove;
  std::uint64_t download_bytes{};

  [[nodiscard]] bool changes_files() const noexcept {
    return !download.empty() || !remove.empty();
  }
};

// Both manifests must be valid (sorted, unique paths). With no installed
// manifest every target file is downloaded.
[[nodiscard]] UpdatePlan plan_update(const UpdateManifest *installed,
                                     const UpdateManifest &target);

} // namespace layerc::core
