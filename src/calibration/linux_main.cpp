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
#include <limits>
#include <string>
#include <string_view>

namespace {

using layerc::core::CaptureAvailability;
using layerc::core::CaptureBackendKind;
using layerc::core::CaptureQuality;
using layerc::core::NormalizedRegion;

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
      "[--region <x> <y> <width> <height>] [--json]\n",
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
      if (++index >= argc) usage();
      const std::string_view value{argv[index]};
      if (value == "low")
        options.quality = CaptureQuality::low;
      else if (value == "balanced")
        options.quality = CaptureQuality::balanced;
      else if (value == "exact")
        options.quality = CaptureQuality::exact;
      else
        usage();
      continue;
    }
    if (argument == "--region") {
      if (index + 4 >= argc ||
          !parse_number(argv[index + 1], options.region.x) ||
          !parse_number(argv[index + 2], options.region.y) ||
          !parse_number(argv[index + 3], options.region.width) ||
          !parse_number(argv[index + 4], options.region.height))
        usage();
      index += 4;
      continue;
    }
    usage();
  }
  if (!layerc::core::valid_normalized_region(options.region)) {
    std::fputs("layercal: region must be finite, nonempty, and within 0..1\n",
               stderr);
    std::exit(2);
  }
  return options;
}

[[nodiscard]] std::string_view quality_name(const CaptureQuality quality) {
  switch (quality) {
  case CaptureQuality::low: return "low";
  case CaptureQuality::balanced: return "balanced";
  case CaptureQuality::exact: return "exact";
  }
  return "balanced";
}

[[nodiscard]] std::string_view
availability_name(const CaptureAvailability availability) {
  switch (availability) {
  case CaptureAvailability::inactive: return "inactive";
  case CaptureAvailability::starting: return "starting";
  case CaptureAvailability::available: return "available";
  case CaptureAvailability::unavailable: return "unavailable";
  case CaptureAvailability::protected_or_black: return "protectedOrBlack";
  case CaptureAvailability::outside_output: return "outsideOutput";
  }
  return "unavailable";
}

[[nodiscard]] std::string_view backend_name(const CaptureBackendKind backend) {
  switch (backend) {
  case CaptureBackendKind::none: return "none";
  case CaptureBackendKind::windows_graphics_capture:
    return "windowsGraphicsCapture";
  case CaptureBackendKind::desktop_duplication: return "desktopDuplication";
  case CaptureBackendKind::x11_composite: return "x11Composite";
  case CaptureBackendKind::x11_full_frame: return "x11FullFrame";
  }
  return "none";
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

[[nodiscard]] std::uint32_t region_edge(const double value,
                                        const std::uint32_t extent,
                                        const bool upper) {
  const double scaled = value * static_cast<double>(extent);
  return static_cast<std::uint32_t>(std::clamp(
      upper ? std::ceil(scaled) : std::floor(scaled), 0.0,
      static_cast<double>(extent)));
}

void append_region(std::string &out, const NormalizedRegion &region) {
  out += "{\"x\":";
  layerc::core::json::append_number(out, region.x);
  out += ",\"y\":";
  layerc::core::json::append_number(out, region.y);
  out += ",\"width\":";
  layerc::core::json::append_number(out, region.width);
  out += ",\"height\":";
  layerc::core::json::append_number(out, region.height);
  out += "}";
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

  std::string out{"{\"schemaVersion\":1,\"window\":"};
  layerc::core::json::append_quoted(out,
                                    std::to_string(options.window.opaque));
  out += ",\"quality\":";
  layerc::core::json::append_quoted(out, quality_name(options.quality));
  out += ",\"availability\":";
  layerc::core::json::append_quoted(
      out, availability_name(poll.result.availability));
  out += ",\"backend\":";
  layerc::core::json::append_quoted(out, backend_name(poll.result.backend));
  out += ",\"width\":";
  layerc::core::json::append_unsigned(out, width);
  out += ",\"height\":";
  layerc::core::json::append_unsigned(out, height);
  out += ",\"region\":";
  append_region(out, options.region);
  out += ",\"pixelRegion\":{\"left\":";
  layerc::core::json::append_unsigned(
      out, region_edge(options.region.x, width, false));
  out += ",\"top\":";
  layerc::core::json::append_unsigned(
      out, region_edge(options.region.y, height, false));
  out += ",\"right\":";
  layerc::core::json::append_unsigned(
      out, region_edge(options.region.x + options.region.width, width, true));
  out += ",\"bottom\":";
  layerc::core::json::append_unsigned(
      out, region_edge(options.region.y + options.region.height, height, true));
  out += "},\"snapshot\":{\"format\":\"ppm\",\"bytes\":";
  layerc::core::json::append_unsigned(out, snapshot_bytes);
  out += "},\"resources\":{\"captureSessions\":";
  layerc::core::json::append_unsigned(out, resources.capture_sessions);
  out += ",\"objects\":";
  layerc::core::json::append_unsigned(out, resources.objects);
  out += ",\"cacheBytes\":";
  layerc::core::json::append_unsigned(out, resources.cache_bytes);
  out += "},\"diagnostic\":";
  layerc::core::json::append_quoted(out, poll.result.diagnostic);
  out += "}\n";
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
  const bool started = session.start();
  auto poll = session.poll(layerc::core::MonotonicClock::now());
  if (!started && poll.result.availability == CaptureAvailability::inactive)
    poll.result.availability = CaptureAvailability::unavailable;
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
