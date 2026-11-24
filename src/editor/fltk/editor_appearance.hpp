#pragma once

#include "appearance_floor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::editor {

// LayerE's own look on Linux, stored beside the profiles in XDG config. The
// presets match Windows: one neutral dark ramp, differing only in accent;
// Graphite / Copper is the default, as on Windows. "System" keeps FLTK's
// default colours. Per-role overrides cover what FLTK draws per widget: fill,
// text and selection colour.
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
  EditorTheme theme{EditorTheme::copper};
  std::optional<std::uint32_t> accent; // nullopt = the preset accent; zero = explicit black
  std::array<RoleOverride, editor_role_count> overrides{};
  bool operator==(const EditorAppearance &) const = default;
};

struct RoleColors {
  std::uint32_t fill{};
  std::uint32_t text{};
  std::uint32_t selection{};
  bool operator==(const RoleColors &) const = default;
};

// Every colour the Linux editor draws with, in one place (DesignSystem.md,
// "Where each color may go"). The neutral values are the Windows neutral
// theme, so a preset renders alike on both editors; System keeps FLTK's own
// palette and resolves from it instead.
struct EditorTokens {
  std::uint32_t background{}, panel{}, input{}, border{}, divider{}, text{};
  std::uint32_t secondary{}, subtext{}, verbose{}, disabled{};
  std::uint32_t button_hover{}, row_hover{}, pressed{};
  std::uint32_t accent{}, selection{}, on_accent{};
  std::uint32_t yes{}, no{}, warning{}, accent_2{}, on_status{};
  bool operator==(const EditorTokens &) const = default;
};
enum class FixedInk : std::uint8_t { secondary, subtext };
[[nodiscard]] std::uint32_t fixed_ink(const EditorTokens &tokens, FixedInk ink) noexcept;
[[nodiscard]] EditorTokens editor_tokens(const EditorAppearance &appearance) noexcept;
// Body text size and this app's scale, from the desktop's pixel size: the
// scale leaves the base size alone when the desktop asks for more than 14 px.
struct DesktopTextMetrics {
  int normal_size{};
  float scale{};
  bool operator==(const DesktopTextMetrics &) const = default;
};
[[nodiscard]] DesktopTextMetrics desktop_text_metrics(std::optional<int> pixels) noexcept;

[[nodiscard]] std::string_view theme_name(EditorTheme theme) noexcept;  // "copper"
[[nodiscard]] std::string_view theme_label(EditorTheme theme) noexcept; // "Graphite / Copper"
[[nodiscard]] std::string_view role_name(EditorRole role) noexcept;     // "edit"
[[nodiscard]] std::string_view role_label(EditorRole role) noexcept;    // "Text fields"
[[nodiscard]] std::uint32_t preset_accent(EditorTheme theme) noexcept;
// The preset colours of a role, with the role's overrides applied; nullopt
// for the system theme, whose base colours are FLTK's own.
[[nodiscard]] std::optional<RoleColors> theme_role_colors(const EditorAppearance &appearance,
                                                          EditorRole role);

// The first role, in EditorRole order, whose colours fall below an
// Appearance floor (DesignSystem.md, Roles); Apply refuses while one does.
// FLTK measures Text / Fill on every role and Text / Selection where the
// selection sits behind text (pressed button, selected list and profile-list
// row). Presets are measured with their overrides. The System theme measures
// `system_roles` (FLTK's own colours) with the overrides applied, and only
// pairs with at least one overridden colour: untouched desktop colours never
// refuse.
enum class PaintedGround : std::uint8_t { own_fill, beneath };
enum class PaintedPurpose : std::uint8_t {
  empty_list_title, empty_list_hint, disabled_reason, options_hint
};
struct PaintedText {
  FixedInk ink;
  PaintedGround ground;
  PaintedPurpose purpose;
  bool dimmed_variant;
};
class PaintedTextSource {
public:
  [[nodiscard]] virtual std::span<const PaintedText> painted_text() const noexcept = 0;
protected:
  ~PaintedTextSource() = default;
};

enum class SurfacePart : std::uint8_t { fill, selection };
struct RoleContrastFailure {
  EditorRole role{};
  ContrastPairKind pair{};
  double ratio{};
  // The role whose colour the text is painted on, for mixed roles or fixed ink.
  std::optional<EditorRole> ground;
  SurfacePart ground_part{SurfacePart::fill};
  bool primary_button{};
  std::optional<PaintedPurpose> purpose{};
  bool dimmed{};
  bool operator==(const RoleContrastFailure &) const = default;
};
[[nodiscard]] std::optional<RoleContrastFailure>
first_role_contrast_failure(const EditorAppearance &appearance,
                            const std::array<RoleColors, editor_role_count> &system_roles);

// Where FLTK paints a role's text, which is not always the role's own fill:
// a label or check box without a box draws on the surface beneath, an
// outside label in the Labels text on its parent, a tab label on a tab.
// `input_field` picks the System colours of an input field over those of a
// choice; it only matters for the edit role.
struct PaintedSurface {
  EditorRole role{};
  bool input_field{};
  SurfacePart part{SurfacePart::fill};
  bool operator==(const PaintedSurface &) const = default;
};
// `verbose` is the mono diagnostics ink, the verbose token until the text
// role's own text is overridden.
enum class TextInk : std::uint8_t { role_text, verbose };
struct PaintedTextPair {
  EditorRole text{};
  TextInk ink{TextInk::role_text};
  PaintedSurface ground{};
  std::optional<FixedInk> fixed{};
  std::optional<PaintedPurpose> purpose{};
  bool dimmed{};
  bool operator==(const PaintedTextPair &) const = default;
};
// FLTK's own colours under the System theme: each role as an input field
// paints it and as any other control does, and the System verbose ink.
struct SystemRoleColors {
  std::array<RoleColors, editor_role_count> field{};
  std::array<RoleColors, editor_role_count> control{};
  std::uint32_t verbose{};
  std::uint32_t foreground{};
  bool operator==(const SystemRoleColors &) const = default;
};
// The nominal pairs above (the System edit role measured as a field and as a
// choice), then each painted pair: the text role's colour on the ground
// role's fill or selection. A preset measures every painted pair; System
// only those whose text or used ground part is overridden.
[[nodiscard]] std::optional<RoleContrastFailure>
first_role_contrast_failure(const EditorAppearance &appearance, const SystemRoleColors &system,
                            std::span<const PaintedTextPair> painted);
// "Lists: Text / Fill contrast is 1.0:1; at least 4.5:1 is required.", and
// for a painted pair "Labels: Text / Fill contrast is 1.3:1 on the Window
// fill; at least 4.5:1 is required."
[[nodiscard]] std::string role_contrast_failure_text(const RoleContrastFailure &failure);
[[nodiscard]] std::uint32_t fltk_inactive_rgb(std::uint32_t fill, std::uint32_t partner) noexcept;

// The 1 px outline of a flat control filled with `fill`: a third of the way
// toward white on dark fills and toward black on light ones.
[[nodiscard]] std::uint32_t control_border(std::uint32_t fill) noexcept;

// The desktop's interface font, from a Pango-style description such as
// "Cantarell 11" or "Noto Sans, Medium 10": the first family without style
// words, and the point size in pixels at 96 DPI when one is given.
struct DesktopFont {
  std::string family;
  std::optional<int> pixels;
  bool operator==(const DesktopFont &) const = default;
};
[[nodiscard]] std::optional<DesktopFont> parse_desktop_font(std::string_view description);
// The string setting `name` (e.g. "Gtk/FontName") in an XSETTINGS
// _XSETTINGS_SETTINGS property; nullopt when absent or malformed.
[[nodiscard]] std::optional<std::string> xsettings_string(std::span<const unsigned char> data,
                                                          std::string_view name);

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
