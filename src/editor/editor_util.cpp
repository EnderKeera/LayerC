#include "editor_text.hpp"
#include "editor_util.hpp"

#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/window_query.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <charconv>
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

void set_text(const HWND control, const std::wstring_view text) {
  if (control != nullptr)
    SetWindowTextW(control, std::wstring(text).c_str());
}

std::optional<std::uint32_t> parse_bounded(const std::wstring_view text,
                                           const std::uint32_t minimum,
                                           const std::uint32_t maximum,
                                           const int base) {
  const auto parsed =
      layerc::platform::parse_integer<std::uint32_t>(text, base);
  if (!parsed || *parsed < minimum || *parsed > maximum)
    return std::nullopt;
  return parsed;
}

std::uint32_t parse_uint(const std::wstring_view text,
                         const std::uint32_t fallback) {
  return parse_bounded(text, 0, UINT32_MAX).value_or(fallback);
}

std::optional<layerc::core::Milliseconds>
parse_duration_ms(const std::wstring_view seconds) {
  if (seconds.empty())
    return std::nullopt;
  const auto source = layerc::platform::ascii_narrow(seconds);
  if (!source)
    return std::nullopt;
  double parsed{};
  const auto [end, code] = std::from_chars(
      source->data(), source->data() + source->size(), parsed);
  if (code != std::errc{} || end != source->data() + source->size() ||
      !std::isfinite(parsed) || parsed <= 0.0 ||
      parsed * 1000.0 >
          static_cast<double>(layerc::core::max_cooldown_duration.count()))
    return std::nullopt;
  const auto milliseconds = std::llround(parsed * 1000.0);
  if (milliseconds < 1 ||
      milliseconds > layerc::core::max_cooldown_duration.count())
    return std::nullopt;
  return layerc::core::Milliseconds(milliseconds);
}

ProfileCheckboxMetrics profile_checkbox_metrics(const UINT source_dpi) {
  const int dpi = source_dpi == 0 ? 96 : static_cast<int>(source_dpi);
  return {
      .image_size = std::max(1, MulDiv(20, dpi, 96)),
      .inset = std::max(1, MulDiv(2, dpi, 96)),
      .pen_width = std::max(1, MulDiv(2, dpi, 96)),
  };
}

int stored_text_scale() {
  const auto stored =
      layerc::platform::read_layerc_registry_dword(text_scale_setting);
  if (!stored.value)
    return 100;
  const auto percent = static_cast<int>(*stored.value);
  return std::ranges::find(text_scale_choices, percent) !=
                 text_scale_choices.end()
             ? percent
             : 100;
}

bool profile_checkbox_hit(const RECT &row, const POINT point,
                          const UINT dpi) {
  // The state-image cell at the row's left edge; the painted box sits inside
  // it. Anything to its right (the label) only selects the row.
  const auto metrics = profile_checkbox_metrics(dpi);
  return point.y >= row.top && point.y < row.bottom &&
         point.x >= row.left && point.x < row.left + metrics.image_size;
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

bool start_daemon() {
  const auto daemon = layerc::platform::sibling_image_path(L"layerd.exe");
  if (daemon.empty()) return false;
  std::wstring command_line =
      layerc::platform::quote_command_argument(daemon.wstring());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created =
      CreateProcessW(daemon.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, daemon.parent_path().c_str(),
                     &startup, &process);
  const layerc::platform::OwnedHandle thread(process.hThread);
  const layerc::platform::OwnedHandle launched(process.hProcess);
  return created != FALSE;
}

bool recycle_profile_bundle(const std::filesystem::path &path) {
  std::vector<std::filesystem::path> candidates{
      path, layerc::platform::recovery_backup_path(path)};
  for (const auto kind : {layerc::core::ProfileModuleKind::visual,
                          layerc::core::ProfileModuleKind::automation,
                          layerc::core::ProfileModuleKind::companion}) {
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
    label += L"  (off)";
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
