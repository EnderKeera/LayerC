#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::editor {

// LayerE's own look on Linux, stored beside the profiles in XDG config. The
// presets match Windows: one neutral dark ramp, differing only in accent.
// "System" keeps FLTK's default colours. Per-role overrides cover what FLTK
// draws per widget: fill, text and selection colour.
enum class EditorTheme : std::uint8_t { copper, teal, plum, system, dark, count };
enum class EditorRole : std::uint8_t {
  window, toolbar, sidebar, label, button, edit, list, checkbox, preview, status, count
};

inline constexpr std::size_t editor_role_count = static_cast<std::size_t>(EditorRole::count);

struct RoleOverride {
  std::optional<std::uint32_t> fill;
  std::optional<std::uint32_t> text;
  std::optional<std::uint32_t> selection;
  bool operator==(const RoleOverride &) const = default;
};

struct EditorAppearance {
  EditorTheme theme{EditorTheme::system};
  std::uint32_t accent{}; // 0 = the preset accent
  std::array<RoleOverride, editor_role_count> overrides{};
  bool operator==(const EditorAppearance &) const = default;
};

struct RoleColors {
  std::uint32_t fill{};
  std::uint32_t text{};
  std::uint32_t selection{};
  bool operator==(const RoleColors &) const = default;
};

[[nodiscard]] std::string_view theme_name(EditorTheme theme) noexcept;  // "copper"
[[nodiscard]] std::string_view theme_label(EditorTheme theme) noexcept; // "Graphite / Copper"
[[nodiscard]] std::string_view role_name(EditorRole role) noexcept;     // "edit"
[[nodiscard]] std::string_view role_label(EditorRole role) noexcept;    // "Text fields"
[[nodiscard]] std::uint32_t preset_accent(EditorTheme theme) noexcept;
// The preset colours of a role, with the role's overrides applied; nullopt
// for the system theme, whose base colours are FLTK's own.
[[nodiscard]] std::optional<RoleColors> theme_role_colors(const EditorAppearance &appearance,
                                                          EditorRole role);

// <config>/layerc/editor-appearance.json; empty without an absolute HOME.
[[nodiscard]] std::filesystem::path editor_appearance_path();
[[nodiscard]] std::string appearance_json(const EditorAppearance &appearance);
[[nodiscard]] std::variant<EditorAppearance, std::string> parse_appearance(std::string_view text);

struct AppearanceLoad {
  EditorAppearance appearance;
  std::string error; // set when a present file was unreadable or invalid
};
// A missing file is the default appearance without an error. An invalid
// file is reported and left untouched until the user applies a new choice.
[[nodiscard]] AppearanceLoad load_editor_appearance(const std::filesystem::path &path);
// Durable replace; the error text on failure.
[[nodiscard]] std::optional<std::string>
save_editor_appearance(const std::filesystem::path &path, const EditorAppearance &appearance);

} // namespace layerc::editor
