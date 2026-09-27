#include "layerc/launcher/update_engine.hpp"

#include "layerc/core/hash.hpp"

#include <cstddef>
#include <fstream>
#include <span>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace layerc::launcher {
namespace {

namespace fs = std::filesystem;
using core::UpdateFile;
using core::UpdateManifest;

[[nodiscard]] UpdateError fail(const UpdateStage stage, std::string message) {
  return {stage, std::move(message)};
}

[[nodiscard]] std::string sha256_of(const std::string_view bytes) {
  return core::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// Reads at most max_bytes; a larger file is rejected rather than truncated.
[[nodiscard]] std::optional<std::string> read_bounded(const fs::path &path,
                                                      const std::size_t max_bytes) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > max_bytes) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
    return std::nullopt;
  return bytes;
}

[[nodiscard]] bool write_bytes(const fs::path &path, const std::string_view bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  return static_cast<bool>(output);
}

// Replaces `target` in one step. std::filesystem::rename does not replace an
// existing file under MinGW's runtime, so Windows asks for it explicitly.
[[nodiscard]] bool replace_file(const fs::path &source, const fs::path &target) {
#ifdef _WIN32
  return MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  fs::rename(source, target, error);
  return !error;
#endif
}

[[nodiscard]] bool write_pointer(const fs::path &target, const std::string_view id) {
  auto temporary = target;
  temporary += ".tmp";
  return write_bytes(temporary, std::string(id) + "\n") &&
         replace_file(temporary, target);
}

[[nodiscard]] std::optional<std::string> read_pointer(const fs::path &path) {
  auto text = read_bounded(path, 128);
  if (!text) return std::nullopt;
  while (!text->empty() && (text->back() == '\n' || text->back() == '\r'))
    text->pop_back();
  // A build id is one safe path segment.
  if (!core::is_safe_update_path(*text) ||
      text->find('/') != std::string::npos)
    return std::nullopt;
  return text;
}

[[nodiscard]] std::variant<InstalledBuild, UpdateError>
load_build(const InstallLayout &layout, const std::string &id) {
  const auto manifest_path =
      layout.build_directory(id) / std::string(installed_manifest_name);
  const auto bytes = read_bounded(manifest_path, core::max_update_manifest_bytes);
  if (!bytes)
    return fail(UpdateStage::read_installed,
                "build " + id + " has no readable manifest");
  auto parsed = core::parse_update_manifest_json(*bytes);
  if (const auto *error = std::get_if<core::UpdateManifestError>(&parsed))
    return fail(UpdateStage::read_installed, "build " + id + " manifest " +
                                                 error->path + ": " +
                                                 error->message);
  auto &manifest = std::get<UpdateManifest>(parsed);
  if (build_id_for(manifest) != id)
    return fail(UpdateStage::read_installed,
                "build " + id + " manifest names another build");
  return InstalledBuild{id, std::move(manifest)};
}

[[nodiscard]] bool file_matches(const fs::path &path, const UpdateFile &file) {
  const auto bytes = read_bounded(path, core::max_update_file_bytes);
  return bytes && bytes->size() == file.size && sha256_of(*bytes) == file.sha256;
}

[[nodiscard]] fs::path install_path(const fs::path &directory,
                                    const std::string &relative) {
  return directory / fs::path(relative).make_preferred();
}

// Keeps the active and rollback builds; everything else under app/ and all of
// staging/ is scratch. Removal failures (a file still open) are left for the
// next launch.
void prune(const InstallLayout &layout, const std::string &current,
           const std::optional<std::string> &previous) {
  std::error_code error;
  fs::remove_all(layout.staging_directory(), error);
  for (fs::directory_iterator it(layout.app_directory(), error), end;
       !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name == current || (previous && name == *previous)) continue;
    std::error_code ignored;
    fs::remove_all(it->path(), ignored);
  }
}

// Copies an unchanged file from the active build, re-checking its hash so a
// damaged local copy is downloaded again instead of carried forward.
[[nodiscard]] bool reuse_file(const fs::path &from, const fs::path &to,
                              const UpdateFile &file) {
  if (!file_matches(from, file)) return false;
  std::error_code error;
  fs::create_directories(to.parent_path(), error);
  if (error) return false;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
  return !error;
}

} // namespace

std::string_view stage_name(const UpdateStage stage) noexcept {
  switch (stage) {
  case UpdateStage::read_installed: return "read installed build";
  case UpdateStage::fetch_manifest: return "download manifest";
  case UpdateStage::verify_signature: return "verify manifest signature";
  case UpdateStage::parse_manifest: return "read manifest";
  case UpdateStage::check_channel: return "check channel";
  case UpdateStage::check_sequence: return "check build order";
  case UpdateStage::prepare_staging: return "prepare staging";
  case UpdateStage::fetch_file: return "download file";
  case UpdateStage::verify_file: return "verify file";
  case UpdateStage::write_file: return "write file";
  case UpdateStage::activate: return "activate build";
  }
  return "update";
}

std::string build_id_for(const UpdateManifest &manifest) {
  return std::to_string(manifest.sequence) + "-" + manifest.commit.substr(0, 12);
}

std::variant<std::optional<InstalledBuild>, UpdateError>
read_installed_build(const InstallLayout &layout) {
  std::error_code error;
  if (!fs::exists(layout.current_file(), error))
    return std::optional<InstalledBuild>{};
  const auto id = read_pointer(layout.current_file());
  if (!id)
    return fail(UpdateStage::read_installed, "the current build pointer is damaged");
  auto build = load_build(layout, *id);
  if (auto *failure = std::get_if<UpdateError>(&build)) return std::move(*failure);
  return std::optional<InstalledBuild>{std::move(std::get<InstalledBuild>(build))};
}

std::variant<UpdateReport, UpdateError>
apply_update(const InstallLayout &layout, const std::string_view channel,
             ReleaseSource &source, SignatureVerifier &verifier) {
  std::optional<InstalledBuild> installed;
  if (auto read = read_installed_build(layout);
      auto *build = std::get_if<std::optional<InstalledBuild>>(&read))
    installed = std::move(*build);

  const auto manifest_bytes =
      source.fetch(manifest_asset_name, core::max_update_manifest_bytes);
  if (!manifest_bytes)
    return fail(UpdateStage::fetch_manifest, "the release manifest is unavailable");
  const auto signature = source.fetch(signature_asset_name, max_signature_bytes);
  if (!signature)
    return fail(UpdateStage::fetch_manifest, "the manifest signature is unavailable");
  if (!verifier.verify(*manifest_bytes, *signature))
    return fail(UpdateStage::verify_signature,
                "the manifest signature does not match the LayerC release key");

  auto parsed = core::parse_update_manifest_json(*manifest_bytes);
  if (const auto *error = std::get_if<core::UpdateManifestError>(&parsed))
    return fail(UpdateStage::parse_manifest, error->path + ": " + error->message);
  const auto &target = std::get<UpdateManifest>(parsed);
  if (target.channel != channel)
    return fail(UpdateStage::check_channel,
                "the release is for channel " + target.channel);

  const auto id = build_id_for(target);
  if (installed) {
    switch (core::compare_update_sequence(installed->manifest.sequence,
                                          target.sequence)) {
    case core::UpdateOrder::older:
      return fail(UpdateStage::check_sequence,
                  "the release is older than the installed build " + installed->id);
    case core::UpdateOrder::same:
      if (installed->manifest.commit != target.commit)
        return fail(UpdateStage::check_sequence,
                    "the release reuses the installed sequence for another commit");
      return UpdateReport{.result = UpdateResult::up_to_date,
                          .build_id = installed->id,
                          .reused_files = target.files.size()};
    case core::UpdateOrder::newer:
      break;
    }
  }

  std::error_code error;
  fs::remove_all(layout.staging_directory(), error);
  const auto staging = layout.staging_directory() / id;
  fs::create_directories(staging, error);
  if (error)
    return fail(UpdateStage::prepare_staging,
                "cannot create " + staging.string() + ": " + error.message());

  const auto plan = core::plan_update(installed ? &installed->manifest : nullptr, target);
  std::vector<std::size_t> download = plan.download;
  UpdateReport report{.result = UpdateResult::installed, .build_id = id};
  if (installed) {
    const auto active = layout.build_directory(installed->id);
    for (const auto index : plan.reuse) {
      const auto &file = target.files[index];
      if (reuse_file(install_path(active, file.path), install_path(staging, file.path),
                     file))
        ++report.reused_files;
      else
        download.push_back(index);
    }
  }

  for (const auto index : download) {
    const auto &file = target.files[index];
    const auto bytes = source.fetch(file.sha256, static_cast<std::size_t>(file.size));
    if (!bytes)
      return fail(UpdateStage::fetch_file, file.path + " is unavailable");
    if (bytes->size() != file.size || sha256_of(*bytes) != file.sha256)
      return fail(UpdateStage::verify_file,
                  file.path + " does not match its manifest hash");
    if (!write_bytes(install_path(staging, file.path), *bytes))
      return fail(UpdateStage::write_file, "cannot write " + file.path);
    ++report.downloaded_files;
    report.downloaded_bytes += file.size;
  }
  if (!write_bytes(staging / std::string(installed_manifest_name), *manifest_bytes))
    return fail(UpdateStage::write_file, "cannot write the installed manifest");

  // A build directory with this id is either a leftover from an attempt that
  // stopped before switching `current`, or the damaged active build being
  // repaired; both are replaced.
  const auto destination = layout.build_directory(id);
  fs::remove_all(destination, error);
  fs::create_directories(layout.app_directory(), error);
  fs::rename(staging, destination, error);
  if (error)
    return fail(UpdateStage::activate, "cannot move the new build into place: " +
                                           error.message());
  // Repairing a damaged install keeps whatever rollback build was recorded.
  auto previous = installed ? std::optional(installed->id)
                            : read_pointer(layout.previous_file());
  if (previous == id) previous.reset();
  if (installed && !write_pointer(layout.previous_file(), installed->id))
    return fail(UpdateStage::activate, "cannot record the rollback build");
  if (!write_pointer(layout.current_file(), id))
    return fail(UpdateStage::activate, "cannot switch to the new build");
  prune(layout, id, previous);
  return report;
}

std::optional<UpdateError> rollback(const InstallLayout &layout) {
  const auto previous = read_pointer(layout.previous_file());
  if (!previous)
    return fail(UpdateStage::read_installed, "no previous build is kept");
  auto build = load_build(layout, *previous);
  if (auto *failure = std::get_if<UpdateError>(&build)) return std::move(*failure);
  const auto current = read_pointer(layout.current_file());
  if (current && !write_pointer(layout.previous_file(), *current))
    return fail(UpdateStage::activate, "cannot record the rollback build");
  if (!write_pointer(layout.current_file(), *previous))
    return fail(UpdateStage::activate, "cannot switch to the previous build");
  return std::nullopt;
}

std::optional<fs::path> active_file(const InstallLayout &layout,
                                    const std::string_view install_path_text) {
  if (!core::is_safe_update_path(install_path_text)) return std::nullopt;
  const auto id = read_pointer(layout.current_file());
  if (!id) return std::nullopt;
  auto path = install_path(layout.build_directory(*id), std::string(install_path_text));
  std::error_code error;
  if (!fs::is_regular_file(path, error)) return std::nullopt;
  return path;
}

} // namespace layerc::launcher
