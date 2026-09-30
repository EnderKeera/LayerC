#pragma once

#include "layerc/core/update_manifest.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::launcher {

// Per-user install tree owned by the launcher:
//   <root>/app/<build-id>/   one complete build, including .layerc-manifest.json
//   <root>/current           one line naming the active build id
//   <root>/previous          one line naming the rollback build id, if kept
//   <root>/staging/          scratch for an update in progress
// A build becomes active only after every payload file, its installed manifest,
// and its directory publication pass the platform durability barriers, then
// `current` is durably replaced. Unconfirmed publication is not pruned. During
// same-ID repair, <build-id>.old preserves the old directory until activation;
// readers recover it if the replacement directory has not reached app/.
struct InstallLayout {
  std::filesystem::path root;

  [[nodiscard]] std::filesystem::path app_directory() const { return root / "app"; }
  [[nodiscard]] std::filesystem::path staging_directory() const {
    return root / "staging";
  }
  [[nodiscard]] std::filesystem::path current_file() const { return root / "current"; }
  [[nodiscard]] std::filesystem::path previous_file() const {
    return root / "previous";
  }
  [[nodiscard]] std::filesystem::path build_directory(std::string_view id) const {
    return app_directory() / std::filesystem::path(std::string(id));
  }
};

// The manifest a build was installed from; its name starts with '.', which
// is_safe_update_path rejects, so no published file can overwrite it.
inline constexpr std::string_view installed_manifest_name = ".layerc-manifest.json";

// Where published bytes come from. The WinHTTP source reads the GitHub
// release; tests and offline installs use a folder. Implementations must
// stop reading once a response exceeds max_bytes and return nullopt.
class ReleaseSource {
public:
  virtual ~ReleaseSource() = default;
  [[nodiscard]] virtual std::optional<std::string>
  fetch(std::string_view asset_name, std::size_t max_bytes) = 0;
};

// Verifies the detached manifest signature against the key compiled into the
// launcher. Called before the manifest is parsed.
class SignatureVerifier {
public:
  virtual ~SignatureVerifier() = default;
  [[nodiscard]] virtual bool verify(std::string_view manifest,
                                    std::string_view signature) = 0;
};

inline constexpr std::string_view manifest_asset_name = "manifest.json";
inline constexpr std::string_view signature_asset_name = "manifest.sig";
inline constexpr std::size_t max_signature_bytes = 1024;

// File assets are content-addressed: each is published under its lowercase
// SHA-256, so names never collide, identical files are stored once, and the
// manifest alone decides where a file is written.

// "<sequence>-<first 12 commit hex>": unique per publish and readable in
// Apps & Features and logs.
[[nodiscard]] std::string build_id_for(const core::UpdateManifest &manifest);

struct InstalledBuild {
  std::string id;
  core::UpdateManifest manifest;
};

enum class UpdateStage {
  read_installed,
  fetch_manifest,
  verify_signature,
  parse_manifest,
  check_channel,
  check_sequence,
  prepare_staging,
  fetch_file,
  verify_file,
  write_file,
  activate,
};

[[nodiscard]] std::string_view stage_name(UpdateStage stage) noexcept;

struct UpdateError {
  UpdateStage stage{};
  std::string message;
};

enum class UpdateResult { installed, up_to_date };

struct UpdateReport {
  UpdateResult result{};
  std::string build_id;
  std::size_t downloaded_files{};
  std::size_t reused_files{};
  std::uint64_t downloaded_bytes{};
};

// Reads the active build from `current`; nullopt when nothing is installed.
// A present but unreadable or inconsistent install is an error so the caller
// can report it instead of silently reinstalling over it.
[[nodiscard]] std::variant<std::optional<InstalledBuild>, UpdateError>
read_installed_build(const InstallLayout &layout);

// Brings the install to the newest published build of `channel`. Never
// changes the active build unless every file of the new build is present,
// verified and durably flushed with its directory. A damaged install is treated
// as no install, so the launcher repairs it with a full download instead of
// refusing to start. A lower sequence than the installed one is refused as a
// rollback attack; the same sequence is up to date without downloading.
[[nodiscard]] std::variant<UpdateReport, UpdateError>
apply_update(const InstallLayout &layout, std::string_view channel,
             ReleaseSource &source, SignatureVerifier &verifier);

// Makes the kept previous build active again. Fails without changes when no
// previous build is recorded or its directory is missing.
[[nodiscard]] std::optional<UpdateError> rollback(const InstallLayout &layout);

// Absolute path of a file inside the active build (e.g. "layerd.exe").
[[nodiscard]] std::optional<std::filesystem::path>
active_file(const InstallLayout &layout, std::string_view install_path);

} // namespace layerc::launcher
