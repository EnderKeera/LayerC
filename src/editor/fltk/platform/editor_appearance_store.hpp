#pragma once

#include "../editor_appearance.hpp"

namespace layerc::editor {

struct AppearanceSaveFailure {
  std::string text;
  bool unreadable{};
};

[[nodiscard]] AppearanceLoad load_platform_appearance();
// On Windows, replacing an unreadable saved record requires consent.
// A missing record is not a failure.
[[nodiscard]] std::optional<AppearanceSaveFailure>
save_platform_appearance(const EditorAppearance &appearance, bool replace_unreadable);
[[nodiscard]] const char *appearance_replacement_prompt() noexcept;
// Windows: the first floor the Win32 editor would refuse once `appearance`
// is merged into the shared record it also paints, with the border, hover
// and disabled parts FLTK keeps but cannot edit (layer55 F-01). Untouched
// System roles never refuse, and an unreadable record is left to the save to
// report. Linux has no shared record: nullopt.
[[nodiscard]] std::optional<RoleContrastFailure>
shared_appearance_failure(const EditorAppearance &appearance);

} // namespace layerc::editor
