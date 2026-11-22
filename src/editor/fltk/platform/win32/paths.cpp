#include "platform/product_text.hpp"
#include "platform/services.hpp"

#include "layerc/core/utf8.hpp"
#include "layerc/platform/profile_store.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace layerc::editor {

std::uint32_t current_process_id() noexcept {
  return static_cast<std::uint32_t>(GetCurrentProcessId());
}

std::string file_chooser_start_directory() {
  PWSTR native_path{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr,
                                 &native_path))) {
    return {};
  }
  const std::wstring path(native_path);
  CoTaskMemFree(native_path);
  const auto utf8 = core::try_wide_to_utf8(path);
  return utf8.value_or(std::string{});
}

std::string profile_directory_unavailable_reason() {
  return "The Windows profile directory is unavailable.";
}

std::filesystem::path editor_appearance_path() { return {}; }

bool editor_appearance_file_supported() noexcept { return false; }

std::string editor_appearance_file_unavailable_reason() {
  return "Appearance settings are unavailable in this editor.";
}

std::string_view folder_opener_name() noexcept { return "Explorer"; }

std::string_view editor_product_line() noexcept { return "Windows editor"; }

const char *editor_options_text() noexcept {
  return "Exclude editor windows from screen capture\n"
         "  This setting is unavailable in this editor.\n\n"
         "Isolated visual recognition\n"
         "  LayerD manages visual recognition. The profile's isolated worker setting is preserved.\n\n"
         "Advanced settings\n"
         "  All fields are shown, including IDs, charges, retrigger and recovery.";
}

const char *visual_tab_note() noexcept {
  return "The Visual tab calibrates visual recognition. Game Bar companion data are preserved.";
}

const char *placement_drag_note() noexcept {
  return "drag bars in the preview. Save to apply the placement.";
}

} // namespace layerc::editor
