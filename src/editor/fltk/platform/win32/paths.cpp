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

std::string_view folder_opener_name() noexcept { return "Explorer"; }

} // namespace layerc::editor
