#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::launcher::win32 {

// Per-user install locations. Nothing here needs administrator rights.
struct InstallPaths {
  std::filesystem::path root;       // %LOCALAPPDATA%\Programs\LayerC
  std::filesystem::path launcher;   // root\layerc.exe
  std::filesystem::path shortcut;   // Start menu\Programs\LayerC.lnk
  std::filesystem::path data_root;  // %LOCALAPPDATA%\LayerC (profiles, logs)
};

[[nodiscard]] std::optional<InstallPaths> default_install_paths();

// Registry locations under HKEY_CURRENT_USER. Tests pass their own
// disposable subkeys so they never touch the real entries.
struct RegistryKeys {
  std::wstring uninstall{L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LayerC"};
  std::wstring run{L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"};
  std::wstring run_value{L"LayerC"};
  std::wstring settings{L"Software\\LayerC"};
};

// Writes the Apps & Features entry; its uninstall command runs the launcher.
[[nodiscard]] bool write_uninstall_entry(const RegistryKeys &keys,
                                         const InstallPaths &paths,
                                         std::wstring_view display_version,
                                         std::uint64_t size_bytes);
[[nodiscard]] bool remove_uninstall_entry(const RegistryKeys &keys);

[[nodiscard]] bool set_autostart(const RegistryKeys &keys, bool enabled,
                                 const std::filesystem::path &launcher);
[[nodiscard]] bool autostart_enabled(const RegistryKeys &keys);

// Updates stay on unless HKCU\<settings>\Updates is a DWORD 0.
[[nodiscard]] bool updates_enabled(const RegistryKeys &keys);

[[nodiscard]] bool create_shortcut(const std::filesystem::path &shortcut,
                                   const std::filesystem::path &target,
                                   std::wstring_view description);

// Reads a REG_SZ value; used by tests and to show the installed version.
[[nodiscard]] std::optional<std::wstring> read_string(const std::wstring &key,
                                                      const wchar_t *value);

// Total bytes of regular files under `root`, for EstimatedSize.
[[nodiscard]] std::uint64_t directory_size(const std::filesystem::path &root);

} // namespace layerc::launcher::win32
