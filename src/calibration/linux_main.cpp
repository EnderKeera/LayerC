// Linux calibration CLI: a bounded X11 snapshot goes to stdout as PPM, or
// --json emits capture/region metadata suitable for a visual sidecar.
#include "x11_capture.hpp"
#include "x11_display.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/visual.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using layerc::core::CaptureAvailability;
using layerc::core::CaptureQuality;
using layerc::core::NormalizedRegion;
using layerc::core::json::Value;

struct Options {
  layerc::core::WindowId window;
  CaptureQuality quality{CaptureQuality::balanced};
  NormalizedRegion region{0.0, 0.0, 1.0, 1.0};
  bool json{};
};

[[noreturn]] void usage() {
  std::fputs(
      "usage: layercal snapshot <x11-window-id> "
      "[--quality low|balanced|exact] "
      "[--region <x> <y> <width> <height>] [--json]\n"
      "       --region values are normalized fractions of the window (0..1)\n",
      stderr);
  std::exit(2);
}

[[nodiscard]] bool parse_window(const std::string_view text,
                                std::uint64_t &value) noexcept {
  int base = 10;
  auto first = text.data();
  if (text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    first += 2;
  }
  if (first == text.data() + text.size()) return false;
  const auto [tail, error] =
      std::from_chars(first, text.data() + text.size(), value, base);
  return error == std::errc{} && tail == text.data() + text.size() &&
         value != 0;
}

[[nodiscard]] bool parse_number(const char *text, double &value) noexcept {
  if (text == nullptr || *text == '\0') return false;
  char *tail{};
  errno = 0;
  value = std::strtod(text, &tail);
  return errno == 0 && tail != text && *tail == '\0' && std::isfinite(value);
}

[[nodiscard]] Options parse_options(const int argc, char **argv) {
  if (argc < 3 || std::string_view{argv[1]} != "snapshot") usage();
  Options options;
  if (!parse_window(argv[2], options.window.opaque)) {
    std::fputs("layercal: x11-window-id must be a nonzero decimal or hex XID\n",
               stderr);
    usage();
  }

  for (int index = 3; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--json") {
      options.json = true;
      continue;
    }
    if (argument == "--quality") {
      if (++index >= argc) {
        std::fputs("layercal: --quality requires a value\n", stderr);
        usage();
      }
      const auto quality = layerc::core::parse_enum_wire(
          layerc::core::capture_quality_wire, std::string_view{argv[index]});
      if (!quality) {
        std::fprintf(stderr,
                     "layercal: unknown --quality '%s' (expected low, "
                     "balanced, or exact)\n",
                     argv[index]);
        usage();
      }
      options.quality = *quality;
      continue;
    }
    if (argument == "--region") {
      if (index + 4 >= argc ||
          !parse_number(argv[index + 1], options.region.x) ||
          !parse_number(argv[index + 2], options.region.y) ||
          !parse_number(argv[index + 3], options.region.width) ||
          !parse_number(argv[index + 4], options.region.height)) {
        std::fputs("layercal: --region requires four numbers: x y width "
                   "height\n",
                   stderr);
        usage();
      }
      index += 4;
      continue;
    }
    std::fprintf(stderr, "layercal: unknown option '%s'\n", argv[index]);
    usage();
  }
  if (!layerc::core::valid_normalized_region(options.region)) {
    std::fputs("layercal: region must be finite, nonempty, and within 0..1\n",
               stderr);
    std::exit(2);
  }
  return options;
}

[[nodiscard]] std::string ppm_header(const std::uint32_t width,
                                     const std::uint32_t height) {
  return "P6\n" + std::to_string(width) + " " + std::to_string(height) +
         "\n255\n";
}

[[nodiscard]] bool write_ppm(const layerc::capture::CaptureFrame &frame) {
  static_assert(sizeof(layerc::core::RgbPixel) == 3);
  const auto header = ppm_header(frame.width, frame.height);
  return std::fwrite(header.data(), 1, header.size(), stdout) == header.size() &&
         std::fwrite(frame.pixels.data(), sizeof(layerc::core::RgbPixel),
                     frame.pixels.size(), stdout) == frame.pixels.size() &&
         std::fflush(stdout) == 0;
}

[[nodiscard]] Value integer(const std::uint64_t value) {
  return Value(static_cast<std::int64_t>(value));
}

// Wire names come from the shared visual tables so the JSON report and
// saved sidecars use one vocabulary.
template <typename Table, typename Enum>
[[nodiscard]] Value wire_name(const Table &table, const Enum value,
                              const std::string_view fallback) {
  return Value(std::string(
      layerc::core::enum_wire_name(table, value).value_or(fallback)));
}

void write_metadata(const Options &options,
                    const layerc::capture::CapturePoll &poll,
                    const layerc::runtime::RuntimeResourceStats &resources) {
  const auto *frame = poll.result.frame.get();
  const std::uint32_t width = frame == nullptr ? 0U : frame->width;
  const std::uint32_t height = frame == nullptr ? 0U : frame->height;
  const auto header = ppm_header(width, height);
  const std::uint64_t snapshot_bytes =
      frame == nullptr
          ? 0U
          : static_cast<std::uint64_t>(header.size()) +
                static_cast<std::uint64_t>(frame->pixels.size()) * 3U;

  const auto &region = options.region;
  const auto pixels = layerc::core::to_pixel_bounds(region, width, height);
  const Value metadata(Value::Object{
      {"schemaVersion", integer(1)},
      {"window", Value(std::to_string(options.window.opaque))},
      {"quality", wire_name(layerc::core::capture_quality_wire,
                            options.quality, "balanced")},
      {"availability", wire_name(layerc::core::capture_availability_wire,
                                 poll.result.availability, "unavailable")},
      {"backend", wire_name(layerc::core::capture_backend_wire,
                            poll.result.backend, "none")},
      {"width", integer(width)},
      {"height", integer(height)},
      {"region", layerc::core::normalized_region_value(region)},
      {"pixelRegion", Value(Value::Object{{"left", integer(pixels.left)},
                                          {"top", integer(pixels.top)},
                                          {"right", integer(pixels.right)},
                                          {"bottom", integer(pixels.bottom)}})},
      {"snapshot", Value(Value::Object{{"format", Value(std::string("ppm"))},
                                       {"bytes", integer(snapshot_bytes)}})},
      {"resources",
       Value(Value::Object{
           {"captureSessions", integer(resources.capture_sessions)},
           {"objects", integer(resources.objects)},
           {"cacheBytes", integer(resources.cache_bytes)}})},
      {"diagnostic", Value(poll.result.diagnostic)}});
  std::string out;
  layerc::core::json::append_value(out, metadata);
  out += '\n';
  (void)std::fwrite(out.data(), 1, out.size(), stdout);
}

} // namespace

int main(const int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  std::string error;
  auto display = layerc::platform::X11Display::open(error);
  if (!display.has_value()) {
    std::fprintf(stderr, "layercal: %s\n", error.c_str());
    return 1;
  }

  layerc::platform::X11CaptureSession session(*display, options.window,
                                               options.quality);
  (void)session.start();
  const auto poll = session.poll(layerc::core::MonotonicClock::now());
  const bool available =
      poll.result.availability == CaptureAvailability::available &&
      poll.result.frame != nullptr;
  if (options.json) {
    write_metadata(options, poll, session.resources());
    return available ? 0 : 1;
  }
  if (!available) {
    std::fprintf(stderr, "layercal: %s\n",
                 poll.result.diagnostic.empty()
                     ? "capture is unavailable"
                     : poll.result.diagnostic.c_str());
    return 1;
  }
  if (!write_ppm(*poll.result.frame)) {
    std::fputs("layercal: writing the PPM snapshot failed\n", stderr);
    return 1;
  }
  return 0;
}
