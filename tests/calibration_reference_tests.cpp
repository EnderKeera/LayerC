#include "calibration_window.hpp"
#include "calibration_mode.hpp"
#include "calibration_resource.h"
#include "test_support.hpp"

#include "layerc/platform/visual_assets_windows.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace layerc::calibration {

// No calibration process, live capture, asset files or production settings are
// opened. Both production reference paths share this acceptance operation;
// capture additionally exercises the real crop/downscale operation.
struct CalibrationWindowTestAccess {
  static void select(CalibrationWindow &window,
                     const layerc::core::NormalizedRegion region) {
    window.set_reference_selection(region);
  }
  static void load(CalibrationWindow &window,
                   const layerc::core::TemplateState &state) {
    window.template_state_ = state;
    window.restore_reference_selection();
  }
  static void frame(CalibrationWindow &window,
                    std::shared_ptr<const layerc::capture::CaptureFrame> frame) {
    window.preview_.set_frame(std::move(frame));
  }
  static auto capture(const CalibrationWindow &window) {
    return window.captured_reference_image();
  }
  static bool append(CalibrationWindow &window,
                     const layerc::platform::ImportedTemplateAsset &asset) {
    return window.append_template_reference(asset);
  }
  static const auto &state(const CalibrationWindow &window) {
    return window.template_state_;
  }
  static auto controls(const CalibrationWindow &window) {
    return window.current_template_from_controls();
  }
  static const auto &selection(const CalibrationWindow &window) {
    return window.reference_selection_;
  }
  static const auto &images(const CalibrationWindow &window) {
    return window.template_images_;
  }
  static void attach(CalibrationWindow &window, HWND dialog) {
    window.dialog_ = dialog;
    window.rule_.id = "readiness-pixel";
    window.rule_.name = "Readiness pixel";
    window.rule_.region = {0.0, 0.0, 1.0, 1.0};
    window.target_color_ = 0xFFFFFFU;
    window.quality_ = layerc::core::CaptureQuality::balanced;
    window.initialize_controls();
  }
  static bool ready(const CalibrationWindow &window) {
    return window.sampling_ready();
  }
  static void evaluate(CalibrationWindow &window) {
    window.update_match_status();
  }
  static void quality(CalibrationWindow &window,
                      layerc::core::CaptureQuality value) {
    window.quality_ = value;
    window.refresh_apply_readiness();
  }
  static void redraw(CalibrationWindow &window, HWND preview,
                     POINT start, POINT end) {
    window.preview_.attach(preview);
    window.preview_.begin_drag(start);
    window.update_region_from_pointer(end, true);
    window.preview_.finish_drag();
    window.preview_.attach(nullptr);
  }
};

} // namespace layerc::calibration

namespace {

using layerc::calibration::CalibrationWindow;
using Access = layerc::calibration::CalibrationWindowTestAccess;

INT_PTR CALLBACK readiness_dialog_proc(HWND, UINT message, WPARAM, LPARAM) {
  return message == WM_INITDIALOG ? TRUE : FALSE;
}
struct ReadinessDialog final {
  HWND handle = CreateDialogParamW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CALIBRATION), nullptr,
      readiness_dialog_proc, 0);
  ~ReadinessDialog() { if (handle != nullptr) DestroyWindow(handle); }
};
using layerc::core::NormalizedRegion;
using layerc::test::expect;

constexpr NormalizedRegion selected{0.25, 0.375, 0.125, 0.25};
constexpr NormalizedRegion search{0.1875, 0.25, 0.25, 0.5};

std::shared_ptr<layerc::capture::CaptureFrame> frame(std::uint8_t seed) {
  auto image = std::make_shared<layerc::capture::CaptureFrame>();
  image->width = image->height = 128;
  image->pixels.resize(128U * 128U);
  for (std::uint32_t y = 0; y < 128; ++y)
    for (std::uint32_t x = 0; x < 128; ++x)
      image->pixels[y * 128U + x] = {static_cast<std::uint8_t>(x),
                                    static_cast<std::uint8_t>(y), seed};
  return image;
}

layerc::platform::ImportedTemplateAsset asset(
    const char identity, layerc::core::TemplateImage image) {
  const std::string hash(64, identity);
  image.asset_sha256 = hash;
  return {.reference = {.asset_sha256 = hash},
          .image = std::move(image), .path = {}};
}

layerc::platform::ImportedTemplateAsset imported(const char identity) {
  layerc::core::TemplateImage image;
  image.width = image.height = 16;
  image.pixels.resize(256);
  image.alpha.assign(256, 255);
  for (std::uint32_t y = 0; y < 16; ++y)
    for (std::uint32_t x = 0; x < 16; ++x)
      image.pixels[y * 16U + x] = {static_cast<std::uint8_t>(x * 8U),
                                  static_cast<std::uint8_t>(y * 8U), 40};
  return asset(identity, std::move(image));
}

void expect_geometry(const CalibrationWindow &window,
                     const NormalizedRegion crop,
                     const NormalizedRegion expected_search) {
  const auto &state = Access::state(window);
  expect(Access::selection(window) == crop &&
             state.expected_width == crop.width &&
             state.expected_height == crop.height &&
             state.search_region == expected_search,
         "references must preserve the explicit crop and expected/search geometry");
  expect(Access::controls(window).search_region == expected_search,
         "reading/applying controls must not replace search with the crop");
}

auto snapshot(const CalibrationWindow &window) {
  return std::tuple{Access::state(window), Access::images(window),
                    Access::selection(window)};
}

} // namespace

void calibration_reference_geometry_contracts() {
  CalibrationWindow window({});
  Access::select(window, selected);
  for (std::uint8_t index = 0; index < 2; ++index) {
    Access::frame(window, frame(index));
    auto crop = Access::capture(window);
    if (!expect(crop && crop->width == 16 && crop->height == 32 &&
                    crop->pixels.front().red == 32 &&
                    crop->pixels.front().green == 48,
                "successive captures must crop the same selected source pixels"))
      return;
    expect(Access::append(window,
                          asset(static_cast<char>('a' + index), std::move(*crop))),
           "distinct captured references must be accepted");
    expect_geometry(window, selected, search);
  }
  for (const char identity : {'c', 'd'}) {
    expect(Access::append(window, imported(identity)),
           "distinct imports must be accepted without recapturing the search area");
    expect_geometry(window, selected, search);
  }
  expect(Access::state(window).references.size() == 4 &&
             Access::images(window).size() == 4,
         "two captures and two imports must retain all four reference images");
  const auto references = Access::state(window).references;
  const auto images = Access::images(window);
  const HWND preview = CreateWindowExW(
      0, L"STATIC", L"Disposable calibration selection", WS_POPUP,
      0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (preview == nullptr) {
    expect(false, "the hidden selection fixture must exist");
    return;
  }
  Access::redraw(window, preview, {16, 32}, {48, 64});
  DestroyWindow(preview);
  expect_geometry(window, {0.125, 0.25, 0.25, 0.25}, {0.0, 0.125, 0.5, 0.5});
  expect(Access::state(window).references == references &&
             Access::images(window) == images,
         "only an explicit redraw may replace geometry, never the references");
  const auto crop = Access::capture(window);
  expect(crop && crop->width == 32 && crop->height == 32,
         "the next capture must honor the explicitly redrawn crop");
}

void calibration_reference_loaded_contracts() {
  CalibrationWindow window({});
  layerc::core::TemplateState state;
  state.id = "loaded-reference";
  state.name = "Loaded reference";
  state.search_region = {0.125, 0.125, 0.75, 0.5};
  state.expected_width = 0.125;
  state.expected_height = 0.25;
  expect(Access::append(window, imported('a')),
         "the loaded fixture must already own a reference");
  state.references = Access::state(window).references;
  Access::load(window, state);
  const NormalizedRegion crop{0.4375, 0.25, 0.125, 0.25};
  for (const char identity : {'b', 'c'}) {
    expect(Access::append(window, imported(identity)),
           "a loaded calibration may append references");
    expect_geometry(window, crop, state.search_region);
  }
  Access::frame(window, frame(1));
  const auto captured = Access::capture(window);
  expect(captured && captured->width == 16 && captured->height == 32,
         "a restored crop must use persisted expected dimensions, not search size");
  expect(Access::state(window).references.size() == 3 &&
             Access::images(window).size() == 3,
         "new references must not replace the loaded reference");

  CalibrationWindow narrow_window({});
  state.search_region = {0.0, 0.0, 0.125, 0.125};
  state.expected_width = state.expected_height = 0.25;
  Access::load(narrow_window, state);
  expect_geometry(narrow_window, {0.0, 0.0, 0.25, 0.25}, state.search_region);
  expect(layerc::core::valid_normalized_region(Access::selection(narrow_window)),
         "restoring an unusual search must still produce an in-bounds crop");
}

void calibration_reference_negative_contracts() {
  CalibrationWindow window({});
  Access::select(window, selected);
  const auto before_empty_capture = snapshot(window);
  expect(!Access::capture(window) && snapshot(window) == before_empty_capture,
         "a missing frame must not change calibration state");
  const auto first = imported('a');
  expect(Access::append(window, first), "the first import must be accepted");
  const auto before_duplicate = snapshot(window);
  expect(!Access::append(window, first) && snapshot(window) == before_duplicate,
         "a duplicate import/capture must be a complete no-op");
  for (const char identity : {'b', 'c', 'd'})
    expect(Access::append(window, imported(identity)),
           "four distinct references must be accepted");
  const auto before_limit = snapshot(window);
  expect(!Access::append(window, imported('e')) && snapshot(window) == before_limit,
         "a fifth reference must not change geometry, references or image state");
  expect_geometry(window, selected, search);

  Access::frame(window, frame(2));
  const NormalizedRegion edge{0.9375, 0.875, 0.0625, 0.125};
  Access::select(window, edge);
  expect_geometry(window, edge, {0.875, 0.75, 0.125, 0.25});
  const auto crop = Access::capture(window);
  expect(crop && crop->width == 8 && crop->height == 16 &&
             layerc::core::valid_normalized_region(
                 Access::state(window).search_region),
         "edge expansion must stay in bounds and leave the crop unchanged");
  Access::select(window, {0.99, 0.99, 0.001, 0.001});
  const auto before_small_capture = snapshot(window);
  expect(!Access::capture(window) && snapshot(window) == before_small_capture,
         "a crop with fewer than 64 pixels must fail without installing a reference");
}

void calibration_sampling_readiness_contracts() {
  using layerc::core::CaptureQuality;
  ReadinessDialog dialog;
  if (!expect(dialog.handle != nullptr,
              "the hidden calibration readiness resource must load")) return;
  CalibrationWindow window({});
  Access::attach(window, dialog.handle);
  const HWND apply = GetDlgItem(dialog.handle, IDC_CAL_APPLY);
  expect(!Access::ready(window) && !IsWindowEnabled(apply),
         "untouched native calibration must disable Apply");
  auto captured = frame(11);
  captured->available_region = {0.0, 0.0, 1.0, 1.0};
  Access::frame(window, captured);
  Access::evaluate(window);
  expect(Access::ready(window) && IsWindowEnabled(apply),
         "a genuinely evaluated negative pixel result enables native Apply");
  Access::quality(window, CaptureQuality::low);
  expect(!Access::ready(window) && !IsWindowEnabled(apply),
         "native quality changes cannot reuse differently sampled evidence");
  Access::evaluate(window);
  expect(Access::ready(window) && IsWindowEnabled(apply),
         "native reevaluation records the newly selected quality");
  Access::frame(window, nullptr);
  Access::evaluate(window);
  expect(!Access::ready(window) && !IsWindowEnabled(apply),
         "clearing the native preview clears sampling readiness");
  layerc::core::TemplateState state;
  state.id = "readiness-template";
  state.name = "Readiness template";
  state.references.push_back({.asset_sha256 = std::string(64, 'a')});
  Access::load(window, state);
  SendDlgItemMessageW(dialog.handle, IDC_CAL_PREDICATE, CB_SETCURSEL,
      layerc::calibration::calibration_mode_index(
          layerc::calibration::CalibrationMode::template_match), 0);
  Access::evaluate(window);
  expect(!Access::ready(window) && !IsWindowEnabled(apply),
         "loaded native references without a captured frame are not evidence");
  Access::frame(window, captured);
  Access::evaluate(window);
  expect(!Access::ready(window) && !IsWindowEnabled(apply),
         "missing native template image bytes cannot enable Apply");
}
