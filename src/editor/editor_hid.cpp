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

// Device arrivals/removals are coalesced: one re-enumeration after the burst.
constexpr UINT_PTR device_refresh_timer = 1;
constexpr UINT device_refresh_delay_ms = 250;

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
  for (const auto &[page, usage] : state.registrations) {
    RAWINPUTDEVICE device{.usUsagePage = page,
                          .usUsage = usage,
                          .dwFlags = RIDEV_REMOVE,
                          .hwndTarget = nullptr};
    (void)RegisterRawInputDevices(&device, 1, sizeof(device));
  }
  state.registrations.clear();
}

// Raw input is registered only while Capture is listening, and only for the
// selected device's collection. Registration queues WM_INPUT_DEVICE_CHANGE
// arrivals for every attached device, so nothing outside Capture registers.
bool register_capture_device(const HWND window, HidDialogState &state,
                             const layerc::platform::HidRawDevice &device) {
  unregister_devices(state);
  const auto usage = std::pair(device.info.usUsagePage, device.info.usUsage);
  RAWINPUTDEVICE registration{.usUsagePage = usage.first,
                              .usUsage = usage.second,
                              .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                              .hwndTarget = window};
  if (RegisterRawInputDevices(&registration, 1, sizeof(registration)) ==
      FALSE)
    return false;
  state.registrations.insert(usage);
  return true;
}

bool known_device(const HidDialogState &state, const HANDLE handle) {
  return std::ranges::any_of(state.devices, [handle](const auto &device) {
    return device.handle == handle;
  });
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

void stop_listening(const HWND window, HidDialogState &state) {
  unregister_devices(state);
  if (!state.listening)
    return;
  state.listening = false;
  state.baselines.clear();
  set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"C&apture");
}

// Re-enumerates after a coalesced device change. Never re-registers raw
// input: that would queue a fresh burst of arrivals.
void refresh_devices(const HWND window, HidDialogState &state) {
  const auto *selected = selected_device(window, state);
  const HANDLE listening_handle =
      state.listening && selected != nullptr ? selected->handle : nullptr;
  state.devices = layerc::platform::enumerate_raw_hid_devices();
  if (!state.captured_device_path.empty() &&
      std::ranges::none_of(state.devices, [&](const auto &device) {
        return _wcsicmp(device.path.c_str(),
                        state.captured_device_path.c_str()) == 0;
      })) {
    state.inferred.reset();
    state.captured_device_path.clear();
    EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
    set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"C&apture");
    set_text(GetDlgItem(window, IDC_HID_SUMMARY),
             L"The captured device was disconnected. Capture again.");
  }
  populate_devices(window, state);
  if (listening_handle == nullptr)
    return;
  const auto *still_selected = selected_device(window, state);
  if (still_selected == nullptr || still_selected->handle != listening_handle) {
    stop_listening(window, state);
    set_text(GetDlgItem(window, IDC_HID_SUMMARY),
             L"The selected device was disconnected. Choose a device and "
             L"press Capture.");
  }
}

bool read_percent(const HWND control, const unsigned maximum, double &value) {
  const auto parsed = parse_bounded(window_text(control), 0, maximum);
  if (!parsed)
    return false;
  value = static_cast<double>(*parsed) / 100.0;
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
  if (DialogBoxParamW(shell_.instance, MAKEINTRESOURCEW(IDD_HID_BINDING), shell_.window,
                      &EditorWindow::hid_binding_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK ||
      !state.inferred)
    return;

  auto captured = std::move(state.inferred->binding);
  const auto cooldown_id = cooldown->id;
  std::size_t binding_index = SIZE_MAX;
  for (const auto &binding : document_.profile().bindings) {
    if (binding.cooldown_id != cooldown_id || binding.chord.tokens.size() != 1)
      continue;
    const auto &token = binding.chord.tokens.front();
    if (token.device == layerc::core::InputDevice::generic_hid &&
        token.code != 0 && token.code <= document_.profile().hid_bindings.size())
      binding_index = token.code - 1U;
  }
  const auto id_base =
      "hid-" + layerc::core::slugify(cooldown_id, "control");
  captured.id = next_unique_id(id_base, [&](const std::string_view id) {
    return std::ranges::any_of(document_.profile().hid_bindings,
                               [&](const auto &candidate) {
      const auto index = static_cast<std::size_t>(
          &candidate - document_.profile().hid_bindings.data());
      return candidate.id == id && index != binding_index;
    });
  });
  if (binding_index == SIZE_MAX) {
    if (document_.profile().hid_bindings.size() >= 128) {
      themed_message_box(shell_.window,
                  L"This profile already has the maximum 128 HID bindings.",
                  L"HID binding", MB_OK | MB_ICONERROR);
      return;
    }
    binding_index = document_.profile().hid_bindings.size();
    document_.profile().hid_bindings.push_back(std::move(captured));
  } else {
    document_.profile().hid_bindings[binding_index] = std::move(captured);
  }
  std::erase_if(document_.profile().bindings, [&](const auto &binding) {
    return binding.cooldown_id == cooldown_id;
  });
  document_.profile().bindings.push_back(
      {.cooldown_id = cooldown_id,
       .chord = layerc::core::InputChord{
           {{layerc::core::InputDevice::generic_hid,
             static_cast<std::uint16_t>(binding_index + 1U), 0}}}});
  layerc::core::compact_hid_bindings(document_.profile());
  refresh_binding_controls();
  record_profile_change(ChangeKind::immediate);
  set_status(document_.path().empty()
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
    apply_dialog_chrome(window, state->editor->shell_.capture_exclusion);
    state->editor->style_dialog_children(window);
    set_owner_draw_checkbox(GetDlgItem(window, IDC_HID_SYSTEM_CLASS), false);
    set_text(GetDlgItem(window, IDC_HID_THRESHOLD), L"50");
    set_text(GetDlgItem(window, IDC_HID_HYSTERESIS), L"10");
    set_text(GetDlgItem(window, IDC_HID_SLOT), L"1");
    EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
    populate_devices(window, *state);
    // A modal dialog is shown on the first idle message; show it now so a
    // busy queue can never leave a disabled owner with nothing to dismiss.
    ShowWindow(window, SW_SHOW);
    return TRUE;
  }
  if (message == WM_INPUT_DEVICE_CHANGE) {
    const auto handle = reinterpret_cast<HANDLE>(lparam);
    // Registration replays an arrival for every attached device; those and
    // removals of devices never listed change nothing.
    if ((wparam == GIDC_ARRIVAL) != known_device(*state, handle))
      SetTimer(window, device_refresh_timer, device_refresh_delay_ms, nullptr);
    return TRUE;
  }
  if (message == WM_TIMER && wparam == device_refresh_timer) {
    KillTimer(window, device_refresh_timer);
    refresh_devices(window, *state);
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
      stop_listening(window, *state);
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
        if (state->listening) {
          const auto *device = selected_device(window, *state);
          if (device == nullptr ||
              !register_capture_device(window, *state, *device))
            stop_listening(window, *state);
        }
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
      // Outside Capture nothing is registered for device notifications, so
      // opening the list re-enumerates.
      if (HIWORD(wparam) == CBN_DROPDOWN && !state->listening)
        refresh_devices(window, *state);
      if (HIWORD(wparam) == CBN_SELCHANGE) {
        stop_listening(window, *state);
        state->inferred.reset();
        state->captured_device_path.clear();
        state->baselines.clear();
        EnableWindow(GetDlgItem(window, IDC_HID_APPLY), FALSE);
        set_text(GetDlgItem(window, IDC_HID_CAPTURE), L"C&apture");
        set_text(GetDlgItem(window, IDC_HID_SUMMARY),
                 L"Press Capture, leave controls idle for one report, then "
                 L"actuate one control.");
      }
      return TRUE;
    case IDC_HID_CAPTURE: {
      const auto *device = selected_device(window, *state);
      if (device == nullptr)
        return TRUE;
      if (!register_capture_device(window, *state, *device)) {
        stop_listening(window, *state);
        set_text(GetDlgItem(window, IDC_HID_SUMMARY),
                 L"Windows refused raw input for this device. Choose "
                 L"another device.");
        return TRUE;
      }
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
    }
    case IDC_HID_APPLY: {
      if (!state->inferred)
        return TRUE;
      double threshold{}, hysteresis{};
      const auto slot =
          parse_uint(window_text(GetDlgItem(window, IDC_HID_SLOT)), 0);
      if (!read_percent(GetDlgItem(window, IDC_HID_THRESHOLD), 100,
                        threshold) ||
          !read_percent(GetDlgItem(window, IDC_HID_HYSTERESIS), 49,
                        hysteresis) ||
          slot < 1 || slot > 32) {
        themed_message_box(
            window,
            L"Threshold must be 0-100, hysteresis 0-49, and stable slot 1-32.",
            L"HID binding", MB_OK | MB_ICONERROR);
        return TRUE;
      }
      const auto activation = state->inferred->binding.activation;
      if ((activation == layerc::core::HidActivationMode::axis_positive ||
           activation == layerc::core::HidActivationMode::axis_negative) &&
          hysteresis > std::min(threshold, 1.0 - threshold)) {
        themed_message_box(window, L"Hysteresis crosses the selected axis threshold.",
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
        themed_message_box(
            window,
            L"Keyboard/mouse-class HID requires the explicit Advanced opt-in.",
            L"HID binding", MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      if (state->inferred->binding.allow_system_class &&
          themed_message_box(window,
                      L"This device may also produce normal keyboard or mouse "
                      L"events, so one action can be observed twice. Keep this "
                      L"device-specific HID binding?",
                      L"System-class HID warning",
                      MB_YESNO | MB_ICONWARNING) != IDYES)
        return TRUE;
      // Nothing is registered after capture, so re-check the device now.
      const auto current = layerc::platform::enumerate_raw_hid_devices();
      const auto device =
          std::ranges::find_if(current, [&](const auto &candidate) {
            return _wcsicmp(candidate.path.c_str(),
                            state->captured_device_path.c_str()) == 0;
          });
      if (state->captured_device_path.empty() || device == current.end()) {
        refresh_devices(window, *state);
        return TRUE;
      }
      if (!layerc::platform::assign_hid_stable_slot(
              state->inferred->binding.device, device->path)) {
        themed_message_box(window,
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
  if (message == WM_DESTROY) {
    KillTimer(window, device_refresh_timer);
    unregister_devices(*state);
  }
  return state->editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

} // namespace layerc::editor
