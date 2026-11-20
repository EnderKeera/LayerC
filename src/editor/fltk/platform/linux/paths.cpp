#include "platform/services.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include <cstdlib>
#include <unistd.h>

namespace layerc::editor {

std::uint32_t current_process_id() noexcept {
  return static_cast<std::uint32_t>(::getpid());
}

std::string file_chooser_start_directory() {
  // FLTK's Linux choosers take the native path bytes.
  const char *home = std::getenv("HOME");
  return home != nullptr ? std::string(home) : std::string{};
}

std::string profile_directory_unavailable_reason() {
  return "No profiles folder: HOME is not an absolute path.";
}

std::filesystem::path editor_appearance_path() {
  const auto roots = platform::profile_store_roots();
  if (roots.config_profiles.empty()) return {};
  return roots.config_profiles.parent_path() / "editor-appearance.json";
}

bool editor_appearance_file_supported() noexcept { return true; }

std::string editor_appearance_file_unavailable_reason() { return {}; }

std::string_view folder_opener_name() noexcept { return "xdg-open"; }

} // namespace layerc::editor
