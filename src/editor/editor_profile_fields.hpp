#pragma once

#include "layerc/core/profile.hpp"

#include <cstdint>
#include <span>

#include <windows.h>

namespace layerc::editor {

enum class ProfileFieldOwner : std::uint8_t { profile, cooldown, panel };
enum class ProfileFieldChange : std::uint8_t { text, immediate };

struct ProfileFieldContext {
  HWND dialog{};
  layerc::core::GameProfile &profile;
  layerc::core::CooldownDefinition *cooldown{};
  layerc::core::Panel *panel{};
};

struct ProfileFieldBinding {
  int control_id{};
  UINT notification{};
  ProfileFieldOwner owner{};
  ProfileFieldChange change{};
  bool refresh_cooldown_label{};
  bool invalidate_preview{};
  void (*read)(ProfileFieldContext &){};
  void (*write)(const ProfileFieldContext &){};
  bool (*valid)(const ProfileFieldContext &){};
};

[[nodiscard]] std::span<const ProfileFieldBinding> profile_field_bindings();
[[nodiscard]] const ProfileFieldBinding *profile_field_binding(int control_id);
void read_profile_fields(ProfileFieldContext &context);
void write_profile_fields(const ProfileFieldContext &context,
                          ProfileFieldOwner owner);
[[nodiscard]] bool profile_fields_valid(const ProfileFieldContext &context);

} // namespace layerc::editor
