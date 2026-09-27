// layerc.exe: the per-user installer and launch-time updater.
//
//   layerc.exe                 update if possible, start layerd, exit
//   layerc.exe --offline       start the installed build without the network
//   layerc.exe --install       (re)register this copy under %LOCALAPPDATA%\Programs
//   layerc.exe --uninstall     remove the program, optionally the profiles
//   layerc.exe --rollback      switch to the kept previous build and start it
//   layerc.exe --autostart=on|off
//
// The launcher is the only LayerC program that uses the network. It checks
// once per launch, starts layerd, and exits, so no update code stays resident.

#include "install.hpp"
#include "release_client.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/launcher/release_key.hpp"
#include "layerc/launcher/update_engine.hpp"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace launcher = layerc::launcher;
namespace win32 = layerc::launcher::win32;

constexpr wchar_t daemon_mutex[] = L"Local\\LayerC.LayerD.SingleInstance";
constexpr wchar_t launcher_mutex[] = L"Local\\LayerC.Launcher.SingleInstance";
constexpr wchar_t title[] = L"LayerC";

struct Options {
  bool offline{};
  bool install{};
  bool uninstall{};
  bool rollback{};
  std::optional<bool> autostart;
  // Internal: a temporary copy deletes the install folder after the
  // uninstalling launcher (process id) has exited.
  std::optional<DWORD> finish_uninstall_after;
};

std::optional<Options> parse_options() {
  int count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (arguments == nullptr) return std::nullopt;
  Options options;
  bool valid = true;
  for (int index = 1; index < count && valid; ++index) {
    const std::wstring_view argument = arguments[index];
    if (argument == L"--offline") options.offline = true;
    else if (argument == L"--install") options.install = true;
    else if (argument == L"--uninstall") options.uninstall = true;
    else if (argument == L"--rollback") options.rollback = true;
    else if (argument == L"--autostart=on") options.autostart = true;
    else if (argument == L"--autostart=off") options.autostart = false;
    else if (argument == L"--finish-uninstall" && index + 1 < count)
      options.finish_uninstall_after =
          static_cast<DWORD>(std::wcstoul(arguments[++index], nullptr, 10));
    else valid = false;
  }
  LocalFree(arguments);
  return valid ? std::optional(options) : std::nullopt;
}

std::wstring widen(const std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), size);
  return wide;
}

fs::path module_path() {
  std::vector<wchar_t> buffer(32'768);
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return fs::path(std::wstring_view(buffer.data(), length));
}

bool same_file(const fs::path &left, const fs::path &right) {
  std::error_code error;
  return fs::equivalent(left, right, error);
}

// One line per launch in %LOCALAPPDATA%\LayerC\logs\launcher.log; the time is
// wall-clock only because it is shown to people.
void log_line(const win32::InstallPaths &paths, const std::string &message) {
  std::error_code error;
  const auto directory = paths.data_root / L"logs";
  fs::create_directories(directory, error);
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char stamp[32]{};
  std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  std::ofstream(directory / L"launcher.log", std::ios::app) << stamp << message << "\n";
}

void show_error(const std::wstring &message) {
  MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONERROR);
}

bool daemon_running() {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, daemon_mutex);
  if (mutex == nullptr) return false;
  CloseHandle(mutex);
  return true;
}

bool start_process(const fs::path &image, const std::wstring &arguments = {}) {
  std::wstring command = L"\"" + image.wstring() + L"\"";
  if (!arguments.empty()) command += L" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const auto directory = image.parent_path().wstring();
  if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, directory.c_str(), &startup, &process))
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

std::optional<std::string> file_sha256(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  return layerc::core::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// Replaces the installed launcher with the copy shipped in the active build.
// A running image cannot be overwritten but can be renamed, so the old one
// becomes layerc.exe.old and is deleted on the next launch.
void self_update(const launcher::InstallLayout &layout, const win32::InstallPaths &paths) {
  const auto shipped = launcher::active_file(layout, "layerc.exe");
  if (!shipped) return;
  const auto shipped_hash = file_sha256(*shipped);
  if (!shipped_hash || shipped_hash == file_sha256(paths.launcher)) return;
  auto old = paths.launcher;
  old += L".old";
  std::error_code error;
  fs::remove(old, error);
  fs::rename(paths.launcher, old, error);
  if (error) {
    log_line(paths, "launcher self-update skipped: " + error.message());
    return;
  }
  fs::copy_file(*shipped, paths.launcher, fs::copy_options::overwrite_existing, error);
  if (error) {
    fs::rename(old, paths.launcher, error);
    log_line(paths, "launcher self-update failed; kept the running launcher");
    return;
  }
  log_line(paths, "launcher updated from the active build");
}

bool register_program(const win32::RegistryKeys &keys, const win32::InstallPaths &paths,
                      const std::wstring &version) {
  std::error_code error;
  return win32::write_uninstall_entry(keys, paths, version,
                                      win32::directory_size(paths.root)) &&
         (fs::exists(paths.shortcut, error) ||
          win32::create_shortcut(paths.shortcut, paths.launcher,
                                 L"Start the LayerC cooldown overlay"));
}

// Copies this executable into the install folder when it runs from anywhere
// else (a download) and nothing is installed yet, so the Start menu entry
// points at a stable launcher. An existing install is never overwritten from
// outside: it updates itself from signed builds instead.
bool install_self(const win32::InstallPaths &paths) {
  const auto self = module_path();
  if (self.empty()) return false;
  std::error_code error;
  fs::create_directories(paths.root, error);
  if (error) return false;
  fs::copy_file(self, paths.launcher, fs::copy_options::none, error);
  return !error;
}

std::string update(const launcher::InstallLayout &layout) {
  const auto key = launcher::parse_p256_public_key_hex(launcher::release_public_key_hex);
  if (!key) return "updates unavailable: this launcher has no release key built in";
  win32::GitHubReleaseSource source(std::wstring(launcher::release_repository),
                                    std::wstring(launcher::release_tag), {});
  win32::BcryptSignatureVerifier verifier(*key);
  const auto result =
      launcher::apply_update(layout, launcher::release_channel, source, verifier);
  if (const auto *failure = std::get_if<launcher::UpdateError>(&result)) {
    auto message = "update skipped at " + std::string(launcher::stage_name(failure->stage)) +
                   ": " + failure->message;
    if (!source.last_error().empty()) message += " (" + source.last_error() + ")";
    return message;
  }
  const auto &report = std::get<launcher::UpdateReport>(result);
  if (report.result == launcher::UpdateResult::up_to_date)
    return "up to date at " + report.build_id;
  return "installed " + report.build_id + ": " + std::to_string(report.downloaded_files) +
         " downloaded (" + std::to_string(report.downloaded_bytes) + " bytes), " +
         std::to_string(report.reused_files) + " reused";
}

int launch(const Options &options, const win32::RegistryKeys &keys,
           const win32::InstallPaths &paths) {
  const launcher::InstallLayout layout{paths.root};
  std::string outcome;
  if (daemon_running()) {
    // The running daemon uses the active build, so it is never switched.
    outcome = options.rollback ? "LayerC is running; exit it before rolling back"
                               : "LayerC is already running; update check skipped";
  } else if (options.rollback) {
    const auto failure = launcher::rollback(layout);
    outcome = failure ? "rollback failed: " + failure->message : "rolled back";
  } else if (options.offline || !win32::updates_enabled(keys)) {
    outcome = "update check turned off";
  } else {
    outcome = update(layout);
  }
  log_line(paths, outcome);

  if (same_file(module_path(), paths.launcher)) self_update(layout, paths);
  const auto installed = launcher::read_installed_build(layout);
  if (const auto *build = std::get_if<std::optional<launcher::InstalledBuild>>(&installed);
      build != nullptr && build->has_value())
    (void)register_program(keys, paths, widen((*build)->id));

  const auto daemon = launcher::active_file(layout, "layerd.exe");
  if (!daemon) {
    show_error(L"LayerC is not installed yet and could not be downloaded.\n\n" +
               widen(outcome));
    return 1;
  }
  if (!start_process(*daemon)) {
    show_error(L"LayerC could not be started from " + daemon->wstring());
    return 1;
  }
  return 0;
}

int uninstall(const win32::RegistryKeys &keys, const win32::InstallPaths &paths) {
  if (daemon_running()) {
    show_error(L"Exit LayerC from its tray icon, then uninstall again.");
    return 1;
  }
  if (MessageBoxW(nullptr, L"Remove LayerC from this computer?", title,
                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    return 1;
  const bool purge =
      MessageBoxW(nullptr,
                  (L"Also delete your profiles and settings?\n\n" +
                   paths.data_root.wstring() + L"\n\nChoose No to keep them for a later install.")
                      .c_str(),
                  title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;

  std::error_code error;
  fs::remove(paths.shortcut, error);
  (void)win32::set_autostart(keys, false, paths.launcher);
  (void)win32::remove_uninstall_entry(keys);
  if (purge) {
    fs::remove_all(paths.data_root, error);
    RegDeleteTreeW(HKEY_CURRENT_USER, keys.settings.c_str());
  }

  // This executable may live inside the folder being removed, so a temporary
  // copy waits for it to exit and then deletes the folder.
  const auto self = module_path();
  const auto relative = self.lexically_normal().lexically_relative(paths.root.lexically_normal());
  const bool inside = !relative.empty() && *relative.begin() != L"..";
  if (!inside) {
    fs::remove_all(paths.root, error);
    return 0;
  }
  wchar_t temp[MAX_PATH + 1]{};
  if (GetTempPathW(MAX_PATH, temp) == 0) return 1;
  const auto helper = fs::path(temp) / (L"layerc-uninstall-" +
                                        std::to_wstring(GetCurrentProcessId()) + L".exe");
  fs::copy_file(self, helper, fs::copy_options::overwrite_existing, error);
  if (error || !start_process(helper, L"--finish-uninstall " +
                                          std::to_wstring(GetCurrentProcessId()))) {
    show_error(L"LayerC was unregistered, but its folder could not be removed:\n" +
               paths.root.wstring());
    return 1;
  }
  return 0;
}

// Runs from %TEMP%: waits for the uninstalling launcher, then removes only the
// fixed per-user install folder (never a path taken from the command line).
int finish_uninstall(const DWORD parent, const win32::InstallPaths &paths) {
  if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent)) {
    WaitForSingleObject(process, 30'000);
    CloseHandle(process);
  }
  std::error_code error;
  for (int attempt = 0; attempt < 10; ++attempt) {
    fs::remove_all(paths.root, error);
    if (!error && !fs::exists(paths.root)) return 0;
    Sleep(500);
  }
  return 1;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
  const auto options = parse_options();
  if (!options) {
    show_error(L"Usage: layerc.exe [--offline] [--install] [--uninstall] [--rollback] "
               L"[--autostart=on|off]");
    return 64;
  }
  const auto paths = win32::default_install_paths();
  if (!paths) {
    show_error(L"The per-user program folders could not be found.");
    return 1;
  }
  if (options->finish_uninstall_after) return finish_uninstall(*options->finish_uninstall_after, *paths);

  // Two launches at once (a double click) would race on the install tree.
  const HANDLE guard = CreateMutexW(nullptr, TRUE, launcher_mutex);
  if (guard == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (guard != nullptr) CloseHandle(guard);
    return 0;
  }

  const win32::RegistryKeys keys;
  int status = 0;
  auto old = paths->launcher;
  old += L".old";
  std::error_code error;
  fs::remove(old, error);

  if (options->uninstall) {
    status = uninstall(keys, *paths);
  } else if (options->autostart) {
    status = win32::set_autostart(keys, *options->autostart, paths->launcher) ? 0 : 1;
  } else {
    const bool from_elsewhere = !same_file(module_path(), paths->launcher);
    std::error_code exists_error;
    if (from_elsewhere && fs::exists(paths->launcher, exists_error)) {
      // An older download must not replace the installed launcher; hand the
      // launch to the installed copy, which updates itself.
      std::wstring arguments;
      if (options->offline) arguments += L"--offline ";
      if (options->rollback) arguments += L"--rollback ";
      ReleaseMutex(guard);
      CloseHandle(guard);
      return start_process(paths->launcher, arguments) ? 0 : 1;
    }
    if (from_elsewhere && !install_self(*paths)) {
      show_error(L"LayerC could not be copied to " + paths->root.wstring());
      status = 1;
    } else {
      status = launch(*options, keys, *paths);
    }
  }
  ReleaseMutex(guard);
  CloseHandle(guard);
  return status;
}
