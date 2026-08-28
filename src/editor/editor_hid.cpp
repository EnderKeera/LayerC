#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/core/hid.hpp"
#include "layerc/platform/hid_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace layerc::editor {
namespace {

struct HidDialogState {
  EditorWindow *editor{};
  std::vector<layerc::platform::HidRawDevice> devices;
  std::vector<std::size_t> visible_devices;
  std::set<std::pair<USHORT, USHORT>> registrations;
  std::map<std::pair<HANDLE, std::uint8_t>, std::vector<std::byte>> baselines;
  std::optional<layerc::platform::InferredHidControl> inferred;
  std::wstring captured_device_path;
  bool listening{};
};

bool system_class(const layerc::platform::HidRawDevice &device) {
  return device.info.usUsagePage == 0x01U &&
         (device.info.usUsage == 0x02U || device.info.usUsage == 0x06U);
}

void unregister_devices(HidDialogState &state) {
  for (const auto [page, usage] : state.registrations) {
    RAWINPUTDEVICE device{.usUsagePage = page,
                          .usUsage = usage,
                          .dwFlags = RIDEV_REMOVE,
                          .hwndTarget = nullptr};
    (void)RegisterRawInputDevices(&device, 1, sizeof(device));
  }
  state.registrations.clear();
}

void register_devices(const HWND window, HidDialogState &state) {
  unregister_devices(state);
  for (const auto index : state.visible_devices) {
    const auto &device = state.devices[index];
    const auto usage = std::pair(device.info.usUsagePage, device.info.usUsage);
    if (!state.registrations.insert(usage).second)
      continue;
    RAWINPUTDEVICE registration{.usUsagePage = usage.first,
                                .usUsage = usage.second,
                                .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                                .hwndTarget = window};
    if (RegisterRawInputDevices(&registration, 1, sizeof(registration)) ==
        FALSE)
      state.registrations.erase(usage);
  }
}

void populate_devices(const HWND window, HidDialogState &state) {
  const HWND combo = GetDlgItem(window, IDC_HID_DEVICE);
  const int previous =
      static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  const HANDLE previous_handle =
      previous >= 0 &&
              static_cast<std::size_t>(previous) < state.visible_devices.size()
          ? state
                .devices[state.visible_devices[static_cast<std::size_t>(
                    previous)]]
                .handle
          : nullptr;
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  state.visible_devices.clear();
  const bool advanced =
      owner_draw_checkbox_checked(GetDlgItem(window, IDC_HID_SYSTEM_CLASS));
  int selected{};
  for (std::size_t index = 0; index < state.devices.size(); ++index) {
    if (system_class(state.devices[index]) && !advanced)
      continue;
    auto label = state.devices[index].product_name;
    if (system_class(state.devices[index]))
      label += L"  [system class]";
    const auto row = SendMessageW(combo, CB_ADDSTRING, 0,
                                  reinterpret_cast<LPARAM>(label.c_str()));
    if (row == CB_ERR || row == CB_ERRSPACE)
      continue;
    state.visible_devices.push_back(index);
    if (state.devices[index].handle == previous_handle)
      selected = static_cast<int>(state.visible_devices.size() - 1U);
  }
  if (!state.visible_devices.empty())
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
  EnableWindow(GetDlgItem(window, IDC_HID_CAPTURE),
               !state.visible_devices.empty());
  if (state.visible_devices.empty())
    set_text(GetDlgItem(window, IDC_HID_SUMMARY),
             advanced ? L"No generic HID devices are currently available."
                      : L"No non-system HID devices are available. Advanced "
                        L"can expose a device-specific keyboard or mouse.");
}

const layerc::platform::HidRawDevice *
selected_device(const HWND window, const HidDialogState &state) {
  const auto row =
      SendMessageW(GetDlgItem(window, IDC_HID_DEVICE), CB_GETCURSEL, 0, 0);
  if (row == CB_ERR || row < 0 ||
      static_cast<std::size_t>(row) >= state.visible_devices.size())
    return nullptr;
  return &state.devices[state.visible_devices[static_cast<std::size_t>(row)]];
}

bool read_percent(const HWND control, const unsigned maximum, double &value) {
  const auto source = control_text(control);
  if (source.empty())
    return false;
  wchar_t *end{};
  const unsigned long parsed = wcstoul(source.c_str(), &end, 10);
  if (end == source.c_str() || end == nullptr || *end != L'\0' ||
      parsed > maximum)
    return false;
  value = static_cast<double>(parsed) / 100.0;
  return true;
}

} // namespace

void EditorWindow::capture_hid_binding() {
  auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) {
    set_status(L"Select a cooldown before capturing a HID binding.");
    return;
  }
  HidDialogState state{};
  state.editor = this;
  state.devices = layerc::platform::enumerate_raw_hid_devices();
  if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_HID_BINDING), dialog_,
                      &EditorWindow::hid_binding_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK ||
      !state.inferred)
    return;

  auto captured = std::move(state.inferred->binding);
  const auto cooldown_id = cooldown->id;
  std::size_t binding_index = SIZE_MAX;
  for (const auto &binding : profile_.bindings) {
    if (binding.cooldown_id != cooldown_id || binding.chord.tokens.size() != 1)
      continue;
    const auto &token = binding.chord.tokens.front();
    if (token.device == layerc::core::InputDevice::generic_hid &&
        token.code != 0 && token.code <= profile_.hid_bindings.size())
      binding_index = token.code - 1U;
  }
  const auto id_base =
      "hid-" + safe_identifier(to_wide(cooldown_id), "control");
  std::string id = id_base;
  int suffix = 2;
  while (std::ranges::any_of(profile_.hid_bindings, [&](const auto &candidate) {
    const auto index =
        static_cast<std::size_t>(&candidate - profile_.hid_bindings.data());
    return candidate.id == id && index != binding_index;
  }))
    id = id_base + "-" + std::to_string(suffix++);
  captured.id = std::move(id);
  if (binding_index == SIZE_MAX) {
    if (profile_.hid_bindings.size() >= 128) {
      MessageBoxW(dialog_,
                  L"This profile already has the maximum 128 HID bindings.",
                  L"HID binding", MB_OK | MB_ICONERROR);
      return;
    }
    binding_index = profile_.hid_bindings.size();
    profile_.hid_bindings.push_back(std::move(captured));
  } else {
    profile_.hid_bindings[binding_index] = std::move(captured);
  }
  std::erase_if(profile_.bindings, [&](const auto &binding) {
    return binding.cooldown_id == cooldown_id;
  });
  profile_.bindings.push_back(
      {.cooldown_id = cooldown_id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::generic_hid,
             static_cast<std::uint16_t>(binding_index + 1U), 0}}}});
  layerc::core::compact_hid_bindings(profile_);
  refresh_binding_controls();
  record_profile_change(ChangeKind::immediate);
  set_status(current_path_.empty()
                 ? L"HID binding captured — Save once to create the profile."
                 : L"HID binding captured and autosaving.");
}

INT_PTR CALLBACK EditorWindow::hid_binding_dialog_proc(const HWND window,
                                                       const UINT message,
                                                       const WPARAM wparam,
                                                       const LPARAM lparam) {
  auto *state = bind_dialog_state<HidDialogState>(window, message, lparam);
  if (state == nullptr)
    return FALSE;
  if (message == WM_INITDIALOG) {
    set_dark_title_bar(window);
    (void)layerc::platform::apply_capture_exclusion(
        window, state->editor->capture_exclusion_);
    state->editor->style_dialog_children(window);
    set_owner_draw_checkbox(GetDlgItem(window, IDC_HID_SYSTEM_CLASS), false);
    set_text(GetDlgItem(window, IDC_HID_THRESHOLD), L"50");
    set_text(GetDlgItem(window, IDC_HID_HYSTERESIS), L"10");
    set_text(GetDlgItem(window, IDC_HID_SLOT), L"1");
    EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
    populate_devices(window, *state);
    register_devices(window, *state);
    return TRUE;
  }
  if (message == WM_INPUT_DEVICE_CHANGE) {
    state->devices = layerc::platform::enumerate_raw_hid_devices();
    state->baselines.clear();
    if (!state->captured_device_path.empty() &&
        std::ranges::none_of(state->devices, [&](const auto &device) {
          return _wcsicmp(device.path.c_str(),
                          state->captured_device_path.c_str()) == 0;
        })) {
      state->inferred.reset();
      state->captured_device_path.clear();
      state->listening = false;
      EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
      set_text(GetDlgItem(window, IDC_HID_SUMMARY),
               L"The captured device was disconnected. Capture again.");
    }
    populate_devices(window, *state);
    register_devices(window, *state);
    return TRUE;
  }
  if (message == WM_INPUT && state->listening) {
    UINT size{};
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr,
                        &size, sizeof(RAWINPUTHEADER)) != 0 ||
        size < sizeof(RAWINPUTHEADER) || size > 1024U * 1024U)
      return TRUE;
    std::vector<std::byte> storage(size);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                        storage.data(), &size, sizeof(RAWINPUTHEADER)) != size)
      return TRUE;
    const auto &input = *reinterpret_cast<const RAWINPUT *>(storage.data());
    const auto *device = selected_device(window, *state);
    if (device == nullptr || input.header.dwType != device->raw_type ||
        input.header.hDevice != device->handle)
      return TRUE;
    std::vector<layerc::platform::InferredHidControl> candidates;
    if (device->raw_type == RIM_TYPEHID) {
      if (input.data.hid.dwSizeHid == 0 || input.data.hid.dwCount == 0)
        return TRUE;
      const auto bytes = std::span(
          reinterpret_cast<const std::byte *>(input.data.hid.bRawData),
          static_cast<std::size_t>(input.data.hid.dwSizeHid) *
              input.data.hid.dwCount);
      for (std::size_t index = 0; index < input.data.hid.dwCount; ++index) {
        const auto report = bytes.subspan(index * input.data.hid.dwSizeHid,
                                          input.data.hid.dwSizeHid);
        const std::uint8_t report_id =
            device->uses_report_ids
                ? std::to_integer<std::uint8_t>(report.front())
                : 0U;
        const auto key = std::pair(device->handle, report_id);
        const auto baseline = state->baselines.find(key);
        if (baseline == state->baselines.end()) {
          state->baselines.emplace(
              key, std::vector<std::byte>(report.begin(), report.end()));
          set_text(GetDlgItem(window, IDC_HID_SUMMARY),
                   L"Idle baseline captured. Actuate exactly one control now.");
          continue;
        }
        auto inferred = layerc::platform::infer_hid_control(*device, report,
                                                            baseline->second);
        if (inferred)
          candidates.push_back(std::move(*inferred));
      }
    } else if (auto inferred =
                   layerc::platform::infer_hid_control(*device, input)) {
      candidates.push_back(std::move(*inferred));
    }
    if (!candidates.empty()) {
      auto &inferred = candidates.front();
      inferred.binding.allow_system_class =
          owner_draw_checkbox_checked(GetDlgItem(window, IDC_HID_SYSTEM_CLASS));
      state->inferred = std::move(inferred);
      state->captured_device_path = device->path;
      state->listening = false;
      set_text(GetDlgItem(window, IDC_HID_SUMMARY),
               L"Captured " + state->inferred->description +
                   L". Refine values if needed, then Use Binding.");
      set_text(GetDlgItem(window, IDC_HID_THRESHOLD),
               std::to_wstring(static_cast<unsigned>(
                   std::lround(state->inferred->binding.threshold * 100.0))));
      set_text(GetDlgItem(window, IDC_HID_HYSTERESIS),
               std::to_wstring(static_cast<unsigned>(
                   std::lround(state->inferred->binding.hysteresis * 100.0))));
      set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"Recapture");
      EnableWindow(GetDlgItem(window, IDC_HID_APPLY), TRUE);
    }
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_HID_SYSTEM_CLASS:
      if (HIWORD(wparam) == BN_CLICKED) {
        populate_devices(window, *state);
        register_devices(window, *state);
        if (state->inferred &&
            layerc::core::is_system_keyboard_or_mouse(
                state->inferred->binding.device) &&
            !owner_draw_checkbox_checked(
                GetDlgItem(window, IDC_HID_SYSTEM_CLASS))) {
          state->inferred.reset();
          EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
        }
      }
      return TRUE;
    case IDC_HID_DEVICE:
      if (HIWORD(wparam) == CBN_SELCHANGE) {
        state->listening = false;
        state->inferred.reset();
        state->captured_device_path.clear();
        state->baselines.clear();
        EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
        set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"Capture");
        set_text(GetDlgItem(window, IDC_HID_SUMMARY),
                 L"Press Capture, leave controls idle for one report, then "
                 L"actuate one control.");
      }
      return TRUE;
    case IDC_HID_CAPTURE:
      if (selected_device(window, *state) == nullptr)
        return TRUE;
      state->listening = true;
      state->inferred.reset();
      state->captured_device_path.clear();
      state->baselines.clear();
      EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
      set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"Listening...");
      set_text(GetDlgItem(window, IDC_HID_SUMMARY),
               L"Leave controls idle until the baseline is captured, then "
               L"actuate exactly one control.");
      return TRUE;
    case IDC_HID_APPLY: {
      if (!state->inferred)
        return TRUE;
      double threshold{}, hysteresis{};
      const auto slot =
          parse_uint(control_text(GetDlgItem(window, IDC_HID_SLOT)), 0);
      if (!read_percent(GetDlgItem(window, IDC_HID_THRESHOLD), 100,
                        threshold) ||
          !read_percent(GetDlgItem(window, IDC_HID_HYSTERESIS), 49,
                        hysteresis) ||
          slot < 1 || slot > 32) {
        MessageBoxW(
            window,
            L"Threshold must be 0-100, hysteresis 0-49, and stable slot 1-32.",
            L"HID binding", MB_OK | MB_ICONERROR);
        return TRUE;
      }
      const auto activation = state->inferred->binding.activation;
      if ((activation == layerc::core::HidActivationMode::axis_positive ||
           activation == layerc::core::HidActivationMode::axis_negative) &&
          hysteresis > std::min(threshold, 1.0 - threshold)) {
        MessageBoxW(window, L"Hysteresis crosses the selected axis threshold.",
                    L"HID binding", MB_OK | MB_ICONERROR);
        return TRUE;
      }
      state->inferred->binding.threshold = threshold;
      state->inferred->binding.hysteresis = hysteresis;
      state->inferred->binding.device.stable_slot =
          static_cast<std::uint16_t>(slot);
      state->inferred->binding.allow_system_class =
          owner_draw_checkbox_checked(GetDlgItem(window, IDC_HID_SYSTEM_CLASS));
      if (layerc::core::is_system_keyboard_or_mouse(
              state->inferred->binding.device) &&
          !state->inferred->binding.allow_system_class) {
        MessageBoxW(
            window,
            L"Keyboard/mouse-class HID requires the explicit Advanced opt-in.",
            L"HID binding", MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      if (state->inferred->binding.allow_system_class &&
          MessageBoxW(window,
                      L"This device may also produce normal keyboard or mouse "
                      L"events, so one action can be observed twice. Keep this "
                      L"device-specific HID binding?",
                      L"System-class HID warning",
                      MB_YESNO | MB_ICONWARNING) != IDYES)
        return TRUE;
      const auto device =
          std::ranges::find_if(state->devices, [&](const auto &candidate) {
            return _wcsicmp(candidate.path.c_str(),
                            state->captured_device_path.c_str()) == 0;
          });
      if (state->captured_device_path.empty() ||
          device == state->devices.end() ||
          !layerc::platform::assign_hid_stable_slot(
              state->inferred->binding.device, device->path)) {
        MessageBoxW(window,
                    L"LayerC could not save this machine's stable device-slot "
                    L"assignment.",
                    L"HID binding", MB_OK | MB_ICONERROR);
        return TRUE;
      }
      unregister_devices(*state);
      EndDialog(window, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      unregister_devices(*state);
      state->inferred.reset();
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_DESTROY)
    unregister_devices(*state);
  return state->editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

} // namespace layerc::editor
