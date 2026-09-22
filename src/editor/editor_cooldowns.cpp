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

bool is_modifier_virtual_key(const WPARAM key) noexcept {
  switch (key) {
  case VK_SHIFT:
  case VK_LSHIFT:
  case VK_RSHIFT:
  case VK_CONTROL:
  case VK_LCONTROL:
  case VK_RCONTROL:
  case VK_MENU:
  case VK_LMENU:
  case VK_RMENU:
  case VK_LWIN:
  case VK_RWIN:
    return true;
  default:
    return false;
  }
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
  if (!cooldown_editor_.capturing_binding())
    return false;
  const bool key_down =
      message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
  const bool key_up =
      message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
  if (key_down || key_up) {
    if (key_down && message.wParam == VK_ESCAPE) {
      cancel_binding_capture(L"Binding capture cancelled.");
      return true;
    }

    const std::uint16_t code = scan_code_from_lparam(message.lParam);
    if ((code & 0x00FFU) == 0) {
      set_status(L"That key cannot be represented as a Windows scan code; try "
                 L"another key.");
      return true;
    }

    const bool modifier = is_modifier_virtual_key(message.wParam);
    if (key_down) {
      cooldown_editor_.record_binding_key(code);
      if (!modifier) {
        assign_binding_chord(cooldown_editor_.pending_binding_codes());
      } else {
        set_status(L"Modifier captured; press another key, or release to bind "
                   L"the modifier chord. Esc cancels.");
      }
    } else if (modifier) {
      if (cooldown_editor_.pending_binding_codes().empty())
        cooldown_editor_.record_binding_key(code);
      assign_binding_chord(cooldown_editor_.pending_binding_codes());
    }
    return true;
  }
  if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
       message.message == WM_MBUTTONDOWN) &&
      message.hwnd != GetDlgItem(shell_.window(), IDC_CAPTURE_BINDING)) {
    cancel_binding_capture();
  }
  return false;
}

void EditorWindow::refresh_profile_controls() {
  cooldown_editor_.finish_binding_capture();
  shell_.syncing() = true;
  const ProfileFieldContext context{shell_.window(), document_.profile()};
  write_profile_fields(context, ProfileFieldOwner::profile);
  set_owner_draw_checkbox(
      GetDlgItem(shell_.window(), IDC_APPLICATION_EXACT_PATH),
      !document_.profile().game.executable_path.empty());
  const auto application =
      !document_.profile().game.executable_path.empty()
          ? to_wide(document_.profile().game.executable_path)
          : !document_.profile().game.executable_name.empty()
                ? to_wide(document_.profile().game.executable_name)
                : L"Choose a running application.";
  set_text(shell_.application_path(), application);
  refresh_cooldown_list();
  refresh_bar_list();
  shell_.syncing() = false;
  InvalidateRect(panel_preview_.window(), nullptr, TRUE);
}

void EditorWindow::refresh_cooldown_list(const int selection) {
  SendMessageW(cooldown_editor_.list(), LB_RESETCONTENT, 0, 0);
  for (std::size_t index = 0; index < document_.profile().cooldowns.size(); ++index) {
    const auto label = cooldown_label(document_.profile().cooldowns[index]);
    const auto item = SendMessageW(cooldown_editor_.list(), LB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(label.c_str()));
    if (item != LB_ERR && item != LB_ERRSPACE)
      SendMessageW(cooldown_editor_.list(), LB_SETITEMDATA, static_cast<WPARAM>(item),
                   static_cast<LPARAM>(index));
  }
  if (!document_.profile().cooldowns.empty())
    SendMessageW(
        cooldown_editor_.list(), LB_SETCURSEL,
        static_cast<WPARAM>(std::clamp(
            selection, 0, static_cast<int>(document_.profile().cooldowns.size()) - 1)),
        0);
  refresh_binding_controls();
}

void EditorWindow::refresh_selected_cooldown_label() {
  const LRESULT row = SendMessageW(cooldown_editor_.list(), LB_GETCURSEL, 0, 0);
  if (row == LB_ERR)
    return;
  const LRESULT data =
      SendMessageW(cooldown_editor_.list(), LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 ||
      static_cast<std::size_t>(data) >= document_.profile().cooldowns.size())
    return;
  const auto label =
      cooldown_label(document_.profile().cooldowns[static_cast<std::size_t>(data)]);
  const bool was_syncing = shell_.syncing();
  shell_.syncing() = true;
  SendMessageW(cooldown_editor_.list(), LB_DELETESTRING, static_cast<WPARAM>(row), 0);
  const LRESULT inserted = SendMessageW(
      cooldown_editor_.list(), LB_INSERTSTRING, static_cast<WPARAM>(row),
      reinterpret_cast<LPARAM>(label.c_str()));
  if (inserted != LB_ERR && inserted != LB_ERRSPACE) {
    SendMessageW(cooldown_editor_.list(), LB_SETITEMDATA,
                 static_cast<WPARAM>(inserted), data);
    SendMessageW(cooldown_editor_.list(), LB_SETCURSEL,
                 static_cast<WPARAM>(inserted), 0);
  }
  shell_.syncing() = was_syncing;
}

layerc::core::CooldownDefinition *EditorWindow::selected_cooldown() {
  const LRESULT row = SendMessageW(cooldown_editor_.list(), LB_GETCURSEL, 0, 0);
  if (row == LB_ERR)
    return nullptr;
  const LRESULT value =
      SendMessageW(cooldown_editor_.list(), LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 ||
      static_cast<std::size_t>(value) >= document_.profile().cooldowns.size())
    return nullptr;
  return &document_.profile().cooldowns[static_cast<std::size_t>(value)];
}

void EditorWindow::refresh_binding_controls() {
  auto *cooldown = selected_cooldown();
  const bool has_selection = cooldown != nullptr;
  const bool was_syncing = shell_.syncing();
  shell_.syncing() = true;
  const HWND binding_control = GetDlgItem(shell_.window(), IDC_COOLDOWN_BINDING);
  const HWND capture = GetDlgItem(shell_.window(), IDC_CAPTURE_BINDING);
  for (const auto &binding : profile_field_bindings())
    if (binding.owner == ProfileFieldOwner::cooldown)
      EnableWindow(GetDlgItem(shell_.window(), binding.control_id), has_selection);
  for (const int id : {IDC_COOLDOWN_ID, IDC_COOLDOWN_GROUPS,
                       IDC_COOLDOWN_BINDING, IDC_CAPTURE_BINDING,
                       IDC_CAPTURE_HID})
    EnableWindow(GetDlgItem(shell_.window(), id), has_selection);
  set_text(capture,
           cooldown_editor_.capturing_binding() ? L"Press chord..."
                                                : L"Capture key");
  if (!has_selection) {
    const ProfileFieldContext context{shell_.window(), document_.profile()};
    write_profile_fields(context, ProfileFieldOwner::cooldown);
    for (const int id : {IDC_COOLDOWN_ID, IDC_COOLDOWN_GROUPS})
      set_text(GetDlgItem(shell_.window(), id), L"");
    set_text(binding_control, L"Select a cooldown");
    shell_.syncing() = was_syncing;
    refresh_visual_controls();
    return;
  }

  const ProfileFieldContext context{shell_.window(), document_.profile(), cooldown};
  write_profile_fields(context, ProfileFieldOwner::cooldown);
  set_text(GetDlgItem(shell_.window(), IDC_COOLDOWN_ID), to_wide(cooldown->id));
  std::wstring groups;
  for (const auto &group : document_.profile().cooldown_groups) {
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
  set_text(GetDlgItem(shell_.window(), IDC_COOLDOWN_GROUPS),
           groups.empty() ? L"None" : groups);
  shell_.syncing() = was_syncing;
  refresh_visual_controls();
  if (cooldown_editor_.capturing_binding()) {
    set_text(binding_control, L"Press a key or chord (Esc cancels)");
    return;
  }

  std::wstring bindings;
  for (const auto &candidate : document_.profile().bindings) {
    if (candidate.cooldown_id != cooldown->id) continue;
    if (!bindings.empty()) bindings += L" or ";
    std::wstring chord;
    for (const auto &token : candidate.chord.tokens) {
      std::wstring label;
  if (token.device == layerc::core::InputDevice::keyboard) {
    label = key_name_for_scan_code(token.code);
  } else if (token.device == layerc::core::InputDevice::mouse) {
    label = L"Mouse button " + std::to_wstring(token.code);
  } else if (token.device == layerc::core::InputDevice::generic_hid) {
    if (token.code != 0 && token.code <= document_.profile().hid_bindings.size()) {
      const auto &hid = document_.profile().hid_bindings[token.code - 1U];
      label = L"HID — " + to_wide(hid.id);
    } else {
      label = L"HID binding unavailable";
    }
  } else {
    label = L"Controller " + std::to_wstring(token.controller_index + 1U) +
            L", input " + std::to_wstring(token.code);
  }
      if (!chord.empty()) chord += L" + ";
      chord += label;
    }
    bindings += chord;
  }
  set_text(binding_control, bindings.empty() ? L"Unbound" : bindings);
}

void EditorWindow::begin_binding_capture() {
  if (selected_cooldown() == nullptr) {
    set_status(L"Select a cooldown before capturing a binding.");
    return;
  }
  cooldown_editor_.begin_binding_capture();
  SetFocus(GetDlgItem(shell_.window(), IDC_CAPTURE_BINDING));
  refresh_binding_controls();
  set_status(L"Press a key or chord for the selected cooldown; Esc cancels.");
}

void EditorWindow::cancel_binding_capture(const std::wstring_view status) {
  if (!cooldown_editor_.capturing_binding())
    return;
  cooldown_editor_.finish_binding_capture();
  refresh_binding_controls();
  if (!status.empty())
    set_status(status);
}

void EditorWindow::assign_binding_chord(
    const std::span<const std::uint16_t> scan_codes) {
  auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) {
    cancel_binding_capture(
        L"Binding capture cancelled because no cooldown is selected.");
    return;
  }
  if (scan_codes.empty()) {
    set_status(L"Press at least one key for the binding.");
    return;
  }

  layerc::core::InputChord chord;
  chord.tokens.reserve(scan_codes.size());
  for (const std::uint16_t code : scan_codes) {
    if ((code & 0x00FFU) == 0) {
      set_status(L"That key cannot be represented as a Windows scan code; try "
                 L"another key.");
      return;
    }
    chord.tokens.push_back(
        {layerc::core::InputDevice::keyboard, code, 0});
  }

  const auto cooldown_id = cooldown->id;
  std::erase_if(document_.profile().bindings,
                [&](const layerc::core::InputBinding &binding) {
                  return binding.cooldown_id == cooldown_id;
                });
  document_.profile().bindings.push_back(
      {.cooldown_id = cooldown_id, .chord = std::move(chord)});
  layerc::core::compact_hid_bindings(document_.profile());
  cooldown_editor_.finish_binding_capture();
  refresh_binding_controls();
  record_profile_change(ChangeKind::immediate);
  set_status(document_.path().empty()
                 ? L"Binding updated — Save once to create the profile."
                 : L"Binding updated and autosaving.");
}

void EditorWindow::add_cooldown() {
  sync_profile_from_controls();
  int number = 1;
  std::string id;
  do {
    id = "ability-" + std::to_string(number++);
  } while (std::any_of(document_.profile().cooldowns.begin(), document_.profile().cooldowns.end(),
                       [&](const layerc::core::CooldownDefinition &cooldown) {
                         return cooldown.id == id;
                       }));
  const int display_number = number - 1;
  document_.profile().cooldowns.push_back(
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
        document_.profile().bindings.begin(), document_.profile().bindings.end(),
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
  document_.profile().bindings.push_back(
      {.cooldown_id = id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::keyboard, scan_code, 0}}}});
  sync_bar_panels(document_.profile(), id);
  refresh_bar_list(document_.profile().panels.size() - 1U);
  refresh_cooldown_list(static_cast<int>(document_.profile().cooldowns.size()) - 1);
  record_profile_change(ChangeKind::immediate);
  set_status(L"Cooldown added with an unused keyboard binding; Capture key can "
             L"change it.");
}

void EditorWindow::remove_cooldown() {
  if (document_.profile().cooldowns.size() <= 1) {
    MessageBoxW(shell_.window(), L"A profile needs at least one cooldown.",
                L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  const LRESULT row = SendMessageW(cooldown_editor_.list(), LB_GETCURSEL, 0, 0);
  const LRESULT data = row == LB_ERR
                           ? LB_ERR
                           : SendMessageW(cooldown_editor_.list(), LB_GETITEMDATA,
                                          static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 ||
      static_cast<std::size_t>(data) >= document_.profile().cooldowns.size()) {
    MessageBoxW(shell_.window(), L"Select a cooldown to remove.", L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto &cooldown = document_.profile().cooldowns[static_cast<std::size_t>(data)];
  const auto prompt = L"Remove '" + to_wide(cooldown.name) + L"'?";
  if (MessageBoxW(shell_.window(), prompt.c_str(), L"Remove cooldown",
                  MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  const auto id = cooldown.id;
  if (document_.visual()) {
    erase_visual_assignment(*document_.visual(), id);
  }
  document_.profile().cooldowns.erase(document_.profile().cooldowns.begin() + data);
  std::erase_if(document_.profile().bindings,
                [&](const layerc::core::InputBinding &binding) {
                  return binding.cooldown_id == id;
                });
  layerc::core::compact_hid_bindings(document_.profile());
  sync_bar_panels(document_.profile());

  refresh_cooldown_list(static_cast<int>(data) - 1);
  refresh_bar_list();
  record_profile_change(ChangeKind::immediate);
  set_status(L"Cooldown removed.");
}

} // namespace layerc::editor
