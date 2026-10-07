
#include "daemon_state.hpp"
#include "input_demand.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::daemon {

SessionInit ProfileSource::to_session_init(const bool visual_enabled) const {
  return {.profile = profile,
          .source_key = key,
          .modules = modules,
          .visual_enabled = visual_enabled};
}

template <typename Module>
void Session::configure_module(const bool visual_enabled) {
  auto &sidecar = modules.get<Module>();
  if (!sidecar.usable()) sidecar.profile.reset();
  // Runtime behavior is module-specific, but loaded state is never enumerated
  // here: even a new module with no runtime consumer propagates automatically.
  if constexpr (std::is_same_v<Module, layerc::core::VisualModule>) {
    automation.set_invalid_visual_sidecar(
        sidecar.state == layerc::platform::ModuleSidecarState::invalid);
    last_visual_sequence = 0;
    automation.configure_visual(sidecar.profile, visual_enabled);
  } else if constexpr (std::is_same_v<Module, layerc::core::AutomationModule>) {
    if (!sidecar.profile) automation.discard_runtime();
    rebuild_input_configuration();
  }
}

Session::Session(SessionInit init)
    : automation(std::move(init.profile)),
      source_key(std::move(init.source_key)), input(automation.profile().bindings),
      modules(std::move(init.modules)) {
  modules.for_each([&](auto module, auto &) {
    configure_module<decltype(module)>(init.visual_enabled);
  });
  rebuild_input_configuration();
}

void Session::apply_modules(const ProfileModules &updated,
                            const bool visual_enabled) {
  modules.reconcile(updated, [&](auto module, auto &) {
    configure_module<decltype(module)>(visual_enabled);
  });
}

bool Session::has_enabled_automations() const noexcept {
  return modules.get<layerc::core::AutomationModule>().usable() &&
         layerc::core::has_enabled_automations(*modules.get<layerc::core::AutomationModule>().profile);
}
bool Session::has_active_visual_rules() const noexcept {
  return automation.visual_triggers() != nullptr && modules.get<layerc::core::VisualModule>().usable() &&
         layerc::core::has_enabled_visual_assignments(*modules.get<layerc::core::VisualModule>().profile) &&
         automation.visual_triggers()->visual_enabled();
}

namespace {

// Seeds matcher state for keyboard/controller tokens and records which
// device classes and global keys this session needs at runtime.
void seed_binding_token(Session &session,
                        const layerc::core::InputToken &token) {
  bool pressed{};
  UINT virtual_key{};
  if (token.device == layerc::core::InputDevice::keyboard) {
    virtual_key = MapVirtualKeyW(token.code, MAPVK_VSC_TO_VK_EX);
    pressed =
        virtual_key != 0 &&
        (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    session.input.seed(token, pressed);
  } else if (token.device == layerc::core::InputDevice::controller &&
             token.controller_index < XUSER_MAX_COUNT) {
    XINPUT_STATE state{};
    if (XInputGetState(token.controller_index, &state) == ERROR_SUCCESS) {
      if (token.code >= 1 && token.code <= 16) {
        const WORD mask = static_cast<WORD>(1U << (token.code - 1U));
        pressed = (state.Gamepad.wButtons & mask) != 0;
      } else if (token.code == 17) {
        pressed =
            state.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      } else if (token.code == 18) {
        pressed =
            state.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      }
      session.input.seed(token, pressed);
    }
  }
  if (!session.is_global() ||
      token.device != layerc::core::InputDevice::keyboard || virtual_key == 0)
    return;
  const auto existing = std::find_if(
      session.global_keyboard.begin(), session.global_keyboard.end(),
      [&](const Session::PolledKeyState &key) { return key.token == token; });
  if (existing == session.global_keyboard.end())
    session.global_keyboard.push_back(
        {.token = token, .virtual_key = virtual_key, .pressed = pressed});
}

} // namespace

void Session::rebuild_input_configuration() {
  hid_states.assign(automation.profile().hid_bindings.size(), {});
  global_keyboard.clear();
  for (const auto &binding : automation.profile().bindings)
    for (const auto &token : binding.chord.tokens)
      seed_binding_token(*this, token);
  const auto demand = input_device_demand(
      automation.profile(), modules.get<layerc::core::AutomationModule>().profile ? &*modules.get<layerc::core::AutomationModule>().profile
                                          : nullptr);
  uses_raw_keyboard = demand.keyboard;
  uses_raw_mouse = demand.mouse;
  uses_controller = demand.controller;
  uses_hid = demand.hid;
}

void Session::apply_profile(const layerc::core::GameProfile &updated,
                            const bool capture_exclusion,
                            const layerc::core::TimePoint now) {
  const auto plan = layerc::core::plan_session_reconciliation(automation.profile(), updated);
  automation.reconfigure_profile(updated, plan.reset_cooldown_ids, now);
  if (plan.rebuild_input_matcher) {
    input.replace_bindings(automation.profile().bindings);
    rebuild_input_configuration();
  }
  if (!plan.rebuild_overlays)
    return;
  if (!overlays.empty())
    overlays.rebuild(automation.profile(), capture_exclusion);
}

void Session::set_capture_exclusion(const bool enabled) {
  overlays.set_capture_exclusion(enabled);
}

void Session::clear_input_state() {
  input.clear();
  for (auto &key : global_keyboard) {
    key.pressed = false;
  }
}


} // namespace layerc::daemon
