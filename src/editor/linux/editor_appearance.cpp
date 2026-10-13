#include "editor_appearance.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include <algorithm>
#include <span>
#include <system_error>

namespace layerc::editor {
namespace {
namespace json = layerc::core::json;

constexpr std::size_t max_appearance_bytes = 64 * 1024;
constexpr std::int64_t appearance_version = 1;

// The Windows neutral dark ramp (layerc::platform::neutral_theme) and preset
// accents, so both editors render a preset alike.
constexpr std::uint32_t ramp_background = 0x1F1F1F;
constexpr std::uint32_t ramp_panel = 0x2D2D2D;
constexpr std::uint32_t ramp_input = 0x171717;
constexpr std::uint32_t ramp_text = 0xF3F3F3;
constexpr std::uint32_t ramp_secondary = 0xB8B8B8;

constexpr std::array<std::string_view, static_cast<std::size_t>(EditorTheme::count)> theme_names{
    "copper", "teal", "plum", "system", "dark"};
constexpr std::array<std::string_view, static_cast<std::size_t>(EditorTheme::count)> theme_labels{
    "Graphite / Copper", "Graphite / Teal", "Graphite / Plum", "System (FLTK colours)",
    "Graphite / Slate"};
constexpr std::array<std::string_view, editor_role_count> role_names{
    "window", "toolbar", "sidebar", "label", "button", "edit", "list", "checkbox", "preview",
    "status"};
constexpr std::array<std::string_view, editor_role_count> role_labels{
    "Window", "Menu bar", "Profile list", "Labels", "Buttons", "Text fields", "Lists",
    "Check boxes", "Preview", "Status text"};

std::uint32_t blend(const std::uint32_t base, const std::uint32_t over, const unsigned amount) {
  std::uint32_t out{};
  for (unsigned shift = 0; shift <= 16; shift += 8) {
    const auto from = (base >> shift) & 0xFFU;
    const auto to = (over >> shift) & 0xFFU;
    const auto mixed = from <= to ? from + (to - from) * amount / 255U
                                  : from - (from - to) * amount / 255U;
    out |= (mixed & 0xFFU) << shift;
  }
  return out;
}

std::optional<std::uint32_t> color_field(const json::Value &object, const std::string_view key,
                                         std::string &error) {
  const auto *value = object.find(key);
  if (!value) return std::nullopt;
  const auto color = value->is_string() ? json::parse_color_text(value->as_string()) : std::nullopt;
  if (!color) error = std::string(key) + " must be a #RRGGBB colour";
  return color;
}

void append_color_member(std::string &out, bool &first, const std::string_view key,
                         const std::optional<std::uint32_t> &color) {
  if (!color) return;
  out += first ? "" : ",";
  first = false;
  json::append_quoted(out, key);
  out += ':';
  json::append_color_quoted(out, *color);
}
} // namespace

std::string_view theme_name(const EditorTheme theme) noexcept {
  const auto index = static_cast<std::size_t>(theme);
  return index < theme_names.size() ? theme_names[index] : std::string_view{};
}
std::string_view theme_label(const EditorTheme theme) noexcept {
  const auto index = static_cast<std::size_t>(theme);
  return index < theme_labels.size() ? theme_labels[index] : std::string_view{};
}
std::string_view role_name(const EditorRole role) noexcept {
  const auto index = static_cast<std::size_t>(role);
  return index < role_names.size() ? role_names[index] : std::string_view{};
}
std::string_view role_label(const EditorRole role) noexcept {
  const auto index = static_cast<std::size_t>(role);
  return index < role_labels.size() ? role_labels[index] : std::string_view{};
}

std::uint32_t preset_accent(const EditorTheme theme) noexcept {
  switch (theme) {
  case EditorTheme::teal: return 0x5AB6AA;
  case EditorTheme::plum: return 0xB382E2;
  case EditorTheme::dark: return 0x8FA3B8;
  default: return 0xD88E4B;
  }
}

std::optional<RoleColors> theme_role_colors(const EditorAppearance &appearance,
                                            const EditorRole role) {
  if (appearance.theme == EditorTheme::system) return std::nullopt;
  const auto accent = appearance.accent != 0 ? appearance.accent : preset_accent(appearance.theme);
  RoleColors colors{.fill = ramp_background, .text = ramp_text,
                    // About a quarter of the accent over the window, as on Windows.
                    .selection = blend(ramp_background, accent, 60)};
  switch (role) {
  case EditorRole::sidebar:
  case EditorRole::edit:
  case EditorRole::list:
  case EditorRole::preview: colors.fill = ramp_input; break;
  case EditorRole::button: colors.fill = ramp_panel; break;
  case EditorRole::label:
  case EditorRole::status: colors.text = ramp_secondary; break;
  default: break;
  }
  const auto &override_ = appearance.overrides[static_cast<std::size_t>(role)];
  if (override_.fill) colors.fill = *override_.fill;
  if (override_.text) colors.text = *override_.text;
  if (override_.selection) colors.selection = *override_.selection;
  return colors;
}

std::filesystem::path editor_appearance_path() {
  const auto roots = layerc::platform::profile_store_roots();
  if (roots.config_profiles.empty()) return {};
  return roots.config_profiles.parent_path() / "editor-appearance.json";
}

std::string appearance_json(const EditorAppearance &appearance) {
  std::string out = "{\"appearanceVersion\":";
  json::append_unsigned(out, static_cast<std::uint64_t>(appearance_version));
  out += ",\"theme\":";
  json::append_quoted(out, theme_name(appearance.theme));
  if (appearance.accent != 0) {
    out += ",\"accent\":";
    json::append_color_quoted(out, appearance.accent);
  }
  // Only changed roles are written; with none, "overrides" is omitted.
  bool first_role = true;
  for (std::size_t index = 0; index < editor_role_count; ++index) {
    const auto &item = appearance.overrides[index];
    if (!item.fill && !item.text && !item.selection) continue;
    out += first_role ? ",\"overrides\":{" : ",";
    first_role = false;
    json::append_quoted(out, role_names[index]);
    out += ":{";
    bool first = true;
    append_color_member(out, first, "fill", item.fill);
    append_color_member(out, first, "text", item.text);
    append_color_member(out, first, "selection", item.selection);
    out += '}';
  }
  if (!first_role) out += '}';
  out += "}\n";
  return out;
}

std::variant<EditorAppearance, std::string> parse_appearance(const std::string_view text) {
  if (text.size() > max_appearance_bytes) return std::string("the appearance file is too large");
  const auto parsed = json::parse(text);
  if (const auto *failure = std::get_if<json::ParseError>(&parsed))
    return "invalid JSON at byte " + std::to_string(failure->offset) + ": " + failure->message;
  const auto &root = std::get<json::Value>(parsed);
  if (!root.is_object()) return std::string("the appearance file is not a JSON object");
  for (const auto &[key, value] : root.as_object()) {
    (void)value;
    if (key != "appearanceVersion" && key != "theme" && key != "accent" && key != "overrides")
      return "unknown appearance field \"" + key + "\"";
  }
  const auto *version = root.find("appearanceVersion");
  if (!version || !version->is_integer() || version->as_integer() != appearance_version)
    return std::string("unsupported appearanceVersion");
  EditorAppearance appearance;
  const auto *theme = root.find("theme");
  const auto found = theme && theme->is_string()
      ? std::ranges::find(theme_names, theme->as_string()) : theme_names.end();
  if (found == theme_names.end()) return std::string("unknown theme");
  appearance.theme = static_cast<EditorTheme>(found - theme_names.begin());
  std::string error;
  if (const auto accent = color_field(root, "accent", error)) appearance.accent = *accent;
  if (!error.empty()) return error;
  if (const auto *overrides = root.find("overrides")) {
    if (!overrides->is_object()) return std::string("overrides must be an object");
    for (const auto &[key, value] : overrides->as_object()) {
      const auto role = std::ranges::find(role_names, key);
      if (role == role_names.end()) return "unknown override role \"" + key + "\"";
      if (!value.is_object()) return "override " + key + " must be an object";
      for (const auto &[field, ignored] : value.as_object()) {
        (void)ignored;
        if (field != "fill" && field != "text" && field != "selection")
          return "unknown override colour \"" + field + "\" for " + key;
      }
      auto &item = appearance.overrides[static_cast<std::size_t>(role - role_names.begin())];
      item.fill = color_field(value, "fill", error);
      item.text = color_field(value, "text", error);
      item.selection = color_field(value, "selection", error);
      if (!error.empty()) return key + ": " + error;
    }
  }
  return appearance;
}

AppearanceLoad load_editor_appearance(const std::filesystem::path &path) {
  if (path.empty()) return {{}, "No configuration directory: HOME is not an absolute path."};
  std::error_code code;
  if (!std::filesystem::exists(path, code) && !code) return {};
  const auto text = layerc::platform::read_durable_text(path, max_appearance_bytes);
  if (!text) return {{}, "The editor appearance file could not be read; defaults are in use."};
  auto parsed = parse_appearance(*text);
  if (const auto *error = std::get_if<std::string>(&parsed))
    return {{}, "The editor appearance file is invalid (" + *error + "); defaults are in use."};
  return {std::get<EditorAppearance>(std::move(parsed)), {}};
}

std::optional<std::string> save_editor_appearance(const std::filesystem::path &path,
                                                  const EditorAppearance &appearance) {
  if (path.empty()) return std::string("No configuration directory: HOME is not an absolute path.");
  std::error_code code;
  std::filesystem::create_directories(path.parent_path(), code);
  if (code || !layerc::platform::flush_directory_hierarchy(path.parent_path()))
    return "Appearance not saved: cannot prepare " + path.parent_path().string();
  const auto text = appearance_json(appearance);
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  if (layerc::platform::write_durable_file(path, bytes) ==
      layerc::platform::DurablePublishOutcome::failed)
    return "Appearance not saved: writing " + path.string() + " failed.";
  return std::nullopt;
}

} // namespace layerc::editor
