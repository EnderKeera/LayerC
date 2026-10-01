#pragma once

#include "layerc/launcher/update_engine.hpp"
#include "layerc/platform/durable_file.hpp"

namespace layerc::launcher {

// A successful activation-record rename is the single activation boundary.
// The stage is flushed first; on POSIX, the parent is flushed after rename.
// Interrupted means a layout checkpoint stopped the update after publication.
enum class ActivationOutcome {
  failed,
  published_unconfirmed,
  published_interrupted,
  published_durable
};

// Internal filesystem seam. Production uses these platform-backed defaults;
// tests override individual barriers to model I/O failure or process loss.
// No process-global fault switches or production test flags are involved.
class UpdateDurability {
public:
  virtual ~UpdateDurability() = default;
  [[nodiscard]] virtual bool flush_file(const std::filesystem::path &path);
  [[nodiscard]] virtual bool flush_directory(const std::filesystem::path &path);
  [[nodiscard]] virtual platform::DurablePublishOutcome publish_directory(
      const std::filesystem::path &from, const std::filesystem::path &to);
  // Stages the complete activation record and atomically replaces
  // activation.json. Called only after the named build is durable.
  [[nodiscard]] virtual ActivationOutcome
  publish_activation(const InstallLayout &layout, std::string_view record);
};

[[nodiscard]] std::variant<UpdateReport, UpdateError>
apply_update_impl(const InstallLayout &layout, std::string_view channel,
                  ReleaseSource &source, SignatureVerifier &verifier,
                  UpdateDurability &durability);

} // namespace layerc::launcher
