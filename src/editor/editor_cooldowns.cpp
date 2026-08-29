#include "editor_resource.h"
#include "editor_window.hpp"
#include "layerc/core/hid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
namespace layerc::editor {
namespace {

std::uint16_t scan_code_from_lparam(const LPARAM key_data) noexcept {
  const auto message_data = static_cast<std::uintptr_t>(key_data);
  std::uint16_t code =
      static_cast<std::uint16_t>((message_data >> 16U) & 0x00FFU);
  if ((message_data & (1ULL << 24U)) != 0)
    code = static_cast<std::uint16_t>(code | 0xE000U);
  return code;
}

std::wstring key_name_for_scan_code(const std::uint16_t code) {
  const LONG key_data = static_cast<LONG>((code & 0x00FFU) << 16U) |
                        ((code & 0xE000U) != 0 ? (1L << 24) : 0L);
  std::array<wchar_t, 128> name{};
  std::wstring label =
      GetKeyNameTextW(key_data, name.data(), static_cast<int>(name.size())) > 0
          ? std::wstring(name.data())
          : L"Keyboard key";
  wchar_t suffix[24]{};
  swprintf_s(suffix, L"  (scan 0x%04X)", code);
  label += suffix;
  return label;
}

std::wstring duration_seconds_text(const layerc::core::Milliseconds duration) {
  const auto milliseconds = duration.count();
  std::wstring result = std::to_wstring(milliseconds / 1000);
  const auto remainder = milliseconds % 1000;
  if (remainder != 0) {
    wchar_t fraction[5]{};
    swprintf_s(fraction, L".%03lld", static_cast<long long>(remainder));
    result += fraction;
    while (result.back() == L'0')
      result.pop_back();
  }
  return result;
}

std::wstring cooldown_label(const layerc::core::CooldownDefinition &cooldown) {
  return to_wide(cooldown.name) + L"  —  " +
         duration_seconds_text(cooldown.duration) + L" s";
}

void sync_bar_panels(
    layerc::core::GameProfile &profile,
    const std::optional<std::string_view> added = std::nullopt) {
  const bool independent =
      !profile.panels.empty() &&
      std::ranges::all_of(profile.panels, [](const layerc::core::Panel &panel) {
        return panel.style == layerc::core::PanelStyle::bar &&
               panel.cooldown_ids.size() == 1;
      });
  const auto exists = [&](const std::string_view id) {
    return std::ranges::any_of(profile.cooldowns, [&](const auto &cooldown) {
      return cooldown.id == id;
    });
  };
  for (auto &panel : profile.panels)
    std::erase_if(panel.cooldown_ids,
                  [&](const auto &id) { return !exists(id); });
  std::erase_if(profile.panels,
                [](const auto &panel) { return panel.cooldown_ids.empty(); });
  if (profile.cooldowns.empty())
    return;

  if (added && independent) {
    const auto cooldown = std::ranges::find(
        profile.cooldowns, *added, &layerc::core::CooldownDefinition::id);
    if (cooldown != profile.cooldowns.end()) {
      layerc::core::Panel panel;
      panel.id = unique_panel_id(cooldown->id + "-bar", profile.panels);
      panel.name = cooldown->name;
      panel.style = layerc::core::PanelStyle::bar;
      panel.anchor_x = 0.5;
      panel.anchor_y = std::clamp(
          0.2 + 0.10 * static_cast<double>(profile.cooldowns.size() - 1U), 0.0,
          1.0);
      panel.cooldown_ids = {cooldown->id};
      profile.panels.push_back(std::move(panel));
      return;
    }
  }

  if (profile.panels.empty()) {
    profile.panels.push_back({.id = "main",
                              .name = "Main",
                              .style = layerc::core::PanelStyle::bar,
                              .bar = {},
                              .cooldown_ids = {}});
  }
  const std::string_view required =
      added.value_or(profile.cooldowns.front().id);
  const bool assigned = std::ranges::any_of(
      profile.panels, [&](const layerc::core::Panel &panel) {
        return std::ranges::find(panel.cooldown_ids, required) !=
               panel.cooldown_ids.end();
      });
  if (!assigned)
    profile.panels.front().cooldown_ids.emplace_back(required);
}

} // namespace
bool EditorWindow::handle_binding_capture_message(const MSG &message) {
  if (!capturing_binding_)
    return false;
  if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
    if (message.wParam == VK_ESCAPE) {
      cancel_binding_capture(L"Binding capture cancelled.");
    } else {
      assign_binding_key(message.lParam);
    }
    return true;
  }
  if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
       message.message == WM_MBUTTONDOWN) &&
      message.hwnd != GetDlgItem(dialog_, IDC_CAPTURE_BINDING)) {
    cancel_binding_capture();
  }
  return false;
}

void EditorWindow::refresh_profile_controls() {
  capturing_binding_ = false;
  syncing_ = true;
  set_text(GetDlgItem(dialog_, IDC_PROFILE_NAME), to_wide(profile_.name));
  set_text(GetDlgItem(dialog_, IDC_PROFILE_ID), to_wide(profile_.id));
  set_owner_draw_checkbox(
      GetDlgItem(dialog_, IDC_APPLICATION_EXACT_PATH),
      !profile_.game.executable_path.empty());
  const auto application =
      !profile_.game.executable_path.empty()
          ? to_wide(profile_.game.executable_path)
          : !profile_.game.executable_name.empty()
                ? to_wide(profile_.game.executable_name)
                : L"Choose a running application.";
  set_text(application_path_, application);
  refresh_cooldown_list();
  refresh_bar_list();
  syncing_ = false;
  InvalidateRect(preview_, nullptr, TRUE);
}

void EditorWindow::refresh_cooldown_list(const int selection) {
  SendMessageW(cooldown_list_, LB_RESETCONTENT, 0, 0);
  for (std::size_t index = 0; index < profile_.cooldowns.size(); ++index) {
    const auto label = cooldown_label(profile_.cooldowns[index]);
    const auto item = SendMessageW(cooldown_list_, LB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(label.c_str()));
    if (item != LB_ERR && item != LB_ERRSPACE)
      SendMessageW(cooldown_list_, LB_SETITEMDATA, static_cast<WPARAM>(item),
                   static_cast<LPARAM>(index));
  }
  if (!profile_.cooldowns.empty())
    SendMessageW(
        cooldown_list_, LB_SETCURSEL,
        static_cast<WPARAM>(std::clamp(
            selection, 0, static_cast<int>(profile_.cooldowns.size()) - 1)),
        0);
  refresh_binding_controls();
}

void EditorWindow::refresh_selected_cooldown_label() {
  const LRESULT row = SendMessageW(cooldown_list_, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR)
    return;
  const LRESULT data =
      SendMessageW(cooldown_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 ||
      static_cast<std::size_t>(data) >= profile_.cooldowns.size())
    return;
  const auto label =
      cooldown_label(profile_.cooldowns[static_cast<std::size_t>(data)]);
  const bool was_syncing = syncing_;
  syncing_ = true;
  SendMessageW(cooldown_list_, LB_DELETESTRING, static_cast<WPARAM>(row), 0);
  const LRESULT inserted = SendMessageW(
      cooldown_list_, LB_INSERTSTRING, static_cast<WPARAM>(row),
      reinterpret_cast<LPARAM>(label.c_str()));
  if (inserted != LB_ERR && inserted != LB_ERRSPACE) {
    SendMessageW(cooldown_list_, LB_SETITEMDATA,
                 static_cast<WPARAM>(inserted), data);
    SendMessageW(cooldown_list_, LB_SETCURSEL,
                 static_cast<WPARAM>(inserted), 0);
  }
  syncing_ = was_syncing;
}

layerc::core::CooldownDefinition *EditorWindow::selected_cooldown() {
  const LRESULT row = SendMessageW(cooldown_list_, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR)
    return nullptr;
  const LRESULT value =
      SendMessageW(cooldown_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 ||
      static_cast<std::size_t>(value) >= profile_.cooldowns.size())
    return nullptr;
  return &profile_.cooldowns[static_cast<std::size_t>(value)];
}

void EditorWindow::refresh_binding_controls() {
  const auto *cooldown = selected_cooldown();
  const bool has_selection = cooldown != nullptr;
  const bool was_syncing = syncing_;
  syncing_ = true;
  const HWND binding = GetDlgItem(dialog_, IDC_COOLDOWN_BINDING);
  const HWND capture = GetDlgItem(dialog_, IDC_CAPTURE_BINDING);
  for (const int id : {IDC_COOLDOWN_NAME, IDC_COOLDOWN_DURATION,
                       IDC_COOLDOWN_ID, IDC_COOLDOWN_CHARGES,
                       IDC_COOLDOWN_RETRIGGER, IDC_COOLDOWN_RECOVERY,
                       IDC_COOLDOWN_GROUPS, IDC_COOLDOWN_BINDING,
                       IDC_CAPTURE_BINDING, IDC_CAPTURE_HID})
    EnableWindow(GetDlgItem(dialog_, id), has_selection);
  set_text(capture, capturing_binding_ ? L"Press key..." : L"Capture key");
  if (!has_selection) {
    for (const int id : {IDC_COOLDOWN_NAME, IDC_COOLDOWN_DURATION,
                         IDC_COOLDOWN_ID, IDC_COOLDOWN_CHARGES,
                         IDC_COOLDOWN_GROUPS})
      set_text(GetDlgItem(dialog_, id), L"");
    SendMessageW(GetDlgItem(dialog_, IDC_COOLDOWN_RETRIGGER), CB_SETCURSEL,
                 static_cast<WPARAM>(-1), 0);
    SendMessageW(GetDlgItem(dialog_, IDC_COOLDOWN_RECOVERY), CB_SETCURSEL,
                 static_cast<WPARAM>(-1), 0);
    set_text(binding, L"Select a cooldown");
    syncing_ = was_syncing;
    refresh_visual_controls();
    return;
  }

  set_text(GetDlgItem(dialog_, IDC_COOLDOWN_NAME), to_wide(cooldown->name));
  set_text(GetDlgItem(dialog_, IDC_COOLDOWN_DURATION),
           duration_seconds_text(cooldown->duration));
  set_text(GetDlgItem(dialog_, IDC_COOLDOWN_ID), to_wide(cooldown->id));
  set_text(GetDlgItem(dialog_, IDC_COOLDOWN_CHARGES),
           std::to_wstring(cooldown->max_charges));
  SendMessageW(GetDlgItem(dialog_, IDC_COOLDOWN_RETRIGGER), CB_SETCURSEL,
               static_cast<WPARAM>(cooldown->retrigger), 0);
  SendMessageW(GetDlgItem(dialog_, IDC_COOLDOWN_RECOVERY), CB_SETCURSEL,
               static_cast<WPARAM>(cooldown->recovery), 0);
  std::wstring groups;
  for (const auto &group : profile_.cooldown_groups) {
    if (std::ranges::find(group.cooldown_ids, cooldown->id) ==
        group.cooldown_ids.end())
      continue;
    if (!groups.empty())
      groups += L", ";
    groups += to_wide(group.name.empty() ? group.id : group.name);
    groups += group.mode == layerc::core::CooldownGroupMode::shared_pool
                  ? L" (shared pool)"
                  : L" (trigger propagation)";
  }
  set_text(GetDlgItem(dialog_, IDC_COOLDOWN_GROUPS),
           groups.empty() ? L"None" : groups);
  syncing_ = was_syncing;
  refresh_visual_controls();
  if (capturing_binding_) {
    set_text(binding, L"Press a key (Esc cancels)");
    return;
  }

  const layerc::core::InputBinding *only{};
  std::size_t count{};
  for (const auto &candidate : profile_.bindings) {
    if (candidate.cooldown_id != cooldown->id)
      continue;
    only = &candidate;
    ++count;
  }
  if (count != 1 || only == nullptr || only->chord.tokens.size() != 1) {
    set_text(binding,
             count == 0 ? L"Unbound" : L"Custom chord — capture replaces it");
    return;
  }

  const auto &token = only->chord.tokens.front();
  std::wstring label;
  if (token.device == layerc::core::InputDevice::keyboard) {
    label = key_name_for_scan_code(token.code);
  } else if (token.device == layerc::core::InputDevice::mouse) {
    label = L"Mouse button " + std::to_wstring(token.code);
  } else if (token.device == layerc::core::InputDevice::generic_hid) {
    if (token.code != 0 && token.code <= profile_.hid_bindings.size()) {
      const auto &hid = profile_.hid_bindings[token.code - 1U];
      label = L"HID — " + to_wide(hid.id);
    } else {
      label = L"HID binding unavailable";
    }
  } else {
    label = L"Controller " + std::to_wstring(token.controller_index + 1U) +
            L", input " + std::to_wstring(token.code);
  }
  set_text(binding, label);
}

void EditorWindow::begin_binding_capture() {
  if (selected_cooldown() == nullptr) {
    set_status(L"Select a cooldown before capturing a binding.");
    return;
  }
  capturing_binding_ = true;
  SetFocus(GetDlgItem(dialog_, IDC_CAPTURE_BINDING));
  refresh_binding_controls();
  set_status(L"Press a key for the selected cooldown; Esc cancels.");
}

void EditorWindow::cancel_binding_capture(const std::wstring_view status) {
  if (!capturing_binding_)
    return;
  capturing_binding_ = false;
  refresh_binding_controls();
  if (!status.empty())
    set_status(status);
}

void EditorWindow::assign_binding_key(const LPARAM key_data) {
  auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) {
    cancel_binding_capture(
        L"Binding capture cancelled because no cooldown is selected.");
    return;
  }
  const std::uint16_t code = scan_code_from_lparam(key_data);

  if ((code & 0x00FFU) == 0) {
    set_status(L"That key cannot be represented as a Windows scan code; try "
               L"another key.");
    return;
  }

  const auto cooldown_id = cooldown->id;
  std::erase_if(profile_.bindings,
                [&](const layerc::core::InputBinding &binding) {
                  return binding.cooldown_id == cooldown_id;
                });
  profile_.bindings.push_back(
      {.cooldown_id = cooldown_id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::keyboard, code, 0}}}});
  layerc::core::compact_hid_bindings(profile_);
  capturing_binding_ = false;
  refresh_binding_controls();
  record_profile_change(ChangeKind::immediate);
  set_status(current_path_.empty()
                 ? L"Binding updated — Save once to create the profile."
                 : L"Binding updated and autosaving.");
}

void EditorWindow::add_cooldown() {
  sync_profile_from_controls();
  int number = 1;
  std::string id;
  do {
    id = "ability-" + std::to_string(number++);
  } while (std::any_of(profile_.cooldowns.begin(), profile_.cooldowns.end(),
                       [&](const layerc::core::CooldownDefinition &cooldown) {
                         return cooldown.id == id;
                       }));
  const int display_number = number - 1;
  profile_.cooldowns.push_back(
      {.id = id,
       .name = "Ability " + std::to_string(display_number),
       .duration = std::chrono::seconds(5),
       .max_charges = 1,
       .retrigger = layerc::core::RetriggerPolicy::ignore});

  constexpr std::array<std::uint16_t, 36> preferred_scan_codes{
      0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1E, 0x1F, 0x20, 0x21,
      0x22, 0x23, 0x24, 0x25, 0x26, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
      0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x3B, 0x3C};
  const auto scan_code_used = [&](const std::uint16_t candidate) {
    return std::any_of(
        profile_.bindings.begin(), profile_.bindings.end(),
        [&](const layerc::core::InputBinding &binding) {
          return std::any_of(
              binding.chord.tokens.begin(), binding.chord.tokens.end(),
              [&](const layerc::core::InputToken &token) {
                return token.device == layerc::core::InputDevice::keyboard &&
                       token.code == candidate;
              });
        });
  };
  std::uint16_t scan_code{};
  for (const auto candidate : preferred_scan_codes) {
    if (!scan_code_used(candidate)) {
      scan_code = candidate;
      break;
    }
  }
  if (scan_code == 0) {
    for (std::uint16_t candidate = 1; candidate < 0x80; ++candidate) {
      if (!scan_code_used(candidate)) {
        scan_code = candidate;
        break;
      }
    }
  }
  if (scan_code == 0)
    scan_code = 0x12;
  profile_.bindings.push_back(
      {.cooldown_id = id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::keyboard, scan_code, 0}}}});
  sync_bar_panels(profile_, id);
  refresh_bar_list(profile_.panels.size() - 1U);
  refresh_cooldown_list(static_cast<int>(profile_.cooldowns.size()) - 1);
  record_profile_change(ChangeKind::immediate);
  set_status(L"Cooldown added with an unused keyboard binding; Capture key can "
             L"change it.");
}

void EditorWindow::remove_cooldown() {
  if (profile_.cooldowns.size() <= 1) {
    MessageBoxW(dialog_, L"A profile needs at least one cooldown.",
                L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  const LRESULT row = SendMessageW(cooldown_list_, LB_GETCURSEL, 0, 0);
  const LRESULT data = row == LB_ERR
                           ? LB_ERR
                           : SendMessageW(cooldown_list_, LB_GETITEMDATA,
                                          static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 ||
      static_cast<std::size_t>(data) >= profile_.cooldowns.size()) {
    MessageBoxW(dialog_, L"Select a cooldown to remove.", L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto &cooldown = profile_.cooldowns[static_cast<std::size_t>(data)];
  const auto prompt = L"Remove '" + to_wide(cooldown.name) + L"'?";
  if (MessageBoxW(dialog_, prompt.c_str(), L"Remove cooldown",
                  MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  const auto id = cooldown.id;
  if (visual_profile_) {
    erase_visual_assignment(*visual_profile_, id);
  }
  profile_.cooldowns.erase(profile_.cooldowns.begin() + data);
  std::erase_if(profile_.bindings,
                [&](const layerc::core::InputBinding &binding) {
                  return binding.cooldown_id == id;
                });
  layerc::core::compact_hid_bindings(profile_);
  sync_bar_panels(profile_);

  refresh_cooldown_list(static_cast<int>(data) - 1);
  refresh_bar_list();
  record_profile_change(ChangeKind::immediate);
  set_status(L"Cooldown removed.");
}

} // namespace layerc::editor
