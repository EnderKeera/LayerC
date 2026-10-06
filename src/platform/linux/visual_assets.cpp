// Linux side of the platform visual-asset contract: profile-local template
// loading through the shared template PNG codec. The daemon visual service
// consumes only the verified TemplateImageResult; no parser or storage policy
// lives in the daemon.

#include "layerc/platform/visual_assets_linux.hpp"

#include "layerc/platform/profile_store.hpp"

namespace layerc::platform {

TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            const std::string_view sha256) {
  return verified_template_from_bytes(
      profile_visual_asset_path(profile_path, sha256), sha256);
}

} // namespace layerc::platform
