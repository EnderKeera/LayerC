#pragma once

#include "layerc/core/profile.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

enum class ProfileFieldOwner : std::uint8_t { profile, cooldown, panel };
enum class ProfileFieldChange : std::uint8_t { text, immediate };

// An immutable projection of the document for widgets: writing/validating
// fields never needs mutable document storage.
struct ProfileFieldView {
  HWND dialog{};
  const layerc::core::GameProfile &profile;
  const layerc::core::CooldownDefinition *cooldown{};
  const layerc::core::Panel *panel{};
};

// A mutable context is used only around a detached edit candidate.
struct ProfileFieldContext {
  HWND dialog{};
  layerc::core::GameProfile &profile;
  layerc::core::CooldownDefinition *cooldown{};
  layerc::core::Panel *panel{};
  [[nodiscard]] operator ProfileFieldView() const noexcept {
    return {dialog, profile, cooldown, panel};
  }
};

// Up/Down and wheel stepping of a numeric field (Shift steps ten times as
// far). Bounds and step count whole 10^-decimals units of the displayed
// value, and the bounds lie inside the domain the field's validator accepts:
// both are built from the same core constants, so stepping never produces a
// value that typing would be refused.
struct ProfileFieldStepping {
  std::int64_t minimum{};
  std::int64_t maximum{};
  std::int64_t step{1};
  int decimals{};
};

struct ProfileFieldBinding {
  int control_id{};
  UINT notification{};
  ProfileFieldOwner owner{};
  ProfileFieldChange change{};
  bool refresh_cooldown_label{};
  bool invalidate_preview{};
  void (*read)(ProfileFieldContext &){};
  void (*write)(const ProfileFieldView &){};
  bool (*valid)(const ProfileFieldView &){};
  // Present only on numeric fields that step.
  const ProfileFieldStepping *stepping{};
};

[[nodiscard]] std::span<const ProfileFieldBinding> profile_field_bindings();
[[nodiscard]] const ProfileFieldBinding *profile_field_binding(int control_id);
void read_profile_fields(ProfileFieldContext &context);
void write_profile_fields(const ProfileFieldView &context,
                          ProfileFieldOwner owner);
[[nodiscard]] bool profile_fields_valid(const ProfileFieldView &context);
// The field text `steps` increments from `text`, clamped to the stepping
// range. Text that is not a number steps from the minimum.
[[nodiscard]] std::wstring stepped_field_text(std::wstring_view text,
                                              const ProfileFieldStepping &stepping,
                                              int steps);

} // namespace layerc::editor
