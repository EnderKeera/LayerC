#include "cooldown_editor.hpp"

#include "accessible_text.hpp"
#include "editor_profile_fields.hpp"
#include "editor_prompt.hpp"
#include "editor_resource.h"
#include "editor_text.hpp"
#include "editor_util.hpp"
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

// Key names only; the scan codes live in the binding's tooltip.
std::wstring key_name_for_scan_code(const std::uint16_t code) {
  const LONG key_data = static_cast<LONG>((code & 0x00FFU) << 16U) |
                        ((code & 0xE000U) != 0 ? (1L << 24) : 0L);
  std::array<wchar_t, 128> name{};
  return GetKeyNameTextW(key_data, name.data(), static_cast<int>(name.size())) >
                 0
             ? std::wstring(name.data())
             : L"Keyboard key";
}

std::wstring binding_token_label(const layerc::core::InputToken &token,
                                 const layerc::core::GameProfile &profile);

// "Shift + C or Mouse button 4" for every binding of `cooldown_id`.
std::wstring bindings_label(const layerc::core::GameProfile &profile,
                            const std::string_view cooldown_id) {
  std::wstring bindings;
  for (const auto &candidate : profile.bindings) {
    if (candidate.cooldown_id != cooldown_id)
      continue;
    if (!bindings.empty())
      bindings += L" or ";
    std::wstring chord;
    for (const auto &token : candidate.chord.tokens) {
      if (!chord.empty())
        chord += L" + ";
      chord += binding_token_label(token, profile);
    }
    bindings += chord;
  }
  return bindings;
}

// "Dash · 6.5 s · Shift + C"
std::wstring cooldown_label(const layerc::core::CooldownDefinition &cooldown,
                            const layerc::core::GameProfile &profile) {
  std::wstring label = to_wide(cooldown.name) + L"  ·  " +
                       duration_seconds_text(cooldown.duration) + L" s";
  if (const auto keys = bindings_label(profile, cooldown.id); !keys.empty())
    label += L"  ·  " + keys;
  return label;
}

std::wstring binding_token_label(const layerc::core::InputToken &token,
                                 const layerc::core::GameProfile &profile) {
  if (token.device == layerc::core::InputDevice::keyboard)
    return key_name_for_scan_code(token.code);
  if (token.device == layerc::core::InputDevice::mouse)
    return L"Mouse button " + std::to_wstring(token.code);
  if (token.device == layerc::core::InputDevice::generic_hid) {
    if (token.code == 0 || token.code > profile.hid_bindings.size())
      return L"HID binding unavailable";
    return L"HID — " + to_wide(profile.hid_bindings[token.code - 1U].id);
  }
  return L"Controller " + std::to_wstring(token.controller_index + 1U) +
         L", input " + std::to_wstring(token.code);
}

struct CooldownRow {
  LRESULT row{};
  std::size_t index{};
};

// The selected list row and the cooldown index stored in its item data, when
// that index still names a cooldown.
std::optional<CooldownRow> selected_cooldown_row(const HWND list,
                                                 const std::size_t count) {
  const LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR)
    return std::nullopt;
  const LRESULT data =
      SendMessageW(list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 || static_cast<std::size_t>(data) >= count)
    return std::nullopt;
  return CooldownRow{row, static_cast<std::size_t>(data)};
}

} // namespace

CooldownEditor::CooldownEditor(ProfileDocument &document, EditorShell &shell,
                               const DialogTheme &theme,
                               EditReporter report_edit, Host host) noexcept
    : document_(document), shell_(shell), theme_(theme),
      report_edit_(std::move(report_edit)), host_(std::move(host)) {}

void CooldownEditor::set_status(const std::wstring_view text) const {
  set_status_text(shell_.status, text);
}

bool CooldownEditor::handle_capture_message(const MSG &message) {
  if (!capture_.capturing_binding())
    return false;
  const bool key_down =
      message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
  const bool key_up =
      message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
  if (key_down || key_up) {
    if (key_down && message.wParam == VK_ESCAPE) {
      cancel_capture(L"Binding capture cancelled.");
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
      capture_.record_binding_key(code);
      if (!modifier) {
        assign_chord(capture_.pending_binding_codes());
      } else {
        set_status(L"Modifier captured; press another key, or release to bind "
                   L"the modifier chord. Esc cancels.");
      }
    } else if (modifier) {
      if (capture_.pending_binding_codes().empty())
        capture_.record_binding_key(code);
      assign_chord(capture_.pending_binding_codes());
    }
    return true;
  }
  if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
       message.message == WM_MBUTTONDOWN) &&
      message.hwnd != GetDlgItem(shell_.window, IDC_CAPTURE_BINDING)) {
    cancel_capture();
  }
  return false;
}

void CooldownEditor::refresh_list(const int selection) {
  SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  for (std::size_t index = 0; index < document_.profile().cooldowns.size(); ++index) {
    const auto label = cooldown_label(document_.profile().cooldowns[index],
                                      document_.profile());
    const auto item = SendMessageW(list_, LB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(label.c_str()));
    if (item != LB_ERR && item != LB_ERRSPACE)
      SendMessageW(list_, LB_SETITEMDATA, static_cast<WPARAM>(item),
                   static_cast<LPARAM>(index));
  }
  if (!document_.profile().cooldowns.empty())
    SendMessageW(
        list_, LB_SETCURSEL,
        static_cast<WPARAM>(std::clamp(
            selection, 0, static_cast<int>(document_.profile().cooldowns.size()) - 1)),
        0);
  refresh_controls();
}

std::optional<std::size_t> CooldownEditor::selected_index() const {
  const auto selected =
      selected_cooldown_row(list_, document_.profile().cooldowns.size());
  if (!selected) return std::nullopt;
  return selected->index;
}

void CooldownEditor::refresh_selected_label() {
  const auto selected = selected_cooldown_row(
      list_, document_.profile().cooldowns.size());
  if (!selected)
    return;
  const auto [row, index] = *selected;
  const auto label = cooldown_label(document_.profile().cooldowns[index],
                                    document_.profile());
  auto sync = shell_.sync_scope();
  SendMessageW(list_, LB_DELETESTRING, static_cast<WPARAM>(row), 0);
  const LRESULT inserted = SendMessageW(
      list_, LB_INSERTSTRING, static_cast<WPARAM>(row),
      reinterpret_cast<LPARAM>(label.c_str()));
  if (inserted != LB_ERR && inserted != LB_ERRSPACE) {
    SendMessageW(list_, LB_SETITEMDATA,
                 static_cast<WPARAM>(inserted), static_cast<LPARAM>(index));
    SendMessageW(list_, LB_SETCURSEL,
                 static_cast<WPARAM>(inserted), 0);
  }
}

const layerc::core::CooldownDefinition *CooldownEditor::selected() const {
  const auto selected = selected_cooldown_row(
      list_, document_.profile().cooldowns.size());
  if (!selected)
    return nullptr;
  return &document_.profile().cooldowns[selected->index];
}

void CooldownEditor::refresh_controls() {
  const auto *cooldown = selected();
  const bool has_selection = cooldown != nullptr;
  auto sync = shell_.sync_scope();
  const HWND binding_control = GetDlgItem(shell_.window, IDC_COOLDOWN_BINDING);
  const HWND capture = GetDlgItem(shell_.window, IDC_CAPTURE_BINDING);
  for (const auto &binding : profile_field_bindings())
    if (binding.owner == ProfileFieldOwner::cooldown)
      EnableWindow(GetDlgItem(shell_.window, binding.control_id), has_selection);
  for (const int id : {IDC_COOLDOWN_ID, IDC_COOLDOWN_GROUPS,
                       IDC_COOLDOWN_BINDING, IDC_CAPTURE_BINDING,
                       IDC_CAPTURE_HID})
    EnableWindow(GetDlgItem(shell_.window, id), has_selection);
  // A profile keeps at least one cooldown: removal is unavailable, not an
  // error to report afterwards, and the button says why.
  const bool last =
      has_selection && document_.profile().cooldowns.size() <= 1;
  EnableWindow(GetDlgItem(shell_.window, IDC_REMOVE_COOLDOWN),
               has_selection && !last);
  const std::wstring_view reason =
      last ? L"A profile needs at least one cooldown." : L"";
  (void)set_button_reason(shell_, IDC_REMOVE_COOLDOWN, reason);
  const HWND reason_line =
      GetDlgItem(shell_.window, IDC_COOLDOWN_REMOVE_REASON);
  set_text(reason_line, reason);
  ShowWindow(reason_line, reason.empty() ? SW_HIDE : SW_SHOWNA);
  host_.follow_bar();
  set_text(capture,
           capture_.capturing_binding() ? L"Press chord..."
                                                : L"Capture key");
  if (!has_selection) {
    const ProfileFieldView context{shell_.window, document_.profile()};
    write_profile_fields(context, ProfileFieldOwner::cooldown);
    for (const int id : {IDC_COOLDOWN_ID, IDC_COOLDOWN_GROUPS})
      set_text(GetDlgItem(shell_.window, id), L"");
    set_text(binding_control, L"Select a cooldown");
    host_.refresh_visual();
    return;
  }

  const ProfileFieldView context{shell_.window, document_.profile(), cooldown};
  write_profile_fields(context, ProfileFieldOwner::cooldown);
  set_text(GetDlgItem(shell_.window, IDC_COOLDOWN_ID), to_wide(cooldown->id));
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
  set_text(GetDlgItem(shell_.window, IDC_COOLDOWN_GROUPS),
           groups.empty() ? L"None" : groups);
  host_.refresh_visual();
  if (capture_.capturing_binding()) {
    set_text(binding_control, L"Press a key or chord (Esc cancels)");
    return;
  }

  const auto bindings = bindings_label(document_.profile(), cooldown->id);
  set_text(binding_control, bindings.empty() ? L"Unbound" : bindings);
}

std::wstring CooldownEditor::binding_codes_text() {
  const auto *cooldown = selected();
  if (cooldown == nullptr)
    return L"Select a cooldown";
  std::wstring text;
  for (const auto &binding : document_.profile().bindings) {
    if (binding.cooldown_id != cooldown->id)
      continue;
    for (const auto &token : binding.chord.tokens) {
      if (token.device != layerc::core::InputDevice::keyboard)
        continue;
      wchar_t line[64]{};
      swprintf_s(line, L"%s: scan code 0x%04X\n",
                 key_name_for_scan_code(token.code).c_str(), token.code);
      text += line;
    }
  }
  if (!text.empty())
    text.pop_back();
  return text.empty() ? L"Capture key records the key or chord." : text;
}

void CooldownEditor::begin_capture() {
  if (selected() == nullptr) {
    set_status(L"Select a cooldown before capturing a binding.");
    return;
  }
  capture_.begin_binding_capture();
  SetFocus(GetDlgItem(shell_.window, IDC_CAPTURE_BINDING));
  refresh_controls();
  set_status(L"Press a key or chord for the selected cooldown; Esc cancels.");
}

void CooldownEditor::cancel_capture(const std::wstring_view status) {
  if (!capture_.capturing_binding())
    return;
  capture_.finish_binding_capture();
  refresh_controls();
  if (!status.empty())
    set_status(status);
}

void CooldownEditor::assign_chord(
    const std::span<const std::uint16_t> scan_codes) {
  const auto *cooldown = selected();
  if (cooldown == nullptr) {
    cancel_capture(
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
  const auto outcome = document_.edit_profile(
      [&](layerc::core::GameProfile &candidate) {
        std::erase_if(candidate.bindings,
                      [&](const layerc::core::InputBinding &binding) {
                        return binding.cooldown_id == cooldown_id;
                      });
        candidate.bindings.push_back(
            {.cooldown_id = cooldown_id, .chord = chord});
        layerc::core::compact_hid_bindings(candidate);
      },
      DocumentChangeKind::immediate);
  if (!report_edit_(outcome, DocumentChangeKind::immediate)) return;
  capture_.finish_binding_capture();
  refresh_controls();
  set_status(document_.path().empty()
                 ? L"Binding updated — Save once to create the profile."
                 : L"Binding updated and autosaving.");
}

namespace {

// The existing add mutation, applied to a candidate profile; returns the new id.
std::string add_cooldown(layerc::core::GameProfile &candidate) {
  int number = 1;
  std::string id;
  do {
    id = "ability-" + std::to_string(number++);
  } while (std::any_of(candidate.cooldowns.begin(), candidate.cooldowns.end(),
                       [&](const layerc::core::CooldownDefinition &cooldown) {
                         return cooldown.id == id;
                       }));
  const int display_number = number - 1;
  candidate.cooldowns.push_back(
      {.id = id,
       .name = "Ability " + std::to_string(display_number),
       .duration = std::chrono::seconds(5),
       .max_charges = 1,
       .retrigger = layerc::core::RetriggerPolicy::ignore});

  constexpr std::array<std::uint16_t, 36> preferred_scan_codes{
      0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1E, 0x1F, 0x20, 0x21,
      0x22, 0x23, 0x24, 0x25, 0x26, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
      0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x3B, 0x3C};
  const auto scan_code_used = [&](const std::uint16_t code) {
    return std::any_of(
        candidate.bindings.begin(), candidate.bindings.end(),
        [&](const layerc::core::InputBinding &binding) {
          return std::any_of(
              binding.chord.tokens.begin(), binding.chord.tokens.end(),
              [&](const layerc::core::InputToken &token) {
                return token.device == layerc::core::InputDevice::keyboard &&
                       token.code == code;
              });
        });
  };
  std::uint16_t scan_code{};
  for (const auto code : preferred_scan_codes) {
    if (!scan_code_used(code)) {
      scan_code = code;
      break;
    }
  }
  if (scan_code == 0) {
    for (std::uint16_t code = 1; code < 0x80; ++code) {
      if (!scan_code_used(code)) {
        scan_code = code;
        break;
      }
    }
  }
  if (scan_code == 0)
    scan_code = 0x12;
  candidate.bindings.push_back(
      {.cooldown_id = id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::keyboard, scan_code, 0}}}});
  sync_bar_panels(candidate, id);
  return id;
}

} // namespace

void CooldownEditor::add() {
  host_.sync_fields();
  const auto outcome = document_.edit_profile(
      [](layerc::core::GameProfile &candidate) { (void)add_cooldown(candidate); },
      DocumentChangeKind::immediate);
  if (!report_edit_(outcome, DocumentChangeKind::immediate)) return;
  host_.refresh_bars(document_.profile().panels.size() - 1U);
  refresh_list(static_cast<int>(document_.profile().cooldowns.size()) - 1);
  set_status(L"Cooldown added with an unused keyboard binding; Capture key can "
             L"change it.");
}

void CooldownEditor::duplicate() {
  // Commit pending field text first, then snapshot the source: copying stale
  // pending values would duplicate the wrong content.
  host_.sync_fields();
  const auto *source = selected();
  if (source == nullptr)
    return;
  const auto original = *source;
  const auto outcome = document_.edit_profile(
      [&](layerc::core::GameProfile &candidate) {
        (void)add_cooldown(candidate);
        auto &added = candidate.cooldowns.back();
        added.name = original.name + " copy";
        added.duration = original.duration;
        added.max_charges = original.max_charges;
        added.retrigger = original.retrigger;
        added.recovery = original.recovery;
      },
      DocumentChangeKind::immediate);
  if (!report_edit_(outcome, DocumentChangeKind::immediate)) return;
  refresh_list(static_cast<int>(document_.profile().cooldowns.size()) - 1);
  set_status(L"Cooldown duplicated with an unused key; Capture key can "
             L"change it.");
}

void CooldownEditor::remove() {
  // The Remove button, menu items and Del are unavailable in these states.
  if (document_.profile().cooldowns.size() <= 1)
    return;
  const auto selected = selected_cooldown_row(
      list_, document_.profile().cooldowns.size());
  if (!selected)
    return;
  const std::size_t index = selected->index;
  const auto cooldown = document_.profile().cooldowns[index];
  const auto show_error = [&](const CooldownRemovalError &error) {
    const auto message = to_wide(error.message);
    themed_message_box(shell_.window, message.c_str(), L"Cannot remove cooldown",
                       MB_OK | MB_ICONWARNING);
    set_status(L"Cooldown retained — no removal changes applied.");
  };
  if (const auto error = document_.cooldown_removal_error(cooldown.id)) {
    show_error(*error);
    return;
  }
  const auto prompt = L"Remove '" + to_wide(cooldown.name) + L"'?";
  if (themed_message_box(shell_.window, prompt.c_str(), L"Remove cooldown",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  if (const auto error = document_.remove_cooldown(cooldown.id)) {
    show_error(*error);
    return;
  }

  refresh_list(static_cast<int>(index) - 1);
  host_.refresh_bars(SIZE_MAX);
  // The document op already recorded the step; report it so autosave/live run.
  (void)report_edit_({ModuleEditStatus::accepted, {}},
                     DocumentChangeKind::immediate);
  set_status(L"Cooldown removed.");
}

} // namespace layerc::editor
