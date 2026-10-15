#include "layerc/core/hid.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace layerc::core {

bool is_system_keyboard_or_mouse(
    const HidDeviceSelector &selector) noexcept {
  return selector.usage_page == 0x01U &&
         (selector.usage == 0x02U || selector.usage == 0x06U);
}

void compact_hid_bindings(GameProfile &profile) {
  std::erase_if(profile.bindings, [&](const InputBinding &binding) {
    return std::ranges::any_of(binding.chord.tokens, [&](const InputToken &token) {
      return token.device == InputDevice::generic_hid &&
             (token.code == 0 || token.code > profile.hid_bindings.size());
    });
  });
  if (profile.hid_bindings.empty()) return;
  constexpr auto missing = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> remap(profile.hid_bindings.size(), missing);
  std::vector<HidControlBinding> compacted;
  compacted.reserve(profile.hid_bindings.size());

  // Unreferenced descriptors can carry opaque data owned by a newer editor.
  // Compaction must not erase that data as a side effect of another edit.
  for (std::size_t i{}; i < profile.hid_bindings.size(); ++i) {
    const auto &hid = profile.hid_bindings[i];
    if (hid.extra_fields.empty() && hid.device_extra_fields.empty() &&
        hid.control_extra_fields.empty()) continue;
    remap[i] = compacted.size();
    compacted.push_back(hid);
  }
  for (auto &binding : profile.bindings) {
    decltype(binding.token_extra_fields) token_fields;
    for (auto &token : binding.chord.tokens) {
      const auto original = token;
      if (token.device != InputDevice::generic_hid) {
        if (const auto extra = binding.token_extra_fields.find(original);
            extra != binding.token_extra_fields.end()) token_fields.emplace(token, extra->second);
        continue;
      }
      const auto old_index = static_cast<std::size_t>(token.code - 1U);
      if (remap[old_index] == missing) {
        remap[old_index] = compacted.size();
        compacted.push_back(profile.hid_bindings[old_index]);
      }
      token.code = static_cast<std::uint16_t>(remap[old_index] + 1U);
      if (const auto extra = binding.token_extra_fields.find(original);
          extra != binding.token_extra_fields.end()) token_fields.emplace(token, extra->second);
    }
    binding.token_extra_fields = std::move(token_fields);
  }
  profile.hid_bindings = std::move(compacted);
}

std::optional<bool>
evaluate_hid_control(const HidControlBinding &binding,
                     const HidControlSample &sample,
                     HidControlState &state) noexcept {
  if (!sample.available || sample.logical_max < sample.logical_min) {
    state = {};
    return std::nullopt;
  }

  bool pressed{};
  switch (binding.activation) {
  case HidActivationMode::button:
    pressed = sample.button_active;
    break;
  case HidActivationMode::equals:
    pressed = sample.value == binding.discrete_value;
    break;
  case HidActivationMode::relative_positive:
    if (!sample.relative) {
      state = {};
      return std::nullopt;
    }
    pressed = sample.value > 0;
    break;
  case HidActivationMode::relative_negative:
    if (!sample.relative) {
      state = {};
      return std::nullopt;
    }
    pressed = sample.value < 0;
    break;
  case HidActivationMode::axis_positive:
  case HidActivationMode::axis_negative: {
    const auto range =
        static_cast<double>(sample.logical_max) - sample.logical_min;
    if (range <= 0.0 || sample.relative) {
      state = {};
      return std::nullopt;
    }
    const double normalized = std::clamp(
        (static_cast<double>(sample.value) - sample.logical_min) / range, 0.0,
        1.0);
    const double threshold = std::clamp(binding.threshold, 0.0, 1.0);
    const double hysteresis = std::clamp(binding.hysteresis, 0.0, 0.49);
    if (binding.activation == HidActivationMode::axis_positive)
      pressed = state.pressed ? normalized >= threshold - hysteresis
                              : normalized >= threshold;
    else
      pressed = state.pressed ? normalized <= 1.0 - threshold + hysteresis
                              : normalized <= 1.0 - threshold;
    break;
  }
  }

  const bool changed = !state.initialized || state.pressed != pressed;
  state.initialized = true;
  state.pressed = pressed;
  if (!changed) return std::nullopt;
  return pressed;
}

} // namespace layerc::core
