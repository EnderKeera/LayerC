#include "daemon_state.hpp"
#include "hid_resolver.hpp"

#include "layerc/platform/hid_device.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace layerc::daemon {
namespace {

struct TopLevelUsage {
  USHORT page{};
  USHORT usage{};
  auto operator<=>(const TopLevelUsage &) const = default;
};

void unregister_usage(const TopLevelUsage &usage) noexcept {
  RAWINPUTDEVICE remove{.usUsagePage = usage.page,
                        .usUsage = usage.usage,
                        .dwFlags = RIDEV_REMOVE,
                        .hwndTarget = nullptr};
  (void)RegisterRawInputDevices(&remove, 1, sizeof(remove));
}

} // namespace

struct HidRuntimeState {
  using Device = layerc::platform::HidRawDevice;
  std::map<HANDLE, Device, std::less<>> devices;
  std::set<TopLevelUsage> registered_usages;
  std::vector<layerc::core::HidDeviceSelector> active_selectors;
  HidResolver resolver;
  bool devices_dirty{true};
};

HidRuntime::HidRuntime(SessionRegistry &sessions, const HWND &window,
                       InputRuntime &input, AutomationRuntimeHost &automation,
                       RuntimeNotifications &notifications) noexcept
    : sessions_(sessions), window_(window), input_(input), automation_(automation),
      notifications_(notifications) {}
HidRuntime::~HidRuntime() = default;

layerc::runtime::ServiceReconcileResult
HidRuntime::reconcile(const layerc::runtime::CapabilityDemand &) {
  if (!state_)
    state_ = std::make_unique<HidRuntimeState>();
  auto &hid = *state_;
  std::set<TopLevelUsage> required;
  std::vector<layerc::core::HidDeviceSelector> active_selectors;
  for (const auto *session : sessions_.active_inputs) {
    if (session == nullptr) continue;
    for (const auto &binding : session->automation.profile().hid_bindings) {
      if (std::ranges::find(active_selectors, binding.device) ==
          active_selectors.end())
        active_selectors.push_back(binding.device);
      // Keyboard/mouse top-level usages are owned by InputRuntimeService so
      // one process-wide RIDEV_REMOVE can never invalidate the other service.
      if (layerc::core::is_system_keyboard_or_mouse(binding.device)) continue;
      required.insert({binding.device.usage_page, binding.device.usage});
    }
  }
  hid.resolver.retain(active_selectors);
  const bool selector_set_changed =
      active_selectors.size() != hid.active_selectors.size() ||
      std::ranges::any_of(active_selectors, [&](const auto &selector) {
        return std::ranges::find(hid.active_selectors, selector) ==
               hid.active_selectors.end();
      });
  const bool usage_set_changed =
      required != hid.registered_usages;

  std::set<TopLevelUsage> retained;
  for (const auto &usage : hid.registered_usages) {
    if (required.contains(usage)) {
      retained.insert(usage);
      continue;
    }
    unregister_usage(usage);
  }
  std::vector<TopLevelUsage> added;
  bool registration_ok = true;
  for (const auto &usage : required) {
    if (retained.contains(usage)) continue;
    RAWINPUTDEVICE add{.usUsagePage = usage.page,
                       .usUsage = usage.usage,
                       .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                       .hwndTarget = window_};
    if (RegisterRawInputDevices(&add, 1, sizeof(add)) == FALSE) {
      registration_ok = false;
      break;
    }
    added.push_back(usage);
  }
  if (!registration_ok) {
    for (const auto &usage : added) {
      unregister_usage(usage);
    }
  } else {
    retained.insert(added.begin(), added.end());
  }
  hid.registered_usages = std::move(retained);

  if (!registration_ok)
    return layerc::runtime::ServiceReconcileResult::failed(
        "Raw Input rejected a required HID usage");
  hid.active_selectors = std::move(active_selectors);
  if (selector_set_changed || usage_set_changed)
    hid.devices_dirty = true;

  if (hid.devices_dirty) {
    hid.devices.clear();
    for (auto &device : layerc::platform::enumerate_raw_hid_devices())
      hid.devices.emplace(device.handle, std::move(device));
    hid.resolver.clear();
    hid.devices_dirty = false;
  }
  return {};
}

layerc::runtime::RuntimeResourceStats HidRuntime::resources() const noexcept {
  if (!state_) return {};
  return {
      .objects = 1U + static_cast<std::uint32_t>(state_->devices.size()) +
                 static_cast<std::uint32_t>(state_->registered_usages.size()) +
                 static_cast<std::uint32_t>(state_->resolver.size()),
  };
}

void HidRuntime::quiesce() noexcept {
  if (!state_) return;
  for (const auto &usage : state_->registered_usages)
    unregister_usage(usage);
  for (auto &session : sessions_.sessions) {
    session->hid_states.clear();
    session->clear_input_state();
  }
  state_.reset();
}

void HidRuntime::handle_device_change(const HANDLE, const bool) {
  if (!state_) return;
  state_->devices_dirty = true;
  state_->resolver.clear();
  for (auto &session : sessions_.sessions) {
    session->hid_states.assign(session->automation.profile().hid_bindings.size(), {});
    session->clear_input_state();
  }
  // Re-enter through RuntimeServiceHost so a registration/enumeration failure
  // is rolled back and appears in diagnostics instead of leaving partial HID
  // state active.
  notifications_.reconcile_runtime_services();
}

namespace {

// Relative samples normalize against the larger magnitude endpoint so a
// signed axis maps into roughly [-1, 1]; absolute samples map the logical
// range onto [0, 1].
double normalized_hid_value(const layerc::core::HidControlSample &sample) {
  if (sample.relative) {
    const double extent = std::max(
        std::abs(static_cast<double>(sample.logical_min)),
        std::abs(static_cast<double>(sample.logical_max)));
    return static_cast<double>(sample.value) / std::max(1.0, extent);
  }
  return (static_cast<double>(sample.value) - sample.logical_min) /
         static_cast<double>(sample.logical_max - sample.logical_min);
}

} // namespace

void HidRuntime::process_sample(
    Session &session, const std::size_t index,
    const layerc::core::HidControlBinding &binding,
    const layerc::core::HidControlSample &sample) {
  auto &state = session.hid_states[index];
  const bool initialized = state.initialized;
  if (session.automation.runtime() && sample.available &&
      sample.logical_max > sample.logical_min) {
    const bool momentary = sample.relative;
    if (!momentary || sample.value != 0)
      automation_.process_input_value(
          session, layerc::core::hid_source_id(index + 1U),
          normalized_hid_value(sample),
          layerc::core::MonotonicClock::now(), momentary);
  }
  const auto edge = layerc::core::evaluate_hid_control(binding, sample, state);
  if (!edge)
    return;

  const layerc::core::InputToken token{
      .device = layerc::core::InputDevice::generic_hid,
      .code = static_cast<std::uint16_t>(index + 1U)};
  const bool relative =
      binding.activation ==
          layerc::core::HidActivationMode::relative_positive ||
      binding.activation == layerc::core::HidActivationMode::relative_negative;
  if (!initialized && !relative) {
    session.input.seed(token, *edge);
    return;
  }
  input_.process_input(session, {.token = token, .pressed = *edge});
  if (relative && *edge) {
    input_.process_input(session, {.token = token, .pressed = false});
    state.initialized = true;
    state.pressed = false;
  }
}

void HidRuntime::handle_input(const RAWINPUT &input) {
  if (!state_ || sessions_.active_inputs.empty())
    return;
  auto device = state_->devices.find(input.header.hDevice);
  if (device == state_->devices.end()) {
    auto loaded =
        layerc::platform::load_raw_hid_device(input.header.hDevice);
    if (!loaded) return;
    device = state_->devices.emplace(input.header.hDevice, std::move(*loaded))
                 .first;
    state_->resolver.clear();
  }

  const auto evaluate_for_sessions = [&](const auto &sample_for) {
    for (auto *session : sessions_.active_inputs) {
      if (session == nullptr || !session->uses_hid) continue;
      if (session->hid_states.size() != session->automation.profile().hid_bindings.size())
        session->hid_states.assign(session->automation.profile().hid_bindings.size(), {});
      for (std::size_t index = 0;
           index < session->automation.profile().hid_bindings.size(); ++index) {
        const auto &binding = session->automation.profile().hid_bindings[index];
        if (!state_->resolver.resolves_to(
                device->second, binding.device, state_->devices))
          continue;
        const auto sample = sample_for(binding);
        if (sample.has_value())
          process_sample(*session, index, binding, *sample);
      }
    }
  };

  if (input.header.dwType == RIM_TYPEHID) {
    if (input.data.hid.dwSizeHid == 0 || input.data.hid.dwCount == 0) return;
    const auto raw = std::span(
        reinterpret_cast<const std::byte *>(input.data.hid.bRawData),
        static_cast<std::size_t>(input.data.hid.dwSizeHid) *
            input.data.hid.dwCount);
    for (std::size_t report_index = 0;
         report_index < input.data.hid.dwCount; ++report_index) {
      const auto report = raw.subspan(
          report_index * input.data.hid.dwSizeHid,
          input.data.hid.dwSizeHid);
      evaluate_for_sessions([&](const auto &binding)
                                -> std::optional<layerc::core::HidControlSample> {
        if (device->second.uses_report_ids && binding.report_id != 0 &&
            (report.empty() ||
             layerc::platform::effective_hid_report_id(device->second,
                                                        report) !=
                 binding.report_id))
          return std::nullopt;
        return layerc::platform::sample_hid_control(device->second, binding,
                                                     report);
      });
    }
  } else {
    evaluate_for_sessions([&](const auto &binding) {
      return layerc::platform::sample_hid_control(device->second, binding,
                                                   input);
    });
  }
}

} // namespace layerc::daemon
