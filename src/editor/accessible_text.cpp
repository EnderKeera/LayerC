#include "accessible_text.hpp"

#include "editor_text.hpp"
#include "layerc/core/json.hpp"

#include <array>
#include <cstring>
#include <string>

#include <windows.h>
// Defines the PROPID_ACC_* GUIDs oleacc.h declares, in this unit only.
#include <initguid.h>
#include <oleacc.h>
#include <oleauto.h>
#include <uiautomation.h>

namespace layerc::editor {
namespace {

void set_property(const HWND control, const MSAAPROPID &property,
                  const std::wstring_view text) {
  if (control == nullptr)
    return;
  IAccPropServices *services{};
  if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr,
                              CLSCTX_INPROC_SERVER, IID_IAccPropServices,
                              reinterpret_cast<void **>(&services))) ||
      services == nullptr)
    return;
  const auto object = static_cast<DWORD>(OBJID_CLIENT);
  const auto child = static_cast<DWORD>(CHILDID_SELF);
  if (text.empty()) {
    (void)services->ClearHwndProps(control, object, child, &property, 1);
  } else {
    const std::wstring value(text);
    (void)services->SetHwndPropStr(control, object, child, property,
                                   value.c_str());
  }
  services->Release();
}

// UI Automation's provider entry points, resolved at run time: notification
// events arrived with Windows 10 1709, and the editor starts without them.
struct AutomationCore {
  using ClientsAreListening = BOOL(WINAPI *)();
  using HostProviderFromHwnd = HRESULT(WINAPI *)(HWND,
                                                 IRawElementProviderSimple **);
  using RaiseNotificationEvent =
      HRESULT(WINAPI *)(IRawElementProviderSimple *, NotificationKind,
                        NotificationProcessing, BSTR, BSTR);

  ClientsAreListening clients_are_listening{};
  HostProviderFromHwnd host_provider_from_hwnd{};
  RaiseNotificationEvent raise_notification_event{};

  [[nodiscard]] bool available() const {
    return clients_are_listening != nullptr &&
           host_provider_from_hwnd != nullptr &&
           raise_notification_event != nullptr;
  }
};

template <typename T> T automation_function(const HMODULE module,
                                            const char *name) {
  const FARPROC procedure = GetProcAddress(module, name);
  static_assert(sizeof(T) == sizeof(procedure));
  T function{};
  std::memcpy(&function, &procedure, sizeof(function));
  return function;
}

const AutomationCore &automation_core() {
  static const AutomationCore core = [] {
    AutomationCore loaded;
    const HMODULE module = LoadLibraryExW(L"uiautomationcore.dll", nullptr,
                                          LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr)
      return loaded;
    loaded.clients_are_listening =
        automation_function<AutomationCore::ClientsAreListening>(
            module, "UiaClientsAreListening");
    loaded.host_provider_from_hwnd =
        automation_function<AutomationCore::HostProviderFromHwnd>(
            module, "UiaHostProviderFromHwnd");
    loaded.raise_notification_event =
        automation_function<AutomationCore::RaiseNotificationEvent>(
            module, "UiaRaiseNotificationEvent");
    return loaded;
  }();
  return core;
}

// The last status message spoken. The editor has one status line, written
// from the window's thread only.
std::wstring &last_announced_status() {
  static std::wstring text;
  return text;
}

} // namespace

void set_accessible_name(const HWND control, const std::wstring_view text) {
  set_property(control, PROPID_ACC_NAME, text);
}

void set_accessible_description(const HWND control,
                                const std::wstring_view text) {
  set_property(control, PROPID_ACC_DESCRIPTION, text);
  set_property(control, PROPID_ACC_HELP, text);
}

void describe_with(const HWND control, const HWND hint) {
  set_accessible_description(control, window_text(hint));
}

std::wstring color_hex(const std::uint32_t color) {
  return to_wide(layerc::core::json::color_text(color));
}

std::wstring color_button_name(const std::wstring_view label,
                               const std::wstring_view color) {
  std::wstring name;
  name.reserve(label.size() + color.size() + 8);
  for (std::size_t index = 0; index < label.size(); ++index) {
    if (label[index] != L'&') {
      name += label[index];
      continue;
    }
    // "&&" is a literal ampersand; a single one marks the access key.
    if (index + 1 < label.size() && label[index + 1] == L'&') {
      name += L'&';
      ++index;
    }
  }
  name += L", color ";
  name += color;
  return name;
}

void set_color_button_name(const HWND button, const std::wstring_view color) {
  if (button != nullptr)
    set_accessible_name(button, color_button_name(window_text(button), color));
}

std::wstring swatch_name(const std::wstring_view name,
                         const std::wstring_view color, const int position,
                         const int count) {
  std::wstring text(name);
  text += L' ';
  text += color;
  text += L", " + std::to_wstring(position) + L" of " + std::to_wstring(count);
  return text;
}

void set_status_text(const HWND status, const std::wstring_view text,
                     const StatusVoice voice) {
  set_text(status, text);
  auto &last = last_announced_status();
  if (!status_is_announced(last, text, voice))
    return;
  last = text;
  announce(status, text);
}

void announce(const HWND control, const std::wstring_view text) {
  const auto &core = automation_core();
  if (control == nullptr || text.empty() || !core.available() ||
      core.clients_are_listening() == FALSE)
    return;
  IRawElementProviderSimple *provider{};
  if (FAILED(core.host_provider_from_hwnd(control, &provider)) ||
      provider == nullptr)
    return;
  const BSTR display =
      SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
  const BSTR activity = SysAllocString(L"LayerC.Status");
  if (display != nullptr && activity != nullptr)
    (void)core.raise_notification_event(
        provider, NotificationKind_ActionCompleted,
        NotificationProcessing_ImportantMostRecent, display, activity);
  SysFreeString(activity);
  SysFreeString(display);
  provider->Release();
}

} // namespace layerc::editor
