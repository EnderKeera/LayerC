// Pure bus-lane models: window report bounds, the GNOME overlay frame, portal
// restore-token persistence, and PipeWire buffer conversion. No bus, no
// PipeWire daemon, no desktop.
#include "dbus_window_report.hpp"
#include "gnome_bridge_frame.hpp"
#include "pipewire_frame.hpp"
#include "portal_tokens.hpp"

#include "test_support.hpp"

#include <unistd.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using layerc::test::expect;
using namespace layerc::platform;

[[nodiscard]] const WindowScan *scan_of(const std::variant<WindowScan, WindowReportError> &result) {
  return std::get_if<WindowScan>(&result);
}
[[nodiscard]] std::string reason_of(const std::variant<WindowScan, WindowReportError> &result) {
  const auto *error = std::get_if<WindowReportError>(&result);
  return error ? error->reason : std::string{};
}

void window_report_parses_full_snapshots() {
  const std::string pid = std::to_string(::getpid());
  const auto result = parse_window_report(
      R"({"version":1,"source":"kwin","extra":true,"windows":[)"
      R"({"id":"{a}","title":"Game","class":"game.exe","instance":"game","pid":)" + pid +
          R"(,"x":-10,"y":20,"width":2560,"height":1440,"active":true,"fullscreen":true,)"
          R"("hidden":false,"x11Window":79691780,"future":{"nested":[1]}},)"
          R"({"id":"b","active":true},{"id":"c","pid":0,"x11Window":0}]})",
      WindowReportSource::kwin);
  const auto *scan = scan_of(result);
  if (!expect(scan != nullptr, "a well-formed KWin report parses")) return;
  expect(scan->authoritative && scan->windows.size() == 3 && scan->truncated == 0,
         "every reported window is kept and the scan is authoritative");
  const auto &game = scan->windows[0];
  expect(game.window == window_report_id(WindowReportSource::kwin, "{a}") && game.window != 0 &&
             game.title == "Game" && game.class_name == "game.exe" && game.instance_name == "game" &&
             game.x == -10 && game.y == 20 && game.width == 2560 && game.height == 1440 &&
             game.fullscreen && !game.hidden && game.x11_window == 79691780U,
         "fields map onto the neutral snapshot");
  expect(game.pid == static_cast<std::uint32_t>(::getpid()) && !game.executable_path.empty() &&
             game.executable_name == path_basename(game.executable_path),
         "a same-UID pid resolves its executable like X11");
  expect(scan->active == game.window && game.active && !scan->windows[1].active,
         "only the first window claiming focus is active");
  expect(!scan->windows[2].pid && !scan->windows[2].x11_window,
         "zero pid and x11Window mean absent");
  expect(window_report_id(WindowReportSource::kwin, "{a}") !=
             window_report_id(WindowReportSource::gnome, "{a}"),
         "ids are namespaced by source");
}

void window_report_rejects_malformed_reports() {
  const auto reject = [](const std::string &payload, const char *reason, const char *what) {
    expect(reason_of(parse_window_report(payload, WindowReportSource::gnome)) == reason, what);
  };
  const auto window = [](const std::string &fields) {
    return R"({"version":1,"source":"gnome","windows":[{"id":"w")" + fields + "}]}";
  };
  reject(R"({"version":1,"source":"kwin","windows":[]})", "window_report_source_mismatch",
         "a report must name its caller's source");
  reject(R"({"version":2,"source":"gnome","windows":[]})", "window_report_invalid",
         "only version 1 is accepted");
  reject(R"({"version":1,"source":"gnome"})", "window_report_invalid", "windows is required");
  reject("[1]", "window_report_invalid", "the root must be an object");
  reject("{", "window_report_invalid", "syntax errors reject the report");
  reject(R"({"version":1,"source":"gnome","windows":[{"title":"x"}]})", "window_report_invalid",
         "every window needs an id");
  reject(window(R"(,"title":")" + std::string(513, 'a') + "\""), "window_report_invalid",
         "strings are bounded to 512 bytes");
  reject(R"({"version":1,"source":"gnome","windows":[{"id":")" + std::string(129, 'i') + "\"}]}",
         "window_report_invalid", "ids are bounded to 128 bytes");
  reject(window(R"(,"width":-1)"), "window_report_invalid", "sizes are non-negative");
  reject(window(R"(,"x":4294967296)"), "window_report_invalid", "coordinates fit 32 bits");
  reject(window(R"(,"pid":"4")"), "window_report_invalid", "pid must be an integer");
  reject(window(R"(,"active":1)"), "window_report_invalid", "flags must be booleans");
  reject(R"({"version":1,"source":"gnome","windows":[{"id":"d"},{"id":"d"}]})",
         "window_report_invalid", "duplicate ids reject the report");
  reject(std::string(window_report_max_bytes + 1, ' '), "window_report_too_large",
         "payloads are bounded to 64 KiB");
  const std::string valid_long = window(R"(,"title":")" + std::string(512, 'a') + "\"");
  expect(scan_of(parse_window_report(valid_long, WindowReportSource::gnome)) != nullptr,
         "a 512-byte string is accepted");
}

void window_report_truncates_excess_windows() {
  std::string payload = R"({"version":1,"source":"kwin","windows":[)";
  for (int index = 0; index < 70; ++index) {
    if (index) payload += ',';
    payload += R"({"id":"w)" + std::to_string(index) + "\"}";
  }
  payload += "]}";
  const auto result = parse_window_report(payload, WindowReportSource::kwin);
  const auto *scan = scan_of(result);
  expect(scan && scan->windows.size() == window_scan_limit && scan->truncated == 6,
         "more than 64 windows are truncated and counted");
}

void gnome_frame_is_byte_stable_and_bounded() {
  OverlayDisplayList list;
  list.bounds = {10, 20, 30, 40};
  list.ops.push_back({OverlayOpKind::fill_rect, {10, 20, 30, 40}, 0x112233, {}, 0});
  const std::vector<GnomePanel> panels{{"p\"1", list}};
  std::string expected_list;
  append_overlay_display_list_json(expected_list, list);
  const auto frame = build_gnome_overlay_frame(panels);
  expect(frame && *frame == R"({"version":1,"panels":[{"id":"p\"1",)" + expected_list.substr(1) +
                                "]}",
         "panels carry an escaped id followed by the shared display list");
  expect(build_gnome_overlay_frame({}) == std::optional<std::string>{R"({"version":1,"panels":[]})"},
         "an empty frame clears every panel");
  std::vector<GnomePanel> many(gnome_frame_max_panels + 1, panels.front());
  expect(!build_gnome_overlay_frame(many), "more than 64 panels are refused");
  GnomePanel busy{"busy", list};
  busy.list.ops.resize(gnome_frame_max_ops + 1, list.ops.front());
  expect(!build_gnome_overlay_frame(std::vector{busy}), "more than 256 ops are refused");
  GnomePanel wordy{"wordy", list};
  wordy.list.ops = std::vector(200, OverlayOp{OverlayOpKind::text, {}, 0, std::string(400, 'x'), 13});
  expect(!build_gnome_overlay_frame(std::vector{wordy}), "frames over 64 KiB are refused");
}

void restore_tokens_persist_per_profile() {
  layerc::test::TempDirectory root{"layerc-portal-tokens"};
  {
    const layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", root.path()};
    expect(screencast_token_path() == root.path() / "layerc" / "state" / "screencast.json",
           "tokens live under XDG data state");
  }
  const auto path = root.path() / "layerc" / "state" / "screencast.json";
  {
    ScreencastTokenStore store{path};
    expect(!store.token("a"), "no file means no token");
    expect(store.replace("a", "tok-1") && store.replace("b", "tok-2"), "tokens are stored");
    expect(store.replace("a", "tok-3"), "a new single-use token replaces the old one");
    expect(!store.replace("", "x") && !store.replace("c", std::string(513, 't')),
           "empty ids and oversized tokens are refused");
  }
  expect(layerc::test::read_file(path) ==
             std::optional<std::string>{R"({"version":1,"tokens":{"a":"tok-3","b":"tok-2"}})" "\n"},
         "the file is byte-stable and sorted");
  {
    ScreencastTokenStore store{path};
    expect(store.token("a") == std::optional<std::string>{"tok-3"} &&
               store.token("b") == std::optional<std::string>{"tok-2"},
           "tokens survive a restart");
    expect(store.replace("b", "") && !store.token("b"), "an empty token removes the entry");
  }
  layerc::test::write_file(path, R"({"version":1,"tokens":{"a":7,"b":"ok"}})");
  ScreencastTokenStore damaged{path};
  expect(!damaged.token("a") && damaged.token("b") == std::optional<std::string>{"ok"},
         "malformed entries are ignored");
  layerc::test::write_file(path, "not json");
  ScreencastTokenStore garbage{path};
  expect(!garbage.token("b"), "an unreadable file reads as empty");
  ScreencastTokenStore nowhere{{}};
  expect(!nowhere.replace("a", "t"), "no state path refuses writes");
}

void pipewire_frames_convert_formats_and_crops() {
  // 4x2 buffer, stride 20 (4 bytes of row padding).
  std::vector<std::byte> bytes(40);
  const auto put = [&](std::size_t x, std::size_t y, int b0, int b1, int b2) {
    const auto at = y * 20 + x * 4;
    bytes[at] = std::byte(b0);
    bytes[at + 1] = std::byte(b1);
    bytes[at + 2] = std::byte(b2);
  };
  for (std::size_t y = 0; y < 2; ++y)
    for (std::size_t x = 0; x < 4; ++x)
      put(x, y, static_cast<int>(10 * x), static_cast<int>(100 + y), 200);
  std::string error;
  RawVideoFrame raw{bytes, 4, 2, 20, VideoPixelFormat::bgrx};
  const auto now = layerc::core::TimePoint{std::chrono::seconds{5}};
  const auto bgr = convert_video_frame(raw, layerc::core::CaptureQuality::exact, std::nullopt,
                                       now, error);
  if (!expect(bgr != nullptr, "a BGRx buffer converts")) return;
  expect(bgr->width == 4 && bgr->height == 2 && bgr->captured_at == now &&
             bgr->pixels[1] == layerc::core::RgbPixel{200, 100, 10} &&
             bgr->pixels[4 + 3] == layerc::core::RgbPixel{200, 101, 30},
         "BGRx channels and row stride are honoured");
  raw.format = VideoPixelFormat::rgba;
  const auto rgb = convert_video_frame(raw, layerc::core::CaptureQuality::exact,
                                       layerc::capture::PixelBounds{2, 1, 4, 2}, now, error);
  expect(rgb && rgb->width == 2 && rgb->height == 1 &&
             rgb->pixels[0] == layerc::core::RgbPixel{20, 101, 200},
         "RGBA order and buffer crops are honoured");
  raw.data = std::span{bytes}.first(30);
  expect(!convert_video_frame(raw, layerc::core::CaptureQuality::exact, std::nullopt, now, error) &&
             !error.empty(),
         "a short buffer is rejected instead of read past its end");
  raw.data = bytes;
  raw.stride = 8;
  expect(!convert_video_frame(raw, layerc::core::CaptureQuality::exact, std::nullopt, now, error),
         "a stride narrower than the row is rejected");

  // A 1280x720 logical monitor streamed at 2x.
  const LogicalRect monitor{1920, 0, 1280, 720};
  expect(monitor_crop({2020, 100, 640, 360}, monitor, 2560, 1440) ==
             std::optional{layerc::capture::PixelBounds{200, 200, 1480, 920}},
         "logical window rects scale to buffer pixels");
  expect(monitor_crop({1800, -50, 300, 200}, monitor, 2560, 1440) ==
             std::optional{layerc::capture::PixelBounds{0, 0, 360, 300}},
         "partially visible windows are clamped to the monitor");
  expect(!monitor_crop({0, 0, 640, 360}, monitor, 2560, 1440),
         "a window on another monitor is outside the output");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(window_report_parses_full_snapshots),
                 LAYERC_TEST_CASE(window_report_rejects_malformed_reports),
                 LAYERC_TEST_CASE(window_report_truncates_excess_windows),
                 LAYERC_TEST_CASE(gnome_frame_is_byte_stable_and_bounded),
                 LAYERC_TEST_CASE(restore_tokens_persist_per_profile),
                 LAYERC_TEST_CASE(pipewire_frames_convert_formats_and_crops))
