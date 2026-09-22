#include "editor_text.hpp"
#include "editor_util.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace layerc::editor {

std::wstring to_wide(const std::string_view text) {
  const auto converted = layerc::platform::utf8_to_wide(text);
  // External diagnostics remain visible even if a provider returns bad UTF-8.
  return !text.empty() && converted.empty() ? L"?" : converted;
}

std::string to_utf8(const std::wstring_view text) {
  return layerc::platform::wide_to_utf8(text);
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0)
    return {};
  std::wstring output(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(control, output.data(), static_cast<int>(output.size()));
  output.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return output;
}

void set_text(const HWND control, const std::wstring_view text) {
  if (control != nullptr)
    SetWindowTextW(control, std::wstring(text).c_str());
}

std::uint32_t parse_uint(const std::wstring_view text,
                         const std::uint32_t fallback) {
  const std::wstring value(text);
  wchar_t *end{};
  const auto parsed = wcstoul(value.c_str(), &end, 10);
  return end != nullptr && end != value.c_str() && *end == L'\0' &&
                 parsed <= UINT32_MAX
             ? static_cast<std::uint32_t>(parsed)
             : fallback;
}

std::optional<layerc::core::Milliseconds>
parse_duration_ms(const std::wstring_view seconds) {
  if (seconds.empty())
    return std::nullopt;
  const std::wstring value(seconds);
  wchar_t *end{};
  const double parsed = wcstod(value.c_str(), &end);
  if (end == value.c_str() || end == nullptr || *end != L'\0' ||
      !std::isfinite(parsed) || parsed <= 0.0 || parsed > 86'400.0)
    return std::nullopt;
  const auto milliseconds = std::llround(parsed * 1000.0);
  if (milliseconds < 1 || milliseconds > 86'400'000)
    return std::nullopt;
  return layerc::core::Milliseconds(milliseconds);
}

ProfileCheckboxMetrics profile_checkbox_metrics(const UINT source_dpi) {
  const int dpi = source_dpi == 0 ? 96 : static_cast<int>(source_dpi);
  return {
      .image_size = std::max(1, MulDiv(16, dpi, 96)),
      .inset = std::max(1, MulDiv(2, dpi, 96)),
      .pen_width = std::max(1, MulDiv(2, dpi, 96)),
  };
}

void set_owner_draw_checkbox(const HWND control, const bool checked) {
  if (control == nullptr)
    return;
  SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  InvalidateRect(control, nullptr, TRUE);
}

bool owner_draw_checkbox_checked(const HWND control) {
  return control != nullptr &&
         SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

COLORREF color_ref(const std::uint32_t color) {
  return layerc::platform::native_color_ref(color);
}

std::uint32_t rgb_value(const COLORREF color) {
  return layerc::platform::native_rgb_value(color);
}

bool start_daemon() {
  std::vector<wchar_t> module_path(32'768);
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
      CreateProcessW(daemon.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, daemon.parent_path().c_str(),
                     &startup, &process);
  if (process.hThread != nullptr)
    CloseHandle(process.hThread);
  if (process.hProcess != nullptr)
    CloseHandle(process.hProcess);
  return created != FALSE;
}

bool recycle_profile_bundle(const std::filesystem::path &path) {
  std::vector<std::filesystem::path> candidates{
      path, layerc::platform::recovery_backup_path(path)};
  for (const auto kind : {layerc::platform::ModuleSidecarKind::visual,
                          layerc::platform::ModuleSidecarKind::automation,
                          layerc::platform::ModuleSidecarKind::companion}) {
    const auto sidecar = layerc::platform::sidecar_paths(path, kind);
    candidates.push_back(sidecar.primary);
    candidates.push_back(sidecar.backup);
    candidates.push_back(sidecar.marker);
  }
  const auto automation_state = layerc::platform::automation_state_path(path);
  candidates.push_back(automation_state);
  candidates.push_back(layerc::platform::recovery_backup_path(automation_state));

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
    if ((lower >= L'a' && lower <= L'z') ||
        (lower >= L'0' && lower <= L'9')) {
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

std::string unique_panel_id(
    const std::string_view base,
    const std::vector<layerc::core::Panel> &existing,
    const std::vector<layerc::core::Panel> *generated) {
  return next_unique_id(base, [&](const std::string_view candidate) {
    const auto contains = [&](const std::vector<layerc::core::Panel> &panels) {
      return std::ranges::any_of(panels, [&](const layerc::core::Panel &panel) {
        return panel.id == candidate;
      });
    };
    return contains(existing) ||
           (generated != nullptr && contains(*generated));
  });
}

std::wstring profile_error_text(const layerc::core::ProfileError &error) {
  const auto path = to_wide(error.path);
  const auto message = to_wide(error.message);
  return path.empty() ? message : path + L": " + message;
}

} // namespace layerc::editor
