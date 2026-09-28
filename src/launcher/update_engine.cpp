#include "layerc/launcher/update_engine.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"

#include <cerrno>
#include <cstddef>
#include <fstream>
#include <span>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
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

// A successful rename is the single activation boundary. The stage is flushed
// first; on POSIX, the parent is flushed after rename as well.
enum class PublishOutcome {
  failed,
  published_unconfirmed,
  published_interrupted,
  published_durable
};

[[nodiscard]] bool plain_file_or_absent(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return (!error && (status.type() == fs::file_type::not_found ||
                     status.type() == fs::file_type::regular)) ||
         error == std::errc::no_such_file_or_directory;
}

// The lock file lives beside the activation record, so separate install roots
// can update independently. Keep its OS handle for the whole transaction:
// staging, recovery reads, publication, and pruning all mutate shared state.
class InstallWriterLock {
public:
  explicit InstallWriterLock(const InstallLayout &layout) {
    std::error_code error;
    fs::create_directories(layout.root, error);
    if (error) return;
    const auto path = layout.root / ".activation.lock";
#ifdef _WIN32
    if (!plain_file_or_absent(path)) return;
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                         S_IRUSR | S_IWUSR);
    if (descriptor_ < 0) return;
    struct stat status {};
    if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() ||
        ::fchmod(descriptor_, S_IRUSR | S_IWUSR) != 0 ||
        ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
#endif
  }

  ~InstallWriterLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      ::close(descriptor_);
    }
#endif
  }
  InstallWriterLock(const InstallWriterLock &) = delete;
  InstallWriterLock &operator=(const InstallWriterLock &) = delete;

  [[nodiscard]] bool locked() const noexcept {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return descriptor_ >= 0;
#endif
  }

private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int descriptor_{-1};
#endif
};

[[nodiscard]] bool write_activation_stage(const fs::path &path,
                                          const std::string_view bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error || !plain_file_or_absent(path)) return false;
  // A previous crash may have left a complete or partial stage. Never follow
  // an existing symlink or truncate a stage owned by another writer.
  fs::remove(path, error);
  if (error) return false;
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                  nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written{};
  const auto ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                            &written, nullptr) != 0 &&
                  written == bytes.size() && FlushFileBuffers(file) != 0;
  CloseHandle(file);
  if (!ok) (void)DeleteFileW(path.c_str());
  return ok;
#else
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                           O_CLOEXEC | O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
  if (file < 0) return false;
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<std::size_t>(count);
  }
  const bool ok = offset == bytes.size() && ::fsync(file) == 0;
  (void)::close(file);
  if (!ok) (void)::unlink(path.c_str());
  return ok;
#endif
}

[[nodiscard]] PublishOutcome publish_activation_file(
    const fs::path &source, const fs::path &target) {
  if (!plain_file_or_absent(target)) return PublishOutcome::failed;
#ifdef _WIN32
  return MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0
             ? PublishOutcome::published_durable
             : PublishOutcome::failed;
#else
  if (::rename(source.c_str(), target.c_str()) != 0)
    return PublishOutcome::failed;
  const int directory = ::open(target.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) return PublishOutcome::published_unconfirmed;
  const bool durable = ::fsync(directory) == 0;
  (void)::close(directory);
  return durable ? PublishOutcome::published_durable
                 : PublishOutcome::published_unconfirmed;
#endif
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

struct ActivationState {
  std::string current;
  std::optional<std::string> previous;
};

// A replacement of an already-named build first parks the old directory in
// staging. If power is lost before the new directory reaches app/, the next
// read restores the parked directory before inspecting its manifest.
[[nodiscard]] bool recover_parked_build(const InstallLayout &layout,
                                         const std::string &id) {
  const auto destination = layout.build_directory(id);
  const auto parked = layout.staging_directory() / ".replaced" / id;
  std::error_code error;
  if (fs::exists(destination, error)) return true;
  if (error) return false;
  if (!fs::exists(parked, error)) return !error;
  if (error) return false;
  fs::rename(parked, destination, error);
  return !error;
}

[[nodiscard]] std::variant<std::optional<ActivationState>, UpdateError>
recover_activation_builds(const InstallLayout &layout, ActivationState state) {
  if (!recover_parked_build(layout, state.current) ||
      (state.previous && !recover_parked_build(layout, *state.previous)))
    return fail(UpdateStage::read_installed,
                "cannot recover an interrupted build replacement");
  return std::optional<ActivationState>{std::move(state)};
}

[[nodiscard]] std::variant<std::optional<ActivationState>, UpdateError>
read_activation(const InstallLayout &layout) {
  const auto path = layout.activation_file();
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory)
    return fail(UpdateStage::read_installed, "cannot inspect the activation record");
  if (status.type() != fs::file_type::not_found) {
    if (status.type() != fs::file_type::regular)
      return fail(UpdateStage::read_installed, "the activation record is damaged");
    const auto bytes = read_bounded(path, 512);
    if (!bytes)
      return fail(UpdateStage::read_installed, "the activation record is unreadable");
    const auto parsed = core::json::parse(*bytes);
    const auto *value = std::get_if<core::json::Value>(&parsed);
    if (value == nullptr || !value->is_object())
      return fail(UpdateStage::read_installed, "the activation record is damaged");
    const auto *version = value->find("schemaVersion");
    const auto *current = value->find("current");
    const auto *previous = value->find("previous");
    if (value->as_object().size() != 3 || version == nullptr ||
        !version->is_integer() || version->as_integer() != 1 ||
        current == nullptr || !current->is_string() ||
        previous == nullptr ||
        !(previous->is_null() || previous->is_string()) ||
        !core::is_safe_update_path(current->as_string()) ||
        current->as_string().find('/') != std::string::npos ||
        (previous->is_string() &&
         (!core::is_safe_update_path(previous->as_string()) ||
          previous->as_string().find('/') != std::string::npos ||
          previous->as_string() == current->as_string())))
      return fail(UpdateStage::read_installed, "the activation record is damaged");
    return recover_activation_builds(
        layout, ActivationState{current->as_string(),
                                previous->is_string()
                                    ? std::optional(previous->as_string())
                                    : std::nullopt});
  }
  // One-way migration: legacy pointers remain untouched until a new record is
  // durably published. A new record is authoritative even if they still exist.
  if (!fs::exists(layout.current_file(), error)) {
    if (error)
      return fail(UpdateStage::read_installed, "cannot inspect the legacy pointer");
    return std::optional<ActivationState>{};
  }
  const auto current = read_pointer(layout.current_file());
  if (!current)
    return fail(UpdateStage::read_installed, "the current build pointer is damaged");
  auto previous = read_pointer(layout.previous_file());
  if (previous == current) previous.reset();
  return recover_activation_builds(layout, ActivationState{*current, previous});
}

[[nodiscard]] PublishOutcome write_activation(const InstallLayout &layout,
                                               const ActivationState &state) {
  std::string bytes = "{\"schemaVersion\":1,\"current\":";
  core::json::append_quoted(bytes, state.current);
  bytes += ",\"previous\":";
  if (state.previous)
    core::json::append_quoted(bytes, *state.previous);
  else
    bytes += "null";
  bytes += "}\n";
  auto stage = layout.activation_file();
  stage += ".tmp";
  if (!write_activation_stage(stage, bytes)) return PublishOutcome::failed;
  if (layout.activation_checkpoint &&
      !layout.activation_checkpoint(ActivationCheckpoint::before_publish))
    return PublishOutcome::failed;
  const auto outcome = publish_activation_file(stage, layout.activation_file());
  if (outcome != PublishOutcome::failed && layout.activation_checkpoint &&
      !layout.activation_checkpoint(ActivationCheckpoint::after_publish))
    return PublishOutcome::published_interrupted;
  return outcome;
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

[[nodiscard]] std::optional<std::string>
first_damaged_file(const fs::path &directory, const UpdateManifest &manifest) {
  for (const auto &file : manifest.files)
    if (!file_matches(install_path(directory, file.path), file))
      return file.path;
  return std::nullopt;
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

// Checks the source and staged copy so a damaged local file is downloaded
// instead of counted as reused or carried forward.
[[nodiscard]] bool reuse_file(const fs::path &from, const fs::path &to,
                              const UpdateFile &file) {
  if (!file_matches(from, file)) return false;
  std::error_code error;
  fs::create_directories(to.parent_path(), error);
  if (error) return false;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
  return !error && file_matches(to, file);
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
  const InstallWriterLock lock(layout);
  if (!lock.locked())
    return fail(UpdateStage::read_installed, "the install lock is unavailable");
  auto read = read_activation(layout);
  if (auto *failure = std::get_if<UpdateError>(&read)) return std::move(*failure);
  const auto &state = std::get<std::optional<ActivationState>>(read);
  if (!state) return std::optional<InstalledBuild>{};
  auto build = load_build(layout, state->current);
  if (auto *failure = std::get_if<UpdateError>(&build)) return std::move(*failure);
  return std::optional<InstalledBuild>{std::move(std::get<InstalledBuild>(build))};
}

std::variant<UpdateReport, UpdateError>
apply_update(const InstallLayout &layout, const std::string_view channel,
             ReleaseSource &source, SignatureVerifier &verifier) {
  const InstallWriterLock lock(layout);
  if (!lock.locked())
    return fail(UpdateStage::read_installed, "the install lock is unavailable");
  auto read = read_activation(layout);
  auto *state = std::get_if<std::optional<ActivationState>>(&read);
  if (state == nullptr) {
    // A damaged legacy pointer can be replaced by a fresh install. A damaged
    // new record is never overwritten because it may be the only rollback
    // evidence.
    std::error_code error;
    if (fs::exists(layout.activation_file(), error) || error)
      return std::get<UpdateError>(std::move(read));
  }
  std::optional<InstalledBuild> installed;
  if (state != nullptr && *state) {
    auto build = load_build(layout, (*state)->current);
    if (auto *metadata = std::get_if<InstalledBuild>(&build))
      installed = std::move(*metadata);
  }

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
      if (core::serialize_update_manifest_json(installed->manifest) !=
          core::serialize_update_manifest_json(target))
        return fail(UpdateStage::check_sequence,
                    "the release changes an installed build's manifest");
      if (!first_damaged_file(layout.build_directory(installed->id),
                              installed->manifest))
        return UpdateReport{.result = UpdateResult::up_to_date,
                            .build_id = installed->id,
                            .reused_files = target.files.size()};
      // A damaged same-version install is repaired through staging.
      [[fallthrough]];
    case core::UpdateOrder::newer:
      break;
    }
  }

  std::error_code error;
  fs::remove_all(layout.staging_directory(), error);
  if (error)
    return fail(UpdateStage::prepare_staging,
                "cannot clear interrupted staging: " + error.message());
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
  if (const auto damaged = first_damaged_file(staging, target))
    return fail(UpdateStage::verify_file,
                "staged file " + *damaged + " does not match its manifest hash");

  // Park an existing destination before replacing it. In particular, a
  // same-ID repair must not delete the current directory before its verified
  // replacement is ready; read_activation restores a parked directory after
  // an interruption between the two renames.
  const auto destination = layout.build_directory(id);
  const auto parked = layout.staging_directory() / ".replaced" / id;
  fs::create_directories(layout.app_directory(), error);
  if (error)
    return fail(UpdateStage::activate, "cannot prepare the app directory: " +
                                           error.message());
  if (fs::exists(destination, error)) {
    fs::create_directories(parked.parent_path(), error);
    if (error)
      return fail(UpdateStage::activate, "cannot park the old build: " +
                                             error.message());
    fs::rename(destination, parked, error);
    if (error)
      return fail(UpdateStage::activate, "cannot park the old build: " +
                                             error.message());
  }
  fs::rename(staging, destination, error);
  if (error) {
    std::error_code restore_error;
    if (fs::exists(parked, restore_error))
      fs::rename(parked, destination, restore_error);
    return fail(UpdateStage::activate, "cannot move the new build into place: " +
                                           error.message());
  }
  // Repairing the same ID preserves the previous rollback target.
  auto previous = installed && installed->id != id
                      ? std::optional(installed->id)
                      : state != nullptr && *state
                            ? (*state)->previous
                            : read_pointer(layout.previous_file());
  if (previous == id) previous.reset();
  const auto publication = write_activation(layout, ActivationState{id, previous});
  if (publication == PublishOutcome::failed)
    return fail(UpdateStage::activate, "cannot publish the activation record");
  if (publication == PublishOutcome::published_unconfirmed)
    return fail(UpdateStage::activate,
                "activation published but its durability is unconfirmed");
  if (publication == PublishOutcome::published_interrupted)
    return fail(UpdateStage::activate,
                "activation published but the update was interrupted");
  prune(layout, id, previous);
  return report;
}

std::optional<UpdateError> rollback(const InstallLayout &layout) {
  const InstallWriterLock lock(layout);
  if (!lock.locked())
    return fail(UpdateStage::read_installed, "the install lock is unavailable");
  auto read = read_activation(layout);
  if (auto *failure = std::get_if<UpdateError>(&read)) return std::move(*failure);
  const auto &state = std::get<std::optional<ActivationState>>(read);
  if (!state || !state->previous)
    return fail(UpdateStage::read_installed, "no previous build is kept");
  auto build = load_build(layout, *state->previous);
  if (auto *failure = std::get_if<UpdateError>(&build)) return std::move(*failure);
  const auto &candidate = std::get<InstalledBuild>(build);
  if (const auto damaged = first_damaged_file(
          layout.build_directory(candidate.id), candidate.manifest))
    return fail(UpdateStage::read_installed,
                "rollback build " + candidate.id + " has a damaged file: " +
                    *damaged);
  const auto publication = write_activation(
      layout, ActivationState{*state->previous, state->current});
  if (publication == PublishOutcome::failed)
    return fail(UpdateStage::activate, "cannot publish the activation record");
  if (publication == PublishOutcome::published_unconfirmed)
    return fail(UpdateStage::activate,
                "activation published but its durability is unconfirmed");
  if (publication == PublishOutcome::published_interrupted)
    return fail(UpdateStage::activate,
                "activation published but rollback was interrupted");
  return std::nullopt;
}

std::optional<fs::path> active_file(const InstallLayout &layout,
                                    const std::string_view install_path_text) {
  if (!core::is_safe_update_path(install_path_text)) return std::nullopt;
  const InstallWriterLock lock(layout);
  if (!lock.locked()) return std::nullopt;
  auto read = read_activation(layout);
  const auto *state = std::get_if<std::optional<ActivationState>>(&read);
  if (state == nullptr || !*state) return std::nullopt;
  auto path = install_path(layout.build_directory((*state)->current),
                           std::string(install_path_text));
  std::error_code error;
  if (!fs::is_regular_file(path, error)) return std::nullopt;
  return path;
}

} // namespace layerc::launcher
