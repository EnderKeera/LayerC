#include "platform/product_text.hpp"
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

std::string_view editor_product_line() noexcept { return "Linux X11 editor"; }

const char *editor_options_text() noexcept {
  return "Exclude editor windows from screen capture\n"
         "  Unavailable on Linux X11: the X server offers no per-window capture\n"
         "  exclusion, so any X client with access to the display can read them.\n\n"
         "Isolated visual recognition\n"
         "  Unavailable on Linux: LayerD runs visual recognition in its own process\n"
         "  through the bounded X11 capture; there is no separate worker to isolate.\n"
         "  A profile's Windows isolated-runtime setting is preserved unchanged.\n\n"
         "Advanced settings\n"
         "  LayerE on Linux always shows every field that the Windows editor keeps\n"
         "  behind Show Advanced Settings (IDs, charges, retrigger, recovery).";
}

const char *visual_tab_note() noexcept {
  return "The Visual tab calibrates X11 recognition; Game Bar companion data are preserved.";
}

const char *placement_drag_note() noexcept {
  return "drag bars in the preview; LayerD moves the overlay through the live draft. "
         "Direct Drag is unavailable on X11 because overlays stay click-through.";
}

} // namespace layerc::editor
