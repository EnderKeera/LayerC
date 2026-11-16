#include "editor_appearance.hpp"
#include "editor_color.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/durable_file.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {
namespace json = layerc::core::json;

constexpr std::size_t max_appearance_bytes = 64 * 1024;
constexpr std::int64_t appearance_version = 1;

// The Windows neutral dark ramp (layerc::platform::neutral_theme) and the
// preset accents, named once so no UI file writes a hex colour of its own.
constexpr std::uint32_t copper_accent = 0xD88E4B;
constexpr std::uint32_t teal_accent = 0x5AB6AA;
constexpr std::uint32_t plum_accent = 0xB382E2;
constexpr std::uint32_t slate_accent = 0x8FA3B8;

constexpr EditorTokens neutral_tokens{
    .background = 0x1F1F1F,
    .panel = 0x2D2D2D,
    .input = 0x171717,
    .border = 0x6E6E6E,
    .divider = 0x333333,
    .text = 0xF3F3F3,
    .secondary = 0xB8B8B8,
    .subtext = 0xA3A3A3,
    .verbose = 0x9A9A9A,
    .disabled = 0x808080,
    .button_hover = 0x383838,
    .row_hover = 0x2A2A2A,
    .pressed = 0x262626,
    .accent = copper_accent,
    .selection = 0x2A2A2A,
    .on_accent = 0x141414,
    .yes = 0x00E676,
    .no = 0xFF5A5A,
    .warning = 0xFFD740,
    .accent_2 = accent_2_blue,
    .on_status = 0x141414,
};

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
  case EditorTheme::teal: return teal_accent;
  case EditorTheme::plum: return plum_accent;
  case EditorTheme::dark: return slate_accent;
  default: return copper_accent;
  }
}

EditorTokens editor_tokens(const EditorAppearance &appearance) noexcept {
  const auto accent = appearance.accent.value_or(preset_accent(appearance.theme));
  auto tokens = neutral_tokens;
  tokens.accent = accent;
  // About a quarter of the accent over the window, as on Windows.
  tokens.selection = blend(neutral_tokens.background, accent, 60);
  tokens.on_accent = rgb_on_accent(accent);
  tokens.accent_2 = rgb_accent_2(accent);
  return tokens;
}

DesktopTextMetrics desktop_text_metrics(const std::optional<int> pixels) noexcept {
  const int size = std::max(11, pixels.value_or(14));
  return {std::min(size, 14), std::max(1.0F, static_cast<float>(size) / 14.0F)};
}

std::optional<RoleColors> theme_role_colors(const EditorAppearance &appearance,
                                            const EditorRole role) {
  if (appearance.theme == EditorTheme::system) return std::nullopt;
  const auto tokens = editor_tokens(appearance);
  RoleColors colors{.fill = tokens.background, .text = tokens.text,
                    .selection = tokens.selection};
  switch (role) {
  case EditorRole::sidebar:
  case EditorRole::edit:
  case EditorRole::list:
  case EditorRole::preview: colors.fill = tokens.input; break;
  case EditorRole::button: colors.fill = tokens.panel; break;
  case EditorRole::label:
  case EditorRole::status: colors.text = tokens.secondary; break;
  // FLTK draws the check mark and a light button's lamp in the selection
  // colour: the full accent, like the filled Windows check box.
  case EditorRole::checkbox: colors.selection = tokens.accent; break;
  default: break;
  }
  const auto &override_ = appearance.overrides[static_cast<std::size_t>(role)];
  if (override_.fill) colors.fill = *override_.fill;
  if (override_.text) colors.text = *override_.text;
  if (override_.selection) colors.selection = *override_.selection;
  return colors;
}

std::uint32_t control_border(const std::uint32_t fill) noexcept {
  const auto luma = (((fill >> 16U) & 0xFFU) * 299U + ((fill >> 8U) & 0xFFU) * 587U +
                     (fill & 0xFFU) * 114U) / 1000U;
  return blend(fill, luma < 128U ? 0xFFFFFFU : 0x000000U, 91);
}

std::optional<DesktopFont> parse_desktop_font(const std::string_view description) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : description) {
    if (c == ' ' || c == '\t') {
      if (!word.empty()) words.push_back(std::exchange(word, {}));
    } else {
      word += c;
    }
  }
  if (!word.empty()) words.push_back(std::move(word));
  if (words.empty()) return std::nullopt;
  DesktopFont font;
  // A trailing number is the size: points, or pixels with a "px" suffix.
  std::string_view size = words.back();
  const bool pixel_size = size.ends_with("px");
  if (pixel_size) size.remove_suffix(2);
  double value{};
  const auto [end, error] = std::from_chars(size.data(), size.data() + size.size(), value);
  if (error == std::errc{} && end == size.data() + size.size() && !size.empty()) {
    words.pop_back();
    if (value > 0.0 && value <= 200.0)
      font.pixels = static_cast<int>(std::lround(pixel_size ? value : value * 96.0 / 72.0));
  }
  std::string rest;
  for (const auto &item : words) rest += (rest.empty() ? "" : " ") + item;
  // "Family A, Family B, Style": the first family; styles follow the list.
  if (const auto comma = rest.find(','); comma != std::string::npos) {
    rest.resize(comma);
  } else {
    static constexpr std::array<std::string_view, 34> styles{
        "normal", "roman", "oblique", "italic", "small-caps", "thin", "ultra-light",
        "extra-light", "light", "semi-light", "demi-light", "book", "regular", "medium",
        "semi-bold", "demi-bold", "bold", "ultra-bold", "extra-bold", "heavy", "black",
        "ultra-black", "extra-black", "ultra-heavy", "ultra-condensed", "extra-condensed",
        "condensed", "semi-condensed", "semi-expanded", "expanded", "extra-expanded",
        "ultra-expanded", "not-rotated", "upside-down"};
    const auto is_style = [](std::string text) {
      for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return std::ranges::find(styles, text) != styles.end();
    };
    while (words.size() > 1 && is_style(words.back())) words.pop_back();
    rest.clear();
    for (const auto &item : words) rest += (rest.empty() ? "" : " ") + item;
  }
  while (!rest.empty() && rest.back() == ' ') rest.pop_back();
  if (rest.empty()) return std::nullopt;
  font.family = std::move(rest);
  return font;
}

std::optional<std::string> xsettings_string(const std::span<const unsigned char> data,
                                            const std::string_view name) {
  // XSETTINGS: byte order, 3 pad, serial, count; then per setting a type, a
  // pad, a 16-bit name length, the name (padded to 4), a serial and a value.
  if (data.size() < 12 || data[0] > 1) return std::nullopt;
  const bool msb_first = data[0] == 1;
  const auto read = [&](const std::size_t at, const std::size_t bytes) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < bytes; ++index) {
      const std::uint32_t byte = data[at + (msb_first ? index : bytes - 1 - index)];
      value = (value << 8U) | byte;
    }
    return value;
  };
  const auto padded = [](const std::size_t length) { return (length + 3U) & ~std::size_t{3}; };
  const auto count = read(8, 4);
  std::size_t at = 12;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (data.size() - at < 4) return std::nullopt;
    const auto type = data[at];
    const std::size_t name_length = read(at + 2, 2);
    at += 4;
    if (data.size() - at < padded(name_length) + 4) return std::nullopt;
    const std::string_view setting(reinterpret_cast<const char *>(data.data() + at), name_length);
    at += padded(name_length) + 4;
    if (type == 0) { // integer
      if (data.size() - at < 4) return std::nullopt;
      at += 4;
    } else if (type == 1) { // string
      if (data.size() - at < 4) return std::nullopt;
      const std::size_t length = read(at, 4);
      at += 4;
      if (data.size() - at < padded(length)) return std::nullopt;
      if (setting == name)
        return std::string(reinterpret_cast<const char *>(data.data() + at), length);
      at += padded(length);
    } else if (type == 2) { // colour
      if (data.size() - at < 8) return std::nullopt;
      at += 8;
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}


std::string appearance_json(const EditorAppearance &appearance) {
  std::string out = "{\"appearanceVersion\":";
  json::append_unsigned(out, static_cast<std::uint64_t>(appearance_version));
  out += ",\"theme\":";
  json::append_quoted(out, theme_name(appearance.theme));
  if (appearance.accent) {
    out += ",\"accent\":";
    json::append_color_quoted(out, *appearance.accent);
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
