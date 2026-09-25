#pragma once

// Shared contract for the platform-neutral template-asset loader
// (`verified_template_from_bytes`). Both platform suites run it so the
// guarded read, size bound, hash check, and decode/structure gates cannot
// drift between Windows and Linux.

#include "layerc/core/hash.hpp"
#include "layerc/platform/visual_assets.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace layerc::test {
namespace visual_asset_contract {

inline layerc::core::TemplateImage contract_image(const bool structured) {
  layerc::core::TemplateImage image{
      .asset_sha256 = {},
      .width = 8,
      .height = 8,
      .pixels = std::vector<layerc::core::RgbPixel>(64),
      .alpha = std::vector<std::uint8_t>(64, 255)};
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    const auto value = structured ? index : 0U;
    image.pixels[index] = {static_cast<std::uint8_t>(value * 3U),
                           static_cast<std::uint8_t>(255U - value * 2U),
                           static_cast<std::uint8_t>(value * 4U)};
  }
  return image;
}

inline layerc::platform::TemplateImageResult
decode_structured(std::vector<std::uint8_t> &) {
  return contract_image(true);
}

inline layerc::platform::TemplateImageResult
decode_flat(std::vector<std::uint8_t> &) {
  return contract_image(false);
}

inline layerc::platform::TemplateImageResult
decode_failure(std::vector<std::uint8_t> &) {
  return layerc::platform::VisualAssetError{"contract decoder rejected bytes"};
}

inline bool write_bytes(const std::filesystem::path &path,
                        const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

inline std::string digest(const std::string &bytes) {
  return layerc::core::sha256(
      std::as_bytes(std::span(bytes.data(), bytes.size())));
}

inline bool fails_with(const layerc::platform::TemplateImageResult &result,
                       const std::string_view fragment) {
  const auto *error = std::get_if<layerc::platform::VisualAssetError>(&result);
  return error != nullptr &&
         error->message.find(fragment) != std::string::npos;
}

} // namespace visual_asset_contract

inline void visual_asset_load_contract(const std::filesystem::path &directory) {
  using namespace visual_asset_contract;
  using layerc::platform::verified_template_from_bytes;

  const std::string payload = "template-asset-contract";
  const auto payload_sha = digest(payload);
  const auto asset = directory / "contract-asset.png";
  expect(write_bytes(asset, payload), "contract asset should be written");

  expect(fails_with(verified_template_from_bytes(asset, "NOT-A-DIGEST",
                                                 decode_structured),
                    "SHA-256"),
         "a malformed asset id is rejected before any read");
  expect(fails_with(verified_template_from_bytes(directory / "missing.png",
                                                 payload_sha, decode_structured),
                    "missing"),
         "a missing asset reports the guarded-read failure");

  const auto oversized = directory / "oversized.png";
  const std::string large(layerc::platform::max_template_asset_bytes + 1U, 'x');
  expect(write_bytes(oversized, large), "oversized fixture should be written");
  expect(fails_with(verified_template_from_bytes(oversized, digest(large),
                                                 decode_structured),
                    "4 MiB"),
         "an asset above the 4 MiB bound is rejected");

  expect(fails_with(verified_template_from_bytes(asset, digest("other bytes"),
                                                 decode_structured),
                    "hash does not match"),
         "an asset whose bytes do not match its id is rejected");
  expect(fails_with(verified_template_from_bytes(asset, payload_sha,
                                                 decode_failure),
                    "contract decoder rejected bytes"),
         "decoder failures are reported unchanged");
  expect(fails_with(verified_template_from_bytes(asset, payload_sha,
                                                 decode_flat),
                    "visual structure"),
         "flat decoded images are rejected for positional matching");

  const auto loaded =
      verified_template_from_bytes(asset, payload_sha, decode_structured);
  const auto *image = std::get_if<layerc::core::TemplateImage>(&loaded);
  expect(image != nullptr && image->asset_sha256 == payload_sha,
         "a verified asset decodes and records its content id");

#ifndef _WIN32
  // Creating symlinks needs privileges on Windows; the Win32 read path opens
  // reparse points without following them (FILE_FLAG_OPEN_REPARSE_POINT).
  const auto linked = directory / "linked-asset.png";
  std::error_code error;
  std::filesystem::create_symlink(asset, linked, error);
  expect(!error && fails_with(verified_template_from_bytes(
                                  linked, payload_sha, decode_structured),
                              "missing"),
         "a symlinked asset is not followed by the guarded read");
#endif
}

} // namespace layerc::test
