// X11 inline visual capture, profile-local PNG resolution and cooldown routing.
// Returns 77 when the host cannot provide the Xvfb fixture.
#include "visual_service.hpp"
#include "x11_error.hpp"

#include "layerc/platform/profile_store_linux.hpp"

#include "test_support.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace {

using layerc::test::skip_code;
constexpr std::uint32_t window_axis = 64;
constexpr std::uint32_t template_axis = 8;
constexpr std::string_view template_sha =
    "ca74586c0ae0504d1fd1b00f0edcb7e2666570b62f35a32cf8a21c77f351639b";
constexpr std::array<std::uint8_t, 325> template_png{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x08U,
    0x00U, 0x00U, 0x00U, 0x08U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xc4U,
    0x0fU, 0xbeU, 0x8bU, 0x00U, 0x00U, 0x01U, 0x0cU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0x9cU, 0x05U, 0xc1U, 0x4dU, 0x44U, 0x04U, 0x51U, 0x00U,
    0xc0U, 0xf1U, 0x47U, 0x12U, 0x7bU, 0x28U, 0x4bU, 0x1dU, 0xa2U, 0x0fU,
    0x66U, 0x29U, 0x52U, 0x87U, 0x35U, 0xe9U, 0xd0U, 0x52U, 0xc9U, 0x74U,
    0x58U, 0x92U, 0x58U, 0xa3U, 0xa4U, 0x3dU, 0x2cU, 0x79U, 0x62U, 0x0fU,
    0x25U, 0xe5U, 0xd9U, 0x24U, 0xb1U, 0x87U, 0x9eU, 0xa5U, 0xd8U, 0x0eU,
    0xd5U, 0xd8U, 0x43U, 0x89U, 0xa2U, 0xa1U, 0x0fU, 0x8aU, 0xd2U, 0x28U,
    0x8aU, 0x14U, 0x0dU, 0xd5U, 0xa1U, 0x28U, 0x46U, 0x1dU, 0x96U, 0x24U,
    0xe6U, 0xd6U, 0x2dU, 0xfeU, 0xfdU, 0x7eU, 0x42U, 0x08U, 0x41U, 0x73U,
    0x65U, 0x94U, 0x9eU, 0x88U, 0x41U, 0xbaU, 0xc6U, 0x64U, 0xa1U, 0xd6U,
    0xa2U, 0x54U, 0x6fU, 0x73U, 0xd1U, 0x24U, 0x79U, 0x8bU, 0x29U, 0x44U,
    0x55U, 0x63U, 0x94U, 0x16U, 0xc3U, 0x60U, 0xa0U, 0xd5U, 0x64U, 0xa2U,
    0xddU, 0x22U, 0x1fU, 0xb7U, 0xd9U, 0xe9U, 0x92U, 0x5cU, 0x27U, 0x14U,
    0x9fU, 0x7dU, 0x1aU, 0x51U, 0xddU, 0x6dU, 0xd0U, 0xd1U, 0x6bU, 0x32U,
    0x68U, 0x59U, 0x64U, 0x93U, 0x36U, 0x85U, 0x21U, 0xc9U, 0x7eU, 0x4aU,
    0x71U, 0x3fU, 0xaaU, 0xf9U, 0x4aU, 0x3bU, 0x88U, 0xbaU, 0x11U, 0x93U,
    0xceU, 0x71U, 0x8bU, 0x54U, 0xc6U, 0x66U, 0x46U, 0x4aU, 0x8aU, 0x59U,
    0xc5U, 0xf1U, 0xb4U, 0xe6U, 0x71U, 0xceU, 0x21U, 0x9cU, 0x77U, 0x11U,
    0x0dU, 0xb3U, 0x16U, 0x89U, 0x9cU, 0xcdU, 0xd8U, 0xa2U, 0x24U, 0x97U,
    0x57U, 0x6cU, 0x6aU, 0xcdU, 0xd9U, 0x8aU, 0xc3U, 0xebU, 0x9aU, 0xcbU,
    0xefU, 0x86U, 0x87U, 0x88U, 0x15U, 0x6dU, 0xfaU, 0xd7U, 0x25U, 0x99U,
    0x92U, 0x62U, 0x69U, 0x5bU, 0xb3U, 0xb5U, 0xebU, 0x70U, 0xe5U, 0xbaU,
    0x04U, 0x47U, 0x1eU, 0x7fU, 0xa7U, 0x3eU, 0xa2U, 0xedU, 0x50U, 0x92U,
    0x3cU, 0x51U, 0x4cU, 0x9eU, 0x6bU, 0x96U, 0x2fU, 0x1dU, 0xf6U, 0x6eU,
    0x5cU, 0x6eU, 0xefU, 0x3cU, 0xcaU, 0xbeU, 0x4fU, 0xc5U, 0x73U, 0x80U,
    0x88U, 0x3fU, 0x28U, 0x86U, 0x9fU, 0x34U, 0x53U, 0x2fU, 0x0eU, 0xabU,
    0xefU, 0x2eU, 0x07U, 0x1fU, 0x1eU, 0x7eU, 0xd9U, 0xe7U, 0xe7U, 0x3bU,
    0x20U, 0x12U, 0x86U, 0xfcU, 0x03U, 0x52U, 0x92U, 0x9aU, 0x81U, 0x5eU,
    0x26U, 0x69U, 0xf0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU,
    0x44U, 0xaeU, 0x42U, 0x60U, 0x82U};

struct TempDirectory {
  std::filesystem::path path;
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct FixtureColor {
  unsigned long pixel{};
  layerc::core::RgbPixel rgb;
};

[[nodiscard]] bool usable(const layerc::platform::X11Extension &extension,
                          const int major, const int minor) noexcept {
  return extension.present &&
         (extension.major > major ||
          (extension.major == major && extension.minor >= minor));
}

[[nodiscard]] layerc::core::RgbPixel pattern_pixel(const std::uint32_t x,
                                                   const std::uint32_t y) {
  return {static_cast<std::uint8_t>((x * 31U + y * 7U) % 256U),
          static_cast<std::uint8_t>((y * 29U + x * 5U) % 256U),
          static_cast<std::uint8_t>(((x + y) * 17U) % 256U)};
}

[[nodiscard]] bool allocate_color(Display *display, const Colormap colormap,
                                  const layerc::core::RgbPixel requested,
                                  FixtureColor &result) {
  XColor color{};
  color.red = static_cast<unsigned short>(requested.red * 257U);
  color.green = static_cast<unsigned short>(requested.green * 257U);
  color.blue = static_cast<unsigned short>(requested.blue * 257U);
  color.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(display, colormap, &color) == 0) return false;
  result.pixel = color.pixel;
  result.rgb = {static_cast<std::uint8_t>((color.red + 128U) / 257U),
                static_cast<std::uint8_t>((color.green + 128U) / 257U),
                static_cast<std::uint8_t>((color.blue + 128U) / 257U)};
  return result.rgb == requested;
}

[[nodiscard]] bool write_template(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(template_png.data()),
               static_cast<std::streamsize>(template_png.size()));
  return output.good();
}

[[nodiscard]] layerc::core::GameProfile profile_fixture() {
  using namespace std::chrono_literals;
  layerc::core::GameProfile profile;
  profile.id = "visual-fixture";
  profile.name = "Visual fixture";
  profile.cooldowns = {
      {.id = "pixel", .name = "Pixel", .duration = 60s},
      {.id = "template", .name = "Template", .duration = 60s}};
  return profile;
}

[[nodiscard]] layerc::core::VisualProfile
visual_fixture(const layerc::core::RgbPixel background) {
  using namespace layerc::core;
  VisualProfile profile;
  profile.profile_id = "visual-fixture";
  profile.sample_rate_hz = 20;
  profile.quality = CaptureQuality::exact;
  profile.rules.push_back(
      {.id = "green",
       .name = "Green upper half",
       .region = {0.0, 0.0, 1.0, 0.5},
       .predicate = AverageColorPredicate{
           (static_cast<std::uint32_t>(background.red) << 16U) |
               (static_cast<std::uint32_t>(background.green) << 8U) |
               background.blue,
           0},
       .stable_samples = 1});
  profile.templates.push_back(
      {.id = "pattern",
       .name = "Pattern",
       .search_region = {0.0, 0.5, 1.0, 0.5},
       .expected_width = static_cast<double>(template_axis) / window_axis,
       .expected_height = static_cast<double>(template_axis) / window_axis,
       .references = {{std::string(template_sha)}},
       .confidence_threshold = 0.99,
       .scale_tolerance = 0.0,
       .color_tolerance = 0,
       .stable_samples = 1});
  profile.triggers = {
      {.cooldown_id = "pixel", .observation_id = "green"},
      {.cooldown_id = "template", .observation_id = "pattern"}};
  return profile;
}

} // namespace

int run_linux_x11_visual_tests() {
  using layerc::test::expect;
  bool passed = true;
  if (!layerc::test::x11_display_available()) {
    std::fprintf(stderr, "skip: DISPLAY is not set (run under Xvfb)\n");
    return skip_code;
  }

  std::string display_error;
  auto display = layerc::platform::X11Display::open(display_error);
  if (!display.has_value()) {
    std::fprintf(stderr, "skip: %s\n", display_error.c_str());
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

  TempDirectory temp{std::filesystem::temp_directory_path() /
                     ("layerc-x11-visual-" + std::to_string(::getpid()))};
  std::error_code error;
  std::filesystem::remove_all(temp.path, error);
  error.clear();
  std::filesystem::create_directories(temp.path, error);
  if (error || ::setenv("HOME", temp.path.c_str(), 1) != 0 ||
      ::setenv("XDG_CONFIG_HOME", (temp.path / "config").c_str(), 1) != 0 ||
      ::setenv("XDG_DATA_HOME", (temp.path / "data").c_str(), 1) != 0) {
    std::fprintf(stderr, "skip: cannot create isolated XDG roots\n");
    return skip_code;
  }
  const auto roots = layerc::platform::profile_store_roots();
  if (!layerc::platform::ensure_profile_store_roots(roots)) {
    std::fprintf(stderr, "skip: cannot initialize isolated profile store\n");
    return skip_code;
  }
  const auto profile_path = roots.config_profiles / "visual-fixture.json";
  const auto asset_path =
      layerc::platform::profile_visual_asset_path(profile_path, template_sha);
  passed &= expect(write_template(asset_path),
                   "the canonical PNG fixture is profile-local");
  const auto loaded = layerc::platform::load_profile_template_asset(
      profile_path, template_sha);
  const auto *image = std::get_if<layerc::core::TemplateImage>(&loaded);
  passed &= expect(image != nullptr && image->width == template_axis &&
                       image->height == template_axis &&
                       image->pixels.front() == pattern_pixel(0, 0) &&
                       image->pixels.back() == pattern_pixel(7, 7),
                   "the Linux loader verifies and decodes the canonical PNG");

  auto *raw = reinterpret_cast<Display *>(display->raw());
  const int screen = DefaultScreen(raw);
  const ::Window root = DefaultRootWindow(raw);
  const Colormap colormap = DefaultColormap(raw, screen);
  FixtureColor black;
  FixtureColor background;
  if (!allocate_color(raw, colormap, {0, 0, 0}, black) ||
      !allocate_color(raw, colormap, {32, 160, 64}, background)) {
    std::fprintf(stderr, "skip: fixture needs exact TrueColor allocation\n");
    return skip_code;
  }

  const ::Window window = XCreateSimpleWindow(
      raw, root, 0, 0, window_axis, window_axis, 0, black.pixel, black.pixel);
  const GC gc = XCreateGC(raw, window, 0, nullptr);
  XMapWindow(raw, window);
  XSync(raw, False);
  XWindowAttributes attributes{};
  if (gc == nullptr || XGetWindowAttributes(raw, window, &attributes) == 0 ||
      attributes.visual == nullptr || attributes.visual->c_class != TrueColor) {
    if (gc != nullptr) XFreeGC(raw, gc);
    XDestroyWindow(raw, window);
    std::fprintf(stderr, "skip: fixture needs a TrueColor window\n");
    return skip_code;
  }
  {
    layerc::platform::X11ErrorTrap trap{raw};
    XCompositeRedirectWindow(raw, window, CompositeRedirectAutomatic);
    XSync(raw, False);
    if (!trap.ok()) {
      XFreeGC(raw, gc);
      XDestroyWindow(raw, window);
      std::fprintf(stderr, "skip: cannot redirect fixture window\n");
      return skip_code;
    }
  }

  XSetForeground(raw, gc, background.pixel);
  XFillRectangle(raw, window, gc, 0, 0, window_axis, window_axis);
  std::vector<unsigned long> allocated{black.pixel, background.pixel};
  for (std::uint32_t y = 0; y < template_axis; ++y) {
    for (std::uint32_t x = 0; x < template_axis; ++x) {
      FixtureColor color;
      if (!allocate_color(raw, colormap, pattern_pixel(x, y), color)) {
        std::fprintf(stderr, "skip: fixture cannot allocate template colors\n");
        XFreeGC(raw, gc);
        XDestroyWindow(raw, window);
        return skip_code;
      }
      allocated.push_back(color.pixel);
      XSetForeground(raw, gc, color.pixel);
      XDrawPoint(raw, window, gc, 24 + static_cast<int>(x),
                 40 + static_cast<int>(y));
    }
  }
  XSync(raw, False);

  std::vector<layerc::daemon::InputSession> sessions;
  sessions.emplace_back(
      layerc::platform::StoredProfile{profile_path, profile_fixture()}, window,
      false, visual_fixture(background.rgb));
  sessions.front().active = true;

  layerc::daemon::X11VisualService visual;
  visual.attach(*display);
  const layerc::runtime::SessionCapabilityRequest request{
      .window = {window}, .active = true, .visual = true,
      .profile_id = sessions.front().profile.id};
  auto demand =
      layerc::runtime::calculate_capability_demand(std::span{&request, 1}, false);
  // This fixture exercises the visual owner; input/overlay have their own
  // native-service suites and are intentionally not registered here.
  demand.set(layerc::runtime::Capability::input, 0);
  demand.set(layerc::runtime::Capability::overlay, 0);
  visual.refresh(sessions, demand);
  layerc::runtime::RuntimeServiceRegistry services{};
  services[static_cast<std::size_t>(layerc::runtime::Capability::visual)] = &visual;
  layerc::runtime::DaemonKernel kernel(services);
  passed &= expect(kernel.reconcile(demand),
                   "visual capture activates with its template resources");
  passed &= expect(
      kernel.services()
                   .stats(layerc::runtime::Capability::visual)
                  .resources.decoded_assets == 1,
      "one referenced template is retained");

  const layerc::core::TimePoint now{};
  const auto triggered = visual.poll(sessions, now);
  passed &= expect(std::ranges::find(triggered, "pixel") != triggered.end(),
                   "the captured pixel rule triggers its cooldown");
  passed &= expect(std::ranges::find(triggered, "template") != triggered.end(),
                   "the resolved template matches and triggers its cooldown");
  passed &= expect(!sessions.front().engine.trigger("pixel", now) &&
                       !sessions.front().engine.trigger("template", now),
                   "accepted visual requests consume both cooldowns");
  passed &= expect(
      visual.poll(sessions, now + std::chrono::milliseconds{50}).empty(),
      "held pixel and template edges do not retrigger");

  kernel.quiesce(now + std::chrono::milliseconds{51});
  passed &= expect(kernel.services().aggregate_resources() ==
                       layerc::runtime::RuntimeResourceStats{},
                    "quiescence releases capture and template resources");
  passed &= expect(visual.resources() ==
                       layerc::runtime::RuntimeResourceStats{} &&
                       !visual.next_deadline(now).has_value(),
                   "the native visual owner releases resources and its deadline");

  auto isolated_request = request;
  isolated_request.profile_id = "isolated-consumer";
  isolated_request.isolated_visual = true;
  const layerc::runtime::SessionCapabilityRequest shared_window[] = {
      request, isolated_request};
  auto isolated_demand =
      layerc::runtime::calculate_capability_demand(shared_window, false);
  isolated_demand.set(layerc::runtime::Capability::input, 0);
  isolated_demand.set(layerc::runtime::Capability::overlay, 0);
  visual.refresh(sessions, isolated_demand);
  passed &= expect(!kernel.reconcile(isolated_demand) &&
                       visual.resources() ==
                           layerc::runtime::RuntimeResourceStats{} &&
                       !visual.next_deadline(now).has_value(),
                   "isolated demand on a shared window cannot launch inline capture");
  visual.refresh(sessions, demand);
  passed &= expect(kernel.reconcile(demand) &&
                       visual.resources().decoded_assets == 1,
                   "removing the isolated consumer restores inline capture and assets");
  kernel.quiesce();

  {
    layerc::platform::X11ErrorTrap trap{raw};
    XCompositeUnredirectWindow(raw, window, CompositeRedirectAutomatic);
    XSync(raw, False);
    passed &= expect(trap.ok(), "the fixture removes its Composite redirect");
  }
  XDestroyWindow(raw, window);
  XFreeGC(raw, gc);
  XFreeColors(raw, colormap, allocated.data(),
              static_cast<int>(allocated.size()), 0);
  XSync(raw, False);

  std::printf("x11 visual tests: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}

int main() {
  return layerc::test::run({{"linux_x11_visual_tests", [] {
                              const int result = run_linux_x11_visual_tests();
                              if (result == skip_code) layerc::test::skip();
                              layerc::test::expect(
                                  result == 0,
                                  "the X11 visual program should succeed");
                            }}});
}
