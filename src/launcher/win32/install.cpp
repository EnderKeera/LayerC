#include "install.hpp"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <system_error>
#include <vector>

namespace layerc::launcher::win32 {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::optional<fs::path> known_folder(const KNOWNFOLDERID &id) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) || raw == nullptr) {
    if (raw != nullptr) CoTaskMemFree(raw);
    return std::nullopt;
  }
  fs::path path(raw);
  CoTaskMemFree(raw);
  return path;
}

class RegistryKey {
public:
  RegistryKey() = default;
  ~RegistryKey() {
    if (key_ != nullptr) RegCloseKey(key_);
  }
  RegistryKey(const RegistryKey &) = delete;
  RegistryKey &operator=(const RegistryKey &) = delete;
  [[nodiscard]] PHKEY put() noexcept { return &key_; }
  [[nodiscard]] HKEY get() const noexcept { return key_; }

private:
  HKEY key_{};
};

[[nodiscard]] bool create_key(const std::wstring &path, RegistryKey &key) {
  return RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                         KEY_SET_VALUE, nullptr, key.put(), nullptr) == ERROR_SUCCESS;
}

[[nodiscard]] bool set_string(HKEY key, const wchar_t *name, const std::wstring &value) {
  return RegSetValueExW(key, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE *>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

[[nodiscard]] bool set_dword(HKEY key, const wchar_t *name, const DWORD value) {
  return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                        sizeof(value)) == ERROR_SUCCESS;
}

[[nodiscard]] std::wstring quoted(const fs::path &path) {
  return L"\"" + path.wstring() + L"\"";
}

} // namespace

std::optional<InstallPaths> default_install_paths() {
  const auto start_menu = known_folder(FOLDERID_Programs);
  const auto local = known_folder(FOLDERID_LocalAppData);
  if (!start_menu || !local) return std::nullopt;
  // FOLDERID_UserProgramFiles is %LOCALAPPDATA%\Programs by definition; fall
  // back to that path where the shell does not resolve it (older builds, Wine).
  auto programs = known_folder(FOLDERID_UserProgramFiles);
  if (!programs) programs = *local / L"Programs";
  InstallPaths paths;
  paths.root = *programs / L"LayerC";
  paths.launcher = paths.root / L"layerc.exe";
  paths.shortcut = *start_menu / L"LayerC.lnk";
  paths.data_root = *local / L"LayerC";
  return paths;
}

bool write_uninstall_entry(const RegistryKeys &keys, const InstallPaths &paths,
                           const std::wstring_view display_version,
                           const std::uint64_t size_bytes) {
  RegistryKey key;
  if (!create_key(keys.uninstall, key)) return false;
  const auto size_kb = static_cast<DWORD>(std::min<std::uint64_t>(size_bytes / 1024, MAXDWORD));
  return set_string(key.get(), L"DisplayName", L"LayerC") &&
         set_string(key.get(), L"DisplayVersion", std::wstring(display_version)) &&
         set_string(key.get(), L"Publisher", L"LayerC") &&
         set_string(key.get(), L"InstallLocation", paths.root.wstring()) &&
         set_string(key.get(), L"DisplayIcon", paths.launcher.wstring()) &&
         set_string(key.get(), L"UninstallString", quoted(paths.launcher) + L" --uninstall") &&
         set_string(key.get(), L"URLInfoAbout", L"https://github.com/EnderKeera/LayerC") &&
         set_dword(key.get(), L"EstimatedSize", size_kb) &&
         set_dword(key.get(), L"NoModify", 1) && set_dword(key.get(), L"NoRepair", 1);
}

bool remove_uninstall_entry(const RegistryKeys &keys) {
  const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, keys.uninstall.c_str());
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool set_autostart(const RegistryKeys &keys, const bool enabled, const fs::path &launcher) {
  RegistryKey key;
  if (!create_key(keys.run, key)) return false;
  if (enabled) return set_string(key.get(), keys.run_value.c_str(), quoted(launcher));
  const auto status = RegDeleteValueW(key.get(), keys.run_value.c_str());
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool autostart_enabled(const RegistryKeys &keys) {
  return read_string(keys.run, keys.run_value.c_str()).has_value();
}

bool updates_enabled(const RegistryKeys &keys) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  const auto status = RegGetValueW(HKEY_CURRENT_USER, keys.settings.c_str(), L"Updates",
                                   RRF_RT_REG_DWORD, nullptr, &value, &size);
  return status == ERROR_SUCCESS && value != 0;
}

bool create_shortcut(const fs::path &shortcut, const fs::path &target,
                     const std::wstring_view description) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(init);
  bool created = false;
  IShellLinkW *link = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&link))) &&
      link != nullptr) {
    const std::wstring text(description);
    IPersistFile *file = nullptr;
    if (SUCCEEDED(link->SetPath(target.c_str())) &&
        SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str())) &&
        SUCCEEDED(link->SetDescription(text.c_str())) &&
        SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file != nullptr) {
      std::error_code error;
      fs::create_directories(shortcut.parent_path(), error);
      created = !error && SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  if (uninitialize) CoUninitialize();
  return created;
}

std::optional<std::wstring> read_string(const std::wstring &key, const wchar_t *value) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr,
                   nullptr, &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t))
    return std::nullopt;
  std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
  size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr,
                   buffer.data(), &size) != ERROR_SUCCESS)
    return std::nullopt;
  return std::wstring(buffer.data());
}

std::uint64_t directory_size(const fs::path &root) {
  std::uint64_t total = 0;
  std::error_code error;
  for (fs::recursive_directory_iterator it(root, error), end; !error && it != end;
       it.increment(error)) {
    std::error_code size_error;
    if (it->is_regular_file(size_error)) {
      const auto size = it->file_size(size_error);
      if (!size_error) total += size;
    }
  }
  return total;
}

} // namespace layerc::launcher::win32
