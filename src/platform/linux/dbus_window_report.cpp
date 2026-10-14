#include "dbus_window_report.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace layerc::platform {
namespace {

namespace json = layerc::core::json;

constexpr std::int64_t coordinate_limit = std::numeric_limits<std::int32_t>::max();

[[nodiscard]] WindowReportError invalid(std::string detail) {
  return {"window_report_invalid", std::move(detail)};
}

[[nodiscard]] int hex_digit(const char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return 0;
}

} // namespace

std::string_view window_report_source_name(const WindowReportSource source) noexcept {
  return source == WindowReportSource::kwin ? "kwin" : "gnome";
}

std::uint64_t window_report_id(const WindowReportSource source, const std::string_view id) {
  std::string key{window_report_source_name(source)};
  key.push_back('\0');
  key.append(id);
  const auto digest = layerc::core::sha256(std::as_bytes(std::span{key.data(), key.size()}));
  std::uint64_t value{};
  for (std::size_t index = 0; index < 16 && index < digest.size(); ++index)
    value = (value << 4U) | static_cast<std::uint64_t>(hex_digit(digest[index]));
  // Zero is the "no window" sentinel everywhere downstream.
  return value == 0 ? 1 : value;
}

std::variant<WindowScan, WindowReportError>
parse_window_report(const std::string_view payload, const WindowReportSource source) {
  if (payload.size() > window_report_max_bytes)
    return WindowReportError{"window_report_too_large",
                             std::to_string(payload.size()) + " bytes"};
  auto parsed = json::parse_bounded(payload, window_report_max_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return invalid(error->describe());
  const auto &root = std::get<layerc::core::json::Value>(parsed);
  if (!root.is_object()) return invalid("$: expected object");

  json::Reader reader{json::Required::yes};
  const auto version = reader.integer(root, "version", "$", 1, 1);
  std::string named;
  reader.string(root, "source", "$", named);
  const auto *windows = reader.array(root, "windows", "$");
  if (!reader.ok() || !version || !windows) return invalid(reader.error().describe());
  if (named != window_report_source_name(source))
    return WindowReportError{"window_report_source_mismatch", "$.source: " + named};

  WindowScan scan;
  scan.authoritative = true;
  std::set<std::string> ids;
  const auto text = [&](const json::Value &object, const char *key, const std::string &path,
                        std::string &output, const std::size_t maximum,
                        const json::Required required) {
    if (!reader.string(object, key, path, output, required,
                       required == json::Required::yes ? json::EmptyString::reject
                                                       : json::EmptyString::allow,
                       maximum))
      return;
    if (!layerc::core::is_valid_utf8(output))
      reader.fail(path + "." + key, "expected valid UTF-8");
  };
  reader.each(*windows, "$.windows", [&](const json::Value &item, const std::string &path) {
    if (!item.is_object()) {
      reader.fail(path, "expected object");
      return;
    }
    std::string id;
    text(item, "id", path, id, window_report_max_id, json::Required::yes);
    WindowSnapshot window;
    text(item, "title", path, window.title, window_report_max_string, json::Required::no);
    text(item, "class", path, window.class_name, window_report_max_string, json::Required::no);
    text(item, "instance", path, window.instance_name, window_report_max_string,
         json::Required::no);
    const auto number = [&](const char *key, const std::int64_t minimum) {
      return reader.integer(item, key, path, minimum, coordinate_limit, json::Required::no);
    };
    const auto x = number("x", -coordinate_limit);
    const auto y = number("y", -coordinate_limit);
    const auto width = number("width", 0);
    const auto height = number("height", 0);
    reader.boolean(item, "active", path, window.active, json::Required::no);
    reader.boolean(item, "fullscreen", path, window.fullscreen, json::Required::no);
    reader.boolean(item, "hidden", path, window.hidden, json::Required::no);
    const auto *pid = reader.field(item, "pid", path, json::Required::no);
    const auto pid_value = reader.unsigned_integer(
        item, "pid", path, std::numeric_limits<std::uint32_t>::max(), json::Required::no);
    const auto *xid = reader.field(item, "x11Window", path, json::Required::no);
    const auto xid_value = reader.unsigned_integer(
        item, "x11Window", path, std::numeric_limits<std::uint32_t>::max(), json::Required::no);
    if (!reader.ok() || !x || !y || !width || !height) return;
    if (!ids.insert(id).second) {
      reader.fail(path + ".id", "duplicate window id");
      return;
    }
    if (scan.windows.size() >= window_scan_limit) {
      ++scan.truncated;
      return;
    }
    window.window = window_report_id(source, id);
    window.x = static_cast<int>(*x);
    window.y = static_cast<int>(*y);
    window.width = static_cast<int>(*width);
    window.height = static_cast<int>(*height);
    if (pid && pid_value && *pid_value != 0) {
      window.pid = static_cast<std::uint32_t>(*pid_value);
      window.executable_path = process_executable_path(*window.pid);
      window.executable_name = path_basename(window.executable_path);
    }
    if (xid && xid_value && *xid_value != 0) window.x11_window = *xid_value;
    if (window.active && !scan.active) scan.active = window.window;
    scan.windows.push_back(std::move(window));
  });
  if (!reader.ok()) return invalid(reader.error().describe());
  // Only one window can hold focus; later claims are not trusted.
  for (auto &window : scan.windows)
    window.active = scan.active && window.window == *scan.active;
  return scan;
}

} // namespace layerc::platform
