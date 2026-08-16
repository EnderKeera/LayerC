#include "test_support.hpp"

#include "layerc/capture/window_capture.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

int main() {
  using layerc::capture::region_is_available;
  using layerc::core::NormalizedRegion;
  using layerc::test::expect;
  expect(region_is_available({0.2, 0.2, 0.2, 0.2},
                             {0.1, 0.1, 0.8, 0.8}),
         "a rule inside the fallback output should remain available");
  expect(!region_is_available({0.0, 0.2, 0.2, 0.2},
                              {0.1, 0.1, 0.8, 0.8}),
         "a rule crossing the fallback output should be unavailable");
  expect(region_is_available({0.0, 0.0, 1.0, 1.0},
                             {0.0, 0.0, 1.0, 1.0}),
         "a full WGC frame should contain every normalized rule");
  constexpr double epsilon = 1e-9;
  expect(region_is_available({0.1 - epsilon * 0.5, 0.1 - epsilon * 0.5,
                              0.2, 0.2},
                             {0.1, 0.1, 0.8, 0.8}),
         "availability tolerance should expand the left and top symmetrically");
  expect(!region_is_available({0.1 - epsilon * 2.0, 0.1, 0.2, 0.2},
                              {0.1, 0.1, 0.8, 0.8}),
         "regions beyond the symmetric tolerance should remain unavailable");
  expect(layerc::capture::capture_sample_dimensions(
             1920, 1080, layerc::core::CaptureQuality::low) ==
             std::pair<std::uint32_t, std::uint32_t>{64, 36},
         "low capture should use an aspect-preserving 64-pixel bound");
  expect(layerc::capture::capture_sample_dimensions(
             3840, 2160, layerc::core::CaptureQuality::balanced) ==
             std::pair<std::uint32_t, std::uint32_t>{256, 144},
         "balanced capture should use an aspect-preserving 256-pixel bound");
  expect(layerc::capture::capture_sample_dimensions(
             3840, 2160, layerc::core::CaptureQuality::exact) ==
             std::pair<std::uint32_t, std::uint32_t>{3840, 2160},
         "exact capture should retain every source pixel");
  using layerc::capture::OutputRotation;
  using layerc::capture::PixelBounds;
  using layerc::capture::transform_output_crop;
  const PixelBounds crop{100, 200, 300, 500};
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::identity) == crop,
         "identity outputs should retain logical crop coordinates");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate90) ==
             PixelBounds{200, 780, 500, 980},
         "90-degree outputs should swap axes and reverse logical X");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate180) ==
             PixelBounds{780, 1420, 980, 1720},
         "180-degree outputs should reverse both axes");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate270) ==
             PixelBounds{1420, 100, 1720, 300},
         "270-degree outputs should swap axes and reverse logical Y");

  layerc::capture::CaptureFrame frame;
  frame.available_region = {0.0, 0.0, 1.0, 1.0};
  frame.unavailable_regions.push_back({0.4, 0.4, 0.2, 0.2});
  expect(!region_is_available({0.45, 0.45, 0.1, 0.1}, frame),
         "rules overlapping excluded overlay pixels should be unavailable");
  expect(region_is_available({0.2, 0.2, 0.2, 0.2}, frame),
         "rules touching but not entering an excluded region should remain available");

  layerc::capture::CaptureFrame black_frame;
  black_frame.width = 512;
  black_frame.height = 512;
  black_frame.pixels.resize(
      static_cast<std::size_t>(black_frame.width) * black_frame.height);
  expect(layerc::capture::frame_is_nearly_black(black_frame),
         "a genuinely black frame should remain protected");
  const layerc::core::RgbPixel white{255, 255, 255};
  for (std::uint32_t x = 0; x < black_frame.width; ++x) {
    black_frame.pixels[x] = white;
    black_frame.pixels[(black_frame.height - 1U) * black_frame.width + x] =
        white;
  }
  for (std::uint32_t y = 0; y < black_frame.height; ++y) {
    black_frame.pixels[static_cast<std::size_t>(y) * black_frame.width] = white;
    black_frame.pixels[static_cast<std::size_t>(y) * black_frame.width +
                       black_frame.width - 1U] = white;
  }
  expect(!layerc::capture::frame_is_nearly_black(black_frame),
         "sparse bright window borders should not be missed by black-frame sampling");
  if (layerc::test::failures != 0) {
    std::cerr << layerc::test::failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All LayerC capture tests passed\n";
  return EXIT_SUCCESS;
}
