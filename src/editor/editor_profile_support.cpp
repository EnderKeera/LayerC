#include "editor_window.hpp"

#include "layerc/platform/profile_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <shellapi.h>
#include <windows.h>
namespace layerc::editor {
bool start_daemon() {
  std::array<wchar_t, 32'768> module_path{};
  const DWORD length = GetModuleFileNameW(
      nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size())
    return false;
  const auto daemon =
      std::filesystem::path(std::wstring_view(module_path.data(), length))
          .parent_path() /
      L"layerd.exe";
  std::wstring command = L"\"" + daemon.wstring() + L"\"";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created =
      CreateProcessW(daemon.c_str(), command_line.data(), nullptr, nullptr,
                     FALSE, CREATE_NO_WINDOW, nullptr,
                     daemon.parent_path().c_str(), &startup, &process);
  if (process.hThread != nullptr)
    CloseHandle(process.hThread);
  if (process.hProcess != nullptr)
    CloseHandle(process.hProcess);
  return created != FALSE;
}

bool recycle_profile_bundle(const std::filesystem::path &path) {
  std::vector<std::filesystem::path> candidates{path};
  auto profile_backup = path;
  profile_backup += L".bak";
  candidates.push_back(std::move(profile_backup));
  const auto visual = layerc::platform::visual_sidecar_path(path);
  candidates.push_back(visual);
  auto visual_backup = visual;
  visual_backup += L".bak";
  candidates.push_back(std::move(visual_backup));
  auto visual_state = visual;
  visual_state += L".state";
  candidates.push_back(std::move(visual_state));
  const auto automation = layerc::platform::automation_sidecar_path(path);
  candidates.push_back(automation);
  auto automation_backup = automation;
  automation_backup += L".bak";
  candidates.push_back(std::move(automation_backup));
  const auto automation_state = layerc::platform::automation_state_path(path);
  candidates.push_back(automation_state);
  auto automation_state_backup = automation_state;
  automation_state_backup += L".bak";
  candidates.push_back(std::move(automation_state_backup));

  std::wstring source;
  std::error_code error;
  for (const auto &candidate : candidates) {
    if (candidate.empty() || !std::filesystem::exists(candidate, error)) {
      error.clear();
      continue;
    }
    source += candidate.wstring();
    source.push_back(L'\0');
  }
  if (source.empty())
    return false;
  source.push_back(L'\0');
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = source.data();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
}

std::string safe_identifier(const std::wstring_view text,
                            const std::string_view fallback) {
  std::string output;
  bool last_dash{};
  for (const wchar_t character : text) {
    const wchar_t lower = character >= L'A' && character <= L'Z'
                              ? character + (L'a' - L'A')
                              : character;
    if ((lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9')) {
      output.push_back(static_cast<char>(lower));
      last_dash = false;
    } else if (!output.empty() && !last_dash) {
      output.push_back('-');
      last_dash = true;
    }
  }
  while (!output.empty() && output.back() == '-')
    output.pop_back();
  return output.empty() ? std::string(fallback) : output;
}

layerc::core::GameProfile make_new_profile() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  GameProfile profile;
  profile.id = "new-profile";
  profile.name = "New Profile";
  profile.enabled = true;
  profile.cooldowns.push_back({.id = "ability",
                               .name = "Ability",
                               .duration = 5s,
                               .max_charges = 1,
                               .retrigger = RetriggerPolicy::ignore});
  profile.panels.push_back({.id = "main",
                            .name = "Main",
                            .style = PanelStyle::bar,
                            .bar = {},
                            .anchor_x = 0.5,
                            .anchor_y = 0.75,
                            .cooldown_ids = {"ability"}});
  profile.bindings.push_back(
      {.cooldown_id = "ability",
       .chord = InputChord{{{InputDevice::keyboard, 0x12, 0}}}});
  return profile;
}

std::wstring profile_list_label(const layerc::core::GameProfile &profile) {
  std::wstring label = to_wide(profile.name);
  if (!profile.game.executable_name.empty())
    label += L"  —  " + to_wide(profile.game.executable_name);
  if (!profile.enabled)
    label += L"  (disabled)";
  return label;
}

std::string unique_panel_id(const std::string_view base,
                            const std::vector<layerc::core::Panel> &existing,
                            const std::vector<layerc::core::Panel> *generated) {
  const auto is_used = [&](const std::string &candidate) {
    const auto contains = [&](const std::vector<layerc::core::Panel> &panels) {
      return std::any_of(panels.begin(), panels.end(),
                         [&](const layerc::core::Panel &panel) {
                           return panel.id == candidate;
                         });
    };
    return contains(existing) || (generated != nullptr && contains(*generated));
  };
  std::string candidate(base);
  for (std::size_t suffix = 2; is_used(candidate); ++suffix)
    candidate = std::string(base) + "-" + std::to_string(suffix);
  return candidate;
}

std::wstring profile_error_text(const layerc::core::ProfileError &error) {
  const auto path = to_wide(error.path);
  const auto message = to_wide(error.message);
  return path.empty() ? message : path + L": " + message;
}

} // namespace layerc::editor
