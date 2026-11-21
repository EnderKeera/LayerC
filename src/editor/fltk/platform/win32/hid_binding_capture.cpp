#include "platform/hid_binding_capture.hpp"
#include "platform/win32/binding_controls.hpp"

#include "layerc/core/hid.hpp"
#include "layerc/platform/hid_device.hpp"
#include "layerc/platform/text.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <commctrl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <utility>

namespace layerc::editor {
namespace {

constexpr UINT_PTR hid_capture_subclass_id = 1;
constexpr UINT max_raw_input_bytes = 1024U * 1024U;
constexpr auto raw_header_bytes = static_cast<UINT>(sizeof(RAWINPUTHEADER));

bool system_class(const platform::HidRawDevice &device) noexcept {
  return device.info.usUsagePage == 0x01 &&
         (device.info.usUsage == 0x02 || device.info.usUsage == 0x06);
}

bool same_path(const std::wstring &lhs, const std::wstring &rhs) noexcept {
  return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool collection_is_free(const USHORT page, const USHORT usage) {
  UINT count{};
  if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) ==
          static_cast<UINT>(-1) &&
      GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return false;
  if (count == 0) return true;
  std::vector<RAWINPUTDEVICE> devices(count);
  const UINT read = GetRegisteredRawInputDevices(devices.data(), &count,
                                                  sizeof(RAWINPUTDEVICE));
  if (read == static_cast<UINT>(-1) || read > devices.size()) return false;
  return std::ranges::none_of(std::span(devices).first(read), [&](const auto &entry) {
    return entry.usUsagePage == page && entry.usUsage == usage;
  });
}

class WinHidBindingCapture final : public HidBindingCapture {
public:
  ~WinHidBindingCapture() override { stop(); }

  std::vector<std::string> scan(const bool include_system) override {
    stop();
    captured_.reset();
    devices_ = platform::enumerate_raw_hid_devices();
    if (!include_system) std::erase_if(devices_, system_class);
    std::vector<std::string> names;
    names.reserve(devices_.size());
    for (const auto &device : devices_) {
      auto label = platform::wide_to_utf8(device.product_name);
      if (system_class(device)) label += " (system class)";
      names.push_back(std::move(label));
    }
    return names;
  }

  std::optional<std::string> start(const std::size_t index, Update update) override {
    stop();
    captured_.reset();
    if (index >= devices_.size()) return "Select a connected device first.";
    const auto &listed = devices_[index];
    auto current = platform::load_raw_hid_device(listed.handle);
    if (!current || !same_path(current->path, listed.path) ||
        current->descriptor_hash != listed.descriptor_hash)
      return "The device changed or was disconnected. Refresh the device list.";

    auto *modal = Fl::modal();
    const HWND window = modal && modal->shown() ? fl_xid(modal) : nullptr;
    if (window == nullptr || !focused(window))
      return "The HID binding dialog needs focus before it can listen.";

    const auto page = current->info.usUsagePage;
    const auto usage = current->info.usUsage;
    if (!collection_is_free(page, usage))
      return "Could not listen because this input collection is already in use.";
    if (!SetWindowSubclass(window, procedure, hid_capture_subclass_id,
                           reinterpret_cast<DWORD_PTR>(this)))
      return "Could not listen for HID input in this dialog.";

    hwnd_ = window;
    const RAWINPUTDEVICE entry{.usUsagePage = page,
                               .usUsage = usage,
                               .dwFlags = RIDEV_DEVNOTIFY,
                               .hwndTarget = window};
    // Foreground delivery only, without changing normal keyboard/mouse events.
    if (RegisterRawInputDevices(&entry, 1, sizeof(entry)) == FALSE) {
      stop();
      return "Windows refused input from this collection. Choose another device.";
    }
    device_ = std::move(current);
    update_ = std::move(update);
    return std::nullopt;
  }

  void stop() noexcept override {
    if (device_) {
      const RAWINPUTDEVICE entry{.usUsagePage = device_->info.usUsagePage,
                                 .usUsage = device_->info.usUsage,
                                 .dwFlags = RIDEV_REMOVE,
                                 .hwndTarget = nullptr};
      (void)RegisterRawInputDevices(&entry, 1, sizeof(entry));
    }
    if (const auto window = std::exchange(hwnd_, nullptr))
      (void)RemoveWindowSubclass(window, procedure, hid_capture_subclass_id);
    device_.reset();
    baselines_.clear();
    update_ = {};
  }

  std::optional<std::string>
  assign_slot(const core::HidDeviceSelector &selector, const bool replace) override {
    if (!captured_) return "Capture the control again before saving its slot.";
    auto identity = captured_->selector;
    identity.stable_slot = selector.stable_slot;
    if (identity != selector)
      return "The selected slot no longer refers to the captured device.";
    const auto current = platform::enumerate_raw_hid_devices();
    if (std::ranges::none_of(current, [&](const auto &device) {
          return same_path(device.path, captured_->path) &&
                 device.descriptor_hash == selector.descriptor_hash;
        }))
      return "The captured device was disconnected. Refresh the list and capture again.";
    switch (platform::assign_hid_stable_slot_checked(selector, captured_->path, replace)) {
    case platform::HidSlotAssignment::assigned:
      return std::nullopt;
    case platform::HidSlotAssignment::occupied:
      return "This slot belongs to another device. Choose another slot or allow replacement.";
    case platform::HidSlotAssignment::unreadable:
      return "Could not read the existing slot. Check the Windows settings and retry.";
    case platform::HidSlotAssignment::write_failed:
      break;
    }
    return "Could not save this device's stable slot. Check the Windows settings and retry.";
  }

private:
  static bool focused(const HWND window) {
    auto *modal = Fl::modal();
    return modal && modal->shown() && fl_xid(modal) == window && owns_native_focus(window);
  }

  void finish(HidCaptureUpdate update) {
    auto notify = std::move(update_);
    stop();
    if (notify) notify(std::move(update));
  }

  void report(const HRAWINPUT handle) {
    UINT size{};
    if (!device_ ||
        GetRawInputData(handle, RID_INPUT, nullptr, &size, raw_header_bytes) != 0 ||
        size < raw_header_bytes || size > max_raw_input_bytes)
      return;

    std::vector<std::byte> bytes(size);
    const UINT read = GetRawInputData(handle, RID_INPUT, bytes.data(), &size,
                                     raw_header_bytes);
    if (read < raw_header_bytes || read != bytes.size()) return;
    RAWINPUT input{};
    std::memcpy(&input.header, bytes.data(), sizeof(input.header));
    if (input.header.dwSize < raw_header_bytes || input.header.dwSize > read ||
        input.header.hDevice != device_->handle || input.header.dwType != device_->raw_type)
      return;

    std::optional<platform::InferredHidControl> inferred;
    bool first_baseline{};
    if (input.header.dwType == RIM_TYPEHID) {
      constexpr auto data_offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
      if (input.header.dwSize < data_offset) return;
      constexpr auto size_offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, dwSizeHid);
      constexpr auto count_offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, dwCount);
      DWORD report_size{}, report_count{};
      std::memcpy(&report_size, bytes.data() + size_offset, sizeof(report_size));
      std::memcpy(&report_count, bytes.data() + count_offset, sizeof(report_count));
      auto payload = std::span<const std::byte>(bytes).first(input.header.dwSize).subspan(data_offset);
      if (report_size == 0 || report_count == 0 ||
          report_count > payload.size() / report_size)
        return;
      for (DWORD i = 0; i < report_count && !inferred; ++i) {
        const auto report = payload.subspan(static_cast<std::size_t>(i) * report_size,
                                            report_size);
        const auto id = platform::effective_hid_report_id(*device_, report);
        if (const auto baseline = baselines_.find(id); baseline != baselines_.end()) {
          inferred = platform::infer_hid_control(*device_, report, baseline->second);
        } else {
          first_baseline = first_baseline || baselines_.empty();
          baselines_.emplace(id, std::vector<std::byte>(report.begin(), report.end()));
        }
      }
    } else {
      constexpr auto data_offset = offsetof(RAWINPUT, data);
      const auto data_size = input.header.dwType == RIM_TYPEKEYBOARD
                                 ? sizeof(RAWKEYBOARD) : sizeof(RAWMOUSE);
      if (input.header.dwSize < data_offset + data_size) return;
      std::memcpy(&input.data, bytes.data() + data_offset, data_size);
      // A release of the key or mouse button used to start capture is not
      // an intended input. Do not let unrelated button-up flags win over a
      // new button press or motion in the same report.
      if (input.header.dwType == RIM_TYPEKEYBOARD) {
        if ((input.data.keyboard.Flags & RI_KEY_BREAK) != 0U ||
            input.data.keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE)
          return;
      } else {
        constexpr USHORT release_flags = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP |
            RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        input.data.mouse.usButtonFlags &= static_cast<USHORT>(~release_flags);
      }
      inferred = platform::infer_hid_control(*device_, input);
    }

    if (!inferred) {
      // Announcing can pump native messages. Call only after the whole packet
      // has been processed and keep the callback alive if capture stops.
      if (first_baseline) {
        if (const auto notify = update_)
          notify({.listening = true, .control = std::nullopt,
                  .text = "Idle baseline captured. Move or press one control."});
      }
      return;
    }
    auto &binding = inferred->binding;
    binding.allow_system_class = core::is_system_keyboard_or_mouse(binding.device);
    captured_ = Captured{device_->path, binding.device};
    finish({.listening = false, .control = std::move(binding),
            .text = "Captured " + platform::wide_to_utf8(inferred->description) +
                    ". Review the slot and values, then use the binding."});
  }

  static LRESULT CALLBACK procedure(const HWND window, const UINT message,
                                    const WPARAM wparam, const LPARAM lparam,
                                    UINT_PTR, const DWORD_PTR data) {
    auto &self = *reinterpret_cast<WinHidBindingCapture *>(data);
    if (message == WM_NCDESTROY) {
      self.stop();
    } else if (message == WM_KILLFOCUS ||
               (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) ||
               (message == WM_ACTIVATEAPP && wparam == 0) ||
               (message == WM_SHOWWINDOW && wparam == 0) ||
               (message == WM_SIZE && wparam == SIZE_MINIMIZED)) {
      self.finish({.listening = false, .control = std::nullopt,
                   .text = "Capture stopped when the dialog lost focus. Press Capture to retry."});
    } else if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL &&
               self.device_ && self.device_->handle == reinterpret_cast<HANDLE>(lparam)) {
      self.finish({.listening = false, .control = std::nullopt,
                   .text = "The selected device was disconnected. Refresh the device list."});
    } else if (message == WM_INPUT && GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT &&
               focused(window)) {
      self.report(reinterpret_cast<HRAWINPUT>(lparam));
    }
    // Keep normal dispatch, including DefWindowProc's foreground WM_INPUT cleanup.
    return DefSubclassProc(window, message, wparam, lparam);
  }

  struct Captured {
    std::wstring path;
    core::HidDeviceSelector selector;
  };

  HWND hwnd_{};
  std::vector<platform::HidRawDevice> devices_;
  std::optional<platform::HidRawDevice> device_;
  std::map<std::uint8_t, std::vector<std::byte>> baselines_;
  std::optional<Captured> captured_;
  Update update_;
};

} // namespace

bool hid_binding_capture_available() noexcept { return true; }

std::unique_ptr<HidBindingCapture> make_hid_binding_capture() {
  return std::make_unique<WinHidBindingCapture>();
}

} // namespace layerc::editor
