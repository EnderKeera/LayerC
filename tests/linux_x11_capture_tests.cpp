// Composite named-pixmap capture, Damage coalescing and bounded direct-window
// fallback. Returns 77 when the host cannot provide the Xvfb fixture.
#include "x11_capture.hpp"
#include "x11_error.hpp"

#include "test_support.hpp"

#include "layerc/core/json.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

extern char **environ;

namespace {

using layerc::capture::CapturePoll;
using layerc::core::CaptureAvailability;
using layerc::core::CaptureBackendKind;
using layerc::core::CaptureQuality;
using layerc::core::RgbPixel;
using layerc::platform::X11CaptureSession;
using layerc::platform::X11Display;

using layerc::test::skip_code;

struct FixtureColor {
  unsigned long pixel{};
  RgbPixel rgb;
};

[[nodiscard]] bool usable(const layerc::platform::X11Extension &extension,
                          const int major, const int minor) noexcept {
  return extension.present &&
         (extension.major > major ||
          (extension.major == major && extension.minor >= minor));
}

[[nodiscard]] bool allocate_color(Display *display, const Colormap colormap,
                                  const std::uint8_t red,
                                  const std::uint8_t green,
                                  const std::uint8_t blue,
                                  FixtureColor &result) {
  XColor color{};
  color.red = static_cast<unsigned short>(red * 257U);
  color.green = static_cast<unsigned short>(green * 257U);
  color.blue = static_cast<unsigned short>(blue * 257U);
  color.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(display, colormap, &color) == 0) return false;
  result.pixel = color.pixel;
  result.rgb = {
      static_cast<std::uint8_t>((color.red + 128U) / 257U),
      static_cast<std::uint8_t>((color.green + 128U) / 257U),
      static_cast<std::uint8_t>((color.blue + 128U) / 257U),
  };
  return true;
}

void fill(Display *display, const ::Window window, const GC gc,
          const FixtureColor color, const unsigned int width,
          const unsigned int height) {
  XSetForeground(display, gc, color.pixel);
  XFillRectangle(display, window, gc, 0, 0, width, height);
  XSync(display, False);
}

struct CommandResult {
  int exit_code{-1};
  std::string output;
};

[[nodiscard]] CommandResult
run_layercal(const std::string &executable,
             const std::vector<std::string> &arguments) {
  CommandResult result;
  int output_pipe[2]{};
  if (::pipe(output_pipe) != 0) return result;

  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1U);
  storage.push_back(executable);
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1U);
  for (auto &value : storage) argv.push_back(value.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    return result;
  }
  (void)posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  (void)posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
  (void)posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0);
  pid_t child{};
  const int spawned = posix_spawn(&child, executable.c_str(), &actions, nullptr,
                                  argv.data(), environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  ::close(output_pipe[1]);
  if (spawned != 0) {
    ::close(output_pipe[0]);
    return result;
  }

  char chunk[4096];
  for (;;) {
    const ssize_t received = ::read(output_pipe[0], chunk, sizeof(chunk));
    if (received > 0) {
      result.output.append(chunk, static_cast<std::size_t>(received));
      continue;
    }
    if (received < 0 && errno == EINTR) continue;
    break;
  }
  ::close(output_pipe[0]);

  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  return result;
}

[[nodiscard]] bool ppm_matches(const std::string &bytes,
                               const std::uint32_t width,
                               const std::uint32_t height,
                               const RgbPixel color) {
  const std::string header = "P6\n" + std::to_string(width) + " " +
                             std::to_string(height) + "\n255\n";
  if (!bytes.starts_with(header) ||
      bytes.size() != header.size() +
                          static_cast<std::size_t>(width) * height * 3U)
    return false;
  for (std::size_t index = header.size(); index < bytes.size(); index += 3U) {
    if (static_cast<std::uint8_t>(bytes[index]) != color.red ||
        static_cast<std::uint8_t>(bytes[index + 1U]) != color.green ||
        static_cast<std::uint8_t>(bytes[index + 2U]) != color.blue)
      return false;
  }
  return true;
}

[[nodiscard]] const layerc::core::json::Value *
json_object(const std::string &source,
            layerc::core::json::ParseResult &parsed) {
  parsed = layerc::core::json::parse(source);
  const auto *value = std::get_if<layerc::core::json::Value>(&parsed);
  return value != nullptr && value->is_object() ? value : nullptr;
}

[[nodiscard]] bool frame_matches(const CapturePoll &poll,
                                 const std::uint32_t width,
                                 const std::uint32_t height,
                                 const RgbPixel color) {
  const auto &result = poll.result;
  if (result.availability != CaptureAvailability::available || !result.frame ||
      result.frame->width != width || result.frame->height != height ||
      result.frame->pixels.size() !=
          static_cast<std::size_t>(width) * height)
    return false;
  const auto mismatch = std::find_if(
      result.frame->pixels.begin(), result.frame->pixels.end(),
      [&](const RgbPixel pixel) { return pixel != color; });
  if (mismatch == result.frame->pixels.end()) return true;
  std::fprintf(stderr, "pixel mismatch: expected %u,%u,%u; got %u,%u,%u\n",
               color.red, color.green, color.blue, mismatch->red,
               mismatch->green, mismatch->blue);
  return false;
}

} // namespace

int run_linux_x11_capture_tests(const int argc, char **argv) {
  using layerc::test::expect;
  bool passed = true;
  if (argc != 2) {
    std::fprintf(stderr, "usage: linux_x11_capture_tests <layercal>\n");
    return 2;
  }
  const std::string layercal{argv[1]};

  if (!layerc::test::x11_display_available()) {
    std::fprintf(stderr, "skip: DISPLAY is not set (run under Xvfb)\n");
    return skip_code;
  }

  std::string error;
  auto display = X11Display::open(error);
  if (!display.has_value()) {
    std::fprintf(stderr, "skip: %s\n", error.c_str());
    return skip_code;
  }
  const auto &probe = display->probe();
  if (!usable(probe.composite, 0, 2) || !usable(probe.damage, 1, 0) ||
      !usable(probe.xfixes, 2, 0)) {
    std::fprintf(stderr,
                 "skip: X Composite 0.2, Damage 1.0 and XFixes 2.0 are "
                 "required\n");
    return skip_code;
  }
  passed &= expect(probe.damage.event_base > 0,
                   "Damage publishes its event base");

  auto *raw = reinterpret_cast<Display *>(display->raw());
  const ::Window root = DefaultRootWindow(raw);
  const int screen = DefaultScreen(raw);
  const Colormap colormap = DefaultColormap(raw, screen);
  const GC gc = XCreateGC(raw, root, 0, nullptr);
  if (gc == nullptr) {
    std::fprintf(stderr, "skip: cannot create an X11 graphics context\n");
    return skip_code;
  }

  FixtureColor initial;
  FixtureColor first;
  FixtureColor final;
  FixtureColor resized;
  FixtureColor black;
  const bool colors_ok =
      allocate_color(raw, colormap, 24, 72, 136, initial) &&
      allocate_color(raw, colormap, 184, 40, 56, first) &&
      allocate_color(raw, colormap, 32, 168, 88, final) &&
      allocate_color(raw, colormap, 112, 64, 208, resized) &&
      allocate_color(raw, colormap, 0, 0, 0, black);
  if (!colors_ok) {
    XFreeGC(raw, gc);
    std::fprintf(stderr, "skip: cannot allocate deterministic X11 colors\n");
    return skip_code;
  }

  const ::Window composite_window =
      XCreateSimpleWindow(raw, root, 0, 0, 320, 180, 0, black.pixel, black.pixel);
  XMapWindow(raw, composite_window);
  XSync(raw, False);
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(raw, composite_window, &attributes) == 0 ||
      attributes.visual == nullptr || attributes.visual->c_class != TrueColor) {
    XDestroyWindow(raw, composite_window);
    XFreeGC(raw, gc);
    std::fprintf(stderr, "skip: fixture needs a TrueColor visual\n");
    return skip_code;
  }

  {
    layerc::platform::X11ErrorTrap trap{raw};
    XCompositeRedirectWindow(raw, composite_window, CompositeRedirectAutomatic);
    XSync(raw, False);
    passed &= expect(trap.ok(), "test fixture redirects its Composite window");
  }
  fill(raw, composite_window, gc, initial, 320, 180);

  const layerc::core::WindowId composite_id{
      static_cast<std::uint64_t>(composite_window)};
  X11CaptureSession composite(*display, composite_id, CaptureQuality::exact);
  passed &= expect(composite.start(), "redirected capture session starts");
  const layerc::core::TimePoint start_time{};
  const auto initial_poll = composite.poll(start_time);
  passed &= expect(initial_poll.window == composite_id,
                   "capture result retains the target window key");
  passed &= expect(initial_poll.result.backend ==
                       CaptureBackendKind::x11_composite,
                   "redirected window selects Composite capture");
  passed &= expect(frame_matches(initial_poll, 320, 180, initial.rgb),
                   "exact Composite pixels are deterministic");

  const auto initial_frame = initial_poll.result.frame;
  const auto quiet_poll = composite.poll(start_time + std::chrono::milliseconds{1});
  passed &= expect(quiet_poll.result.frame == initial_frame,
                   "quiet Damage poll reuses the frame pointer");
  if (initial_frame && quiet_poll.result.frame)
    passed &= expect(quiet_poll.result.frame->captured_at ==
                         initial_frame->captured_at,
                     "quiet Damage poll preserves capture time");

  fill(raw, composite_window, gc, first, 320, 180);
  fill(raw, composite_window, gc, final, 320, 180);
  const auto coalesced =
      composite.poll(start_time + std::chrono::milliseconds{2});
  passed &= expect(frame_matches(coalesced, 320, 180, final.rgb),
                   "two damaged draws coalesce to the final pixels");
  passed &= expect(coalesced.result.frame != initial_frame,
                   "damaged Composite capture publishes a new frame");
  const auto coalesced_frame = coalesced.result.frame;
  const auto coalesced_quiet =
      composite.poll(start_time + std::chrono::milliseconds{3});
  passed &= expect(coalesced_quiet.result.frame == coalesced_frame,
                   "post-damage quiet poll reuses the coalesced frame");

  composite.set_quality(CaptureQuality::low);
  const auto low = composite.poll(start_time + std::chrono::milliseconds{4});
  passed &= expect(frame_matches(low, 64, 36, final.rgb),
                   "Low capture is 64x36");
  passed &= expect(composite.resources().cache_bytes == 64U * 36U * 3U,
                   "Low capture reports its bounded pixel cache");
  composite.set_quality(CaptureQuality::balanced);
  const auto balanced =
      composite.poll(start_time + std::chrono::milliseconds{5});
  passed &= expect(frame_matches(balanced, 256, 144, final.rgb),
                   "Balanced capture is 256x144");
  passed &= expect(composite.resources().cache_bytes == 256U * 144U * 3U,
                   "Balanced capture reports its bounded pixel cache");
  composite.set_quality(CaptureQuality::exact);
  const auto exact = composite.poll(start_time + std::chrono::milliseconds{6});
  passed &= expect(frame_matches(exact, 320, 180, final.rgb),
                   "Exact capture remains source-sized");
  const auto exact_resources = composite.resources();
  passed &= expect(exact_resources.capture_sessions == 1 &&
                       exact_resources.objects == 3 &&
                       exact_resources.cache_bytes == 320U * 180U * 3U,
                   "Composite capture reports one session and owned resources");

  const auto cli_low = run_layercal(
      layercal, {"snapshot", std::to_string(composite_window), "--quality",
                 "low"});
  passed &= expect(cli_low.exit_code == 0 &&
                       ppm_matches(cli_low.output, 64, 36, final.rgb),
                   "layercal writes binary-clean Low PPM bytes to stdout");

  const auto cli_metadata = run_layercal(
      layercal,
      {"snapshot", std::to_string(composite_window), "--quality", "balanced",
       "--region", "0.25", "0.2", "0.5", "0.4", "--json"});
  layerc::core::json::ParseResult metadata_parse;
  const auto *metadata = json_object(cli_metadata.output, metadata_parse);
  const auto *region = metadata != nullptr ? metadata->find("region") : nullptr;
  const auto *pixel_region =
      metadata != nullptr ? metadata->find("pixelRegion") : nullptr;
  const auto *snapshot =
      metadata != nullptr ? metadata->find("snapshot") : nullptr;
  const auto *resources =
      metadata != nullptr ? metadata->find("resources") : nullptr;
  const std::uint64_t balanced_ppm_bytes =
      std::string{"P6\n256 144\n255\n"}.size() + 256U * 144U * 3U;
  passed &= expect(
      cli_metadata.exit_code == 0 && metadata != nullptr &&
          metadata->find("availability") != nullptr &&
          metadata->find("availability")->as_string() == "available" &&
          metadata->find("backend") != nullptr &&
          metadata->find("backend")->as_string() == "x11Composite" &&
          metadata->find("width") != nullptr &&
          metadata->find("width")->as_integer() == 256 &&
          metadata->find("height") != nullptr &&
          metadata->find("height")->as_integer() == 144 && region != nullptr &&
          region->find("x") != nullptr &&
          region->find("x")->as_number() == 0.25 &&
          pixel_region != nullptr && pixel_region->find("left") != nullptr &&
          pixel_region->find("left")->as_integer() == 64 &&
          pixel_region->find("top") != nullptr &&
          pixel_region->find("top")->as_integer() == 28 &&
          pixel_region->find("right") != nullptr &&
          pixel_region->find("right")->as_integer() == 192 &&
          pixel_region->find("bottom") != nullptr &&
          pixel_region->find("bottom")->as_integer() == 87 &&
          snapshot != nullptr && snapshot->find("bytes") != nullptr &&
          snapshot->find("bytes")->as_integer() ==
              static_cast<std::int64_t>(balanced_ppm_bytes) &&
          resources != nullptr && resources->find("captureSessions") != nullptr &&
          resources->find("captureSessions")->as_integer() == 1 &&
          resources->find("objects") != nullptr &&
          resources->find("objects")->as_integer() == 3 &&
          resources->find("cacheBytes") != nullptr &&
          resources->find("cacheBytes")->as_integer() == 256 * 144 * 3,
      "layercal JSON reports validated region, grid, and resource metadata");

  const auto invalid_region = run_layercal(
      layercal,
      {"snapshot", std::to_string(composite_window), "--region", "0.9", "0",
       "0.2", "1", "--json"});
  passed &= expect(invalid_region.exit_code == 2 &&
                       invalid_region.output.empty(),
                   "layercal rejects an out-of-bounds normalized region");

  XResizeWindow(raw, composite_window, 400, 200);
  XSync(raw, False);
  fill(raw, composite_window, gc, resized, 400, 200);
  const auto resized_poll =
      composite.poll(start_time + std::chrono::milliseconds{7});
  passed &= expect(resized_poll.result.backend ==
                       CaptureBackendKind::x11_composite,
                   "resize re-names the Composite pixmap");
  passed &= expect(frame_matches(resized_poll, 400, 200, resized.rgb),
                   "resize returns new exact dimensions and pixels");

  CapturePoll black_poll;
  for (std::uint32_t index = 0; index < 30; ++index) {
    fill(raw, composite_window, gc, black, 400, 200);
    black_poll = composite.poll(start_time +
                                std::chrono::milliseconds{8 + index});
    if (index < 29)
      passed &= expect(frame_matches(black_poll, 400, 200, black.rgb),
                       "black threshold waits for 30 captured frames");
  }
  passed &= expect(black_poll.result.availability ==
                       CaptureAvailability::protected_or_black,
                   "30 damaged black captures report protected_or_black");
  passed &= expect(!black_poll.result.frame,
                   "protected_or_black clears the outgoing frame");

  fill(raw, composite_window, gc, final, 400, 200);
  const auto recovered =
      composite.poll(start_time + std::chrono::milliseconds{38});
  passed &= expect(frame_matches(recovered, 400, 200, final.rgb),
                   "a non-black damaged frame recovers capture");

  composite.stop();
  passed &= expect(composite.resources() ==
                       layerc::runtime::RuntimeResourceStats{},
                   "stopping Composite capture releases all owned resources");
  {
    layerc::platform::X11ErrorTrap trap{raw};
    XCompositeUnredirectWindow(raw, composite_window,
                               CompositeRedirectAutomatic);
    XSync(raw, False);
    passed &= expect(trap.ok(), "test fixture removes its Composite redirect");
  }
  XDestroyWindow(raw, composite_window);
  XSync(raw, False);

  const ::Window direct_window =
      XCreateSimpleWindow(raw, root, 0, 0, 80, 60, 0, black.pixel, black.pixel);
  XMapWindow(raw, direct_window);
  XSync(raw, False);
  fill(raw, direct_window, gc, initial, 80, 60);

  const layerc::core::WindowId direct_id{
      static_cast<std::uint64_t>(direct_window)};
  X11CaptureSession direct(*display, direct_id, CaptureQuality::exact);
  passed &= expect(direct.start(), "unredirected capture session starts");
  const auto direct_time = start_time + std::chrono::seconds{1};
  const auto direct_initial = direct.poll(direct_time);
  passed &= expect(direct_initial.result.backend ==
                       CaptureBackendKind::x11_full_frame,
                   "unredirected window selects direct fallback");
  passed &= expect(!direct_initial.result.diagnostic.empty(),
                   "direct fallback retains its Composite warning");
  passed &= expect(frame_matches(direct_initial, 80, 60, initial.rgb),
                   "direct fallback captures deterministic pixels");

  const auto direct_frame = direct_initial.result.frame;
  fill(raw, direct_window, gc, final, 80, 60);
  const auto early = direct.poll(direct_time + std::chrono::milliseconds{50});
  passed &= expect(early.result.frame == direct_frame,
                   "direct fallback reuses its frame at +50ms");
  const auto due = direct.poll(direct_time + std::chrono::milliseconds{100});
  passed &= expect(frame_matches(due, 80, 60, final.rgb),
                   "direct fallback captures new pixels at +100ms");
  if (direct_frame && due.result.frame)
    passed &= expect(due.result.frame->captured_at != direct_frame->captured_at,
                     "bounded direct recapture updates capture time");

  XDestroyWindow(raw, direct_window);
  XSync(raw, False);
  const auto lost = direct.poll(direct_time + std::chrono::milliseconds{200});
  passed &= expect(lost.window == direct_id,
                   "lost-window result retains the target key");
  passed &= expect(lost.result.availability == CaptureAvailability::unavailable,
                   "destroyed target reports unavailable");
  passed &= expect(!lost.result.frame,
                   "destroyed target clears the outgoing frame");
  const auto lost_resources = direct.resources();
  passed &= expect(lost_resources.capture_sessions == 1 &&
                       lost_resources.objects == 0 &&
                       lost_resources.cache_bytes == 0,
                   "window loss clears capture objects and cached pixels");

  const auto cli_fault = run_layercal(
      layercal,
      {"snapshot", std::to_string(direct_window), "--json"});
  layerc::core::json::ParseResult fault_parse;
  const auto *fault = json_object(cli_fault.output, fault_parse);
  const auto *fault_resources =
      fault != nullptr ? fault->find("resources") : nullptr;
  passed &= expect(
      cli_fault.exit_code == 1 && fault != nullptr &&
          fault->find("availability") != nullptr &&
          fault->find("availability")->as_string() == "unavailable" &&
          fault->find("diagnostic") != nullptr &&
          fault->find("diagnostic")->as_string().find("target window") !=
              std::string::npos &&
          fault_resources != nullptr &&
          fault_resources->find("captureSessions") != nullptr &&
          fault_resources->find("captureSessions")->as_integer() == 0 &&
          fault_resources->find("cacheBytes") != nullptr &&
          fault_resources->find("cacheBytes")->as_integer() == 0,
      "layercal JSON preserves fault diagnostics without snapshot bytes");

  direct.stop();
  passed &= expect(direct.resources() ==
                       layerc::runtime::RuntimeResourceStats{},
                   "stopping direct capture releases its session resource");
  std::vector<unsigned long> pixels = {initial.pixel, first.pixel, final.pixel,
                                       resized.pixel, black.pixel};
  XFreeColors(raw, colormap, pixels.data(), static_cast<int>(pixels.size()), 0);
  XFreeGC(raw, gc);
  XSync(raw, False);

  std::printf("x11 capture tests: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}

int main(const int argc, char **argv) {
  return layerc::test::run({{"linux_x11_capture_tests", [argc, argv] {
                              const int result =
                                  run_linux_x11_capture_tests(argc, argv);
                              if (result == skip_code) layerc::test::skip();
                              layerc::test::expect(
                                  result == 0,
                                  "the X11 capture program should succeed");
                            }}});
}
