#include "editor_profile_fields.hpp"
#include "editor_resource.h"
#include "editor_text.hpp"
#include "editor_util.hpp"

#include <algorithm>
#include <array>
#include <cwchar>

namespace layerc::editor {
namespace {

HWND field_control(const ProfileFieldView &context, const int id) {
  return GetDlgItem(context.dialog, id);
}

bool bounded_decimal(const std::wstring &text, const std::uint32_t minimum,
                     const std::uint32_t maximum) {
  return parse_bounded(text, minimum, maximum).has_value();
}

bool field_owner_available(const ProfileFieldView &context,
                           const ProfileFieldOwner owner) {
  switch (owner) {
  case ProfileFieldOwner::profile:
    return true;
  case ProfileFieldOwner::cooldown:
    return context.cooldown != nullptr;
  case ProfileFieldOwner::panel:
    return context.panel != nullptr;
  }
  return false;
}

const std::array kProfileFields{
    ProfileFieldBinding{
        .control_id = IDC_PROFILE_NAME,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::profile,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.profile.name =
                  wide_to_utf8(window_text(field_control(context, IDC_PROFILE_NAME)));
            },
        .write =
            +[](const ProfileFieldView &context) {
              set_text(field_control(context, IDC_PROFILE_NAME),
                       to_wide(context.profile.name));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return !window_text(field_control(context, IDC_PROFILE_NAME))
                          .empty();
            }},
    ProfileFieldBinding{
        .control_id = IDC_PROFILE_ID,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::profile,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.profile.id =
                  wide_to_utf8(window_text(field_control(context, IDC_PROFILE_ID)));
            },
        .write =
            +[](const ProfileFieldView &context) {
              set_text(field_control(context, IDC_PROFILE_ID),
                       to_wide(context.profile.id));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return !window_text(field_control(context, IDC_PROFILE_ID))
                          .empty();
            }},
    ProfileFieldBinding{
        .control_id = IDC_COOLDOWN_NAME,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::cooldown,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = true,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.cooldown->name =
                  wide_to_utf8(window_text(field_control(context, IDC_COOLDOWN_NAME)));
            },
        .write =
            +[](const ProfileFieldView &context) {
              set_text(field_control(context, IDC_COOLDOWN_NAME),
                       context.cooldown == nullptr ? L""
                                                   : to_wide(context.cooldown->name));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return context.cooldown != nullptr &&
                     !window_text(field_control(context, IDC_COOLDOWN_NAME))
                          .empty();
            }},
    ProfileFieldBinding{
        .control_id = IDC_COOLDOWN_DURATION,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::cooldown,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = true,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              if (const auto duration = parse_duration_ms(
                      window_text(field_control(context, IDC_COOLDOWN_DURATION))))
                context.cooldown->duration = *duration;
            },
        .write =
            +[](const ProfileFieldView &context) {
              set_text(field_control(context, IDC_COOLDOWN_DURATION),
                       context.cooldown == nullptr
                           ? L""
                           : duration_seconds_text(context.cooldown->duration));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return context.cooldown != nullptr &&
                     parse_duration_ms(window_text(
                         field_control(context, IDC_COOLDOWN_DURATION)))
                         .has_value();
            }},
    ProfileFieldBinding{
        .control_id = IDC_COOLDOWN_CHARGES,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::cooldown,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.cooldown->max_charges = parse_uint(
                  window_text(field_control(context, IDC_COOLDOWN_CHARGES)),
                  context.cooldown->max_charges);
            },
        .write =
            +[](const ProfileFieldView &context) {
              set_text(field_control(context, IDC_COOLDOWN_CHARGES),
                       context.cooldown == nullptr
                           ? L""
                           : std::to_wstring(context.cooldown->max_charges));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return context.cooldown != nullptr &&
                     bounded_decimal(
                         window_text(field_control(context, IDC_COOLDOWN_CHARGES)),
                         layerc::core::min_cooldown_charges,
                         layerc::core::max_cooldown_charges);
            }},
    ProfileFieldBinding{
        .control_id = IDC_COOLDOWN_RETRIGGER,
        .notification = CBN_SELCHANGE,
        .owner = ProfileFieldOwner::cooldown,
        .change = ProfileFieldChange::immediate,
        .refresh_cooldown_label = false,
        .invalidate_preview = false,
        .read =
            +[](ProfileFieldContext &context) {
              const auto selection = SendMessageW(
                  field_control(context, IDC_COOLDOWN_RETRIGGER), CB_GETCURSEL, 0, 0);
              if (selection >= 0 && selection <= 1)
                context.cooldown->retrigger =
                    static_cast<layerc::core::RetriggerPolicy>(selection);
            },
        .write =
            +[](const ProfileFieldView &context) {
              SendMessageW(field_control(context, IDC_COOLDOWN_RETRIGGER),
                           CB_SETCURSEL,
                           context.cooldown == nullptr
                               ? static_cast<WPARAM>(-1)
                               : static_cast<WPARAM>(context.cooldown->retrigger),
                           0);
            },
        .valid = +[](const ProfileFieldView &) { return true; }},
    ProfileFieldBinding{
        .control_id = IDC_COOLDOWN_RECOVERY,
        .notification = CBN_SELCHANGE,
        .owner = ProfileFieldOwner::cooldown,
        .change = ProfileFieldChange::immediate,
        .refresh_cooldown_label = false,
        .invalidate_preview = false,
        .read =
            +[](ProfileFieldContext &context) {
              const auto selection = SendMessageW(
                  field_control(context, IDC_COOLDOWN_RECOVERY), CB_GETCURSEL, 0, 0);
              if (selection >= 0 && selection <= 2)
                context.cooldown->recovery =
                    static_cast<layerc::core::RecoveryMode>(selection);
            },
        .write =
            +[](const ProfileFieldView &context) {
              SendMessageW(field_control(context, IDC_COOLDOWN_RECOVERY),
                           CB_SETCURSEL,
                           context.cooldown == nullptr
                               ? static_cast<WPARAM>(-1)
                               : static_cast<WPARAM>(context.cooldown->recovery),
                           0);
            },
        .valid = +[](const ProfileFieldView &) { return true; }},
    ProfileFieldBinding{
        .control_id = IDC_BAR_WIDTH,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::panel,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.panel->bar.width = parse_uint(
                  window_text(field_control(context, IDC_BAR_WIDTH)),
                  context.panel->bar.width);
            },
        .write =
            +[](const ProfileFieldView &context) {
              if (context.panel != nullptr)
                set_text(field_control(context, IDC_BAR_WIDTH),
                         std::to_wstring(context.panel->bar.width));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return context.panel == nullptr ||
                     bounded_decimal(
                         window_text(field_control(context, IDC_BAR_WIDTH)),
                         layerc::core::min_bar_width, layerc::core::max_bar_width);
            }},
    ProfileFieldBinding{
        .control_id = IDC_BAR_HEIGHT,
        .notification = EN_CHANGE,
        .owner = ProfileFieldOwner::panel,
        .change = ProfileFieldChange::text,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.panel->bar.height = parse_uint(
                  window_text(field_control(context, IDC_BAR_HEIGHT)),
                  context.panel->bar.height);
            },
        .write =
            +[](const ProfileFieldView &context) {
              if (context.panel != nullptr)
                set_text(field_control(context, IDC_BAR_HEIGHT),
                         std::to_wstring(context.panel->bar.height));
            },
        .valid =
            +[](const ProfileFieldView &context) {
              return context.panel == nullptr ||
                     bounded_decimal(
                         window_text(field_control(context, IDC_BAR_HEIGHT)),
                         layerc::core::min_bar_height,
                         layerc::core::max_bar_height);
            }},
    ProfileFieldBinding{
        .control_id = IDC_SHOW_TEXT,
        .notification = BN_CLICKED,
        .owner = ProfileFieldOwner::panel,
        .change = ProfileFieldChange::immediate,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.panel->bar.show_text =
                  owner_draw_checkbox_checked(field_control(context, IDC_SHOW_TEXT));
            },
        .write =
            +[](const ProfileFieldView &context) {
              if (context.panel != nullptr)
                set_owner_draw_checkbox(field_control(context, IDC_SHOW_TEXT),
                                        context.panel->bar.show_text);
            },
        .valid = +[](const ProfileFieldView &) { return true; }},
    ProfileFieldBinding{
        .control_id = IDC_HIDE_READY,
        .notification = BN_CLICKED,
        .owner = ProfileFieldOwner::panel,
        .change = ProfileFieldChange::immediate,
        .refresh_cooldown_label = false,
        .invalidate_preview = true,
        .read =
            +[](ProfileFieldContext &context) {
              context.panel->bar.hide_when_ready =
                  owner_draw_checkbox_checked(field_control(context, IDC_HIDE_READY));
            },
        .write =
            +[](const ProfileFieldView &context) {
              if (context.panel != nullptr)
                set_owner_draw_checkbox(field_control(context, IDC_HIDE_READY),
                                        context.panel->bar.hide_when_ready);
            },
        .valid = +[](const ProfileFieldView &) { return true; }},
};

} // namespace

std::wstring
duration_seconds_text(const layerc::core::Milliseconds duration) {
  const auto milliseconds = duration.count();
  std::wstring result = std::to_wstring(milliseconds / 1000);
  const auto remainder = milliseconds % 1000;
  if (remainder != 0) {
    wchar_t fraction[5]{};
    swprintf_s(fraction, L".%03lld", static_cast<long long>(remainder));
    result += fraction;
    while (result.back() == L'0') result.pop_back();
  }
  return result;
}

std::span<const ProfileFieldBinding> profile_field_bindings() {
  return kProfileFields;
}

const ProfileFieldBinding *profile_field_binding(const int control_id) {
  const auto found =
      std::ranges::find(kProfileFields, control_id,
                        &ProfileFieldBinding::control_id);
  return found == kProfileFields.end() ? nullptr : &*found;
}

void read_profile_fields(ProfileFieldContext &context) {
  // A control that does not exist never overwrites a value (headless hosts).
  for (const auto &binding : kProfileFields)
    if (field_owner_available(context, binding.owner) &&
        GetDlgItem(context.dialog, binding.control_id) != nullptr)
      binding.read(context);
}

void write_profile_fields(const ProfileFieldView &context,
                          const ProfileFieldOwner owner) {
  for (const auto &binding : kProfileFields)
    if (binding.owner == owner) binding.write(context);
}

bool profile_fields_valid(const ProfileFieldView &context) {
  // A control that does not exist holds no buffer, so it cannot be invalid.
  return std::ranges::all_of(kProfileFields, [&](const auto &binding) {
    return GetDlgItem(context.dialog, binding.control_id) == nullptr ||
           binding.valid(context);
  });
}

} // namespace layerc::editor
