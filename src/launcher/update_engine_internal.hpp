#pragma once

#include "layerc/launcher/update_engine.hpp"
#include "layerc/platform/durable_file.hpp"

namespace layerc::launcher {

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
  [[nodiscard]] virtual bool write_pointer(const std::filesystem::path &path,
                                           std::string_view id);
};

[[nodiscard]] std::variant<UpdateReport, UpdateError>
apply_update_impl(const InstallLayout &layout, std::string_view channel,
                  ReleaseSource &source, SignatureVerifier &verifier,
                  UpdateDurability &durability);

} // namespace layerc::launcher
