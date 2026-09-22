// Linux side of the platform visual-asset contract: profile-local template
// loading with a miniz PNG decoder. The daemon visual service consumes only
// the verified TemplateImageResult; no parser or storage policy lives in
// the daemon.

#include "layerc/platform/visual_assets.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/platform/profile_store.hpp"

#include "miniz.h"
#include "miniz_tinfl.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>

namespace layerc::platform {
namespace {

constexpr std::array<std::uint8_t, 8> png_signature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
constexpr std::uint32_t maximum_template_axis = 512U;
constexpr std::size_t png_channel_count = 4U;

[[nodiscard]] TemplateImageResult failure(std::string message) {
  return VisualAssetError{.message = std::move(message)};
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
read_profile_bytes(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > max_template_asset_bytes)
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
    return std::nullopt;
  return bytes;
}

[[nodiscard]] std::uint8_t paeth(const std::uint8_t left,
                                 const std::uint8_t above,
                                 const std::uint8_t upper_left) noexcept {
  const int estimate = static_cast<int>(left) + static_cast<int>(above) -
                       static_cast<int>(upper_left);
  const int left_distance = std::abs(estimate - static_cast<int>(left));
  const int above_distance = std::abs(estimate - static_cast<int>(above));
  const int diagonal_distance =
      std::abs(estimate - static_cast<int>(upper_left));
  if (left_distance <= above_distance && left_distance <= diagonal_distance)
    return left;
  return above_distance <= diagonal_distance ? above : upper_left;
}

[[nodiscard]] TemplateImageResult
decode_canonical_png(const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() < png_signature.size() + 12U ||
      !std::equal(png_signature.begin(), png_signature.end(), bytes.begin()))
    return failure("asset is not a PNG image");

  std::uint32_t width{};
  std::uint32_t height{};
  bool saw_header{};
  bool saw_data{};
  bool saw_end{};
  std::vector<std::uint8_t> compressed;
  std::size_t cursor = png_signature.size();
  while (cursor <= bytes.size() - 12U) {
    const auto length =
        static_cast<std::size_t>(read_u32(bytes.data() + cursor));
    if (length > bytes.size() - cursor - 12U)
      return failure("PNG chunk exceeds the asset bounds");
    const auto *type = bytes.data() + cursor + 4U;
    const auto *data = type + 4U;
    const auto expected_crc = read_u32(data + length);
    const auto actual_crc =
        static_cast<std::uint32_t>(mz_crc32(0, type, length + 4U));
    if (actual_crc != expected_crc)
      return failure("PNG chunk checksum is invalid");

    const std::array<char, 4> name{static_cast<char>(type[0]),
                                   static_cast<char>(type[1]),
                                   static_cast<char>(type[2]),
                                   static_cast<char>(type[3])};
    if (name == std::array<char, 4>{'I', 'H', 'D', 'R'}) {
      if (saw_header || saw_data || length != 13U)
        return failure("PNG header is malformed");
      width = read_u32(data);
      height = read_u32(data + 4U);
      if (width == 0 || height == 0 || width > maximum_template_axis ||
          height > maximum_template_axis)
        return failure("template dimensions must be between 1 and 512 pixels");
      if (data[8] != 8U || data[9] != 6U || data[10] != 0U ||
          data[11] != 0U || data[12] != 0U)
        return failure(
            "PNG must be non-interlaced 8-bit RGBA with standard compression");
      saw_header = true;
    } else if (name == std::array<char, 4>{'I', 'D', 'A', 'T'}) {
      if (!saw_header || saw_end ||
          length > max_template_asset_bytes - compressed.size())
        return failure("PNG image data is malformed or too large");
      compressed.insert(compressed.end(), data, data + length);
      saw_data = true;
    } else if (name == std::array<char, 4>{'I', 'E', 'N', 'D'}) {
      if (!saw_header || !saw_data || saw_end || length != 0U)
        return failure("PNG end marker is malformed");
      saw_end = true;
    } else if ((type[0] & 0x20U) == 0U) {
      return failure("PNG contains an unsupported critical chunk");
    }

    cursor += length + 12U;
    if (saw_end) break;
  }
  if (!saw_end || cursor != bytes.size())
    return failure("PNG is truncated or has trailing data");

  const auto row_bytes = static_cast<std::size_t>(width) * png_channel_count;
  const auto filtered_size =
      (row_bytes + 1U) * static_cast<std::size_t>(height);
  std::vector<std::uint8_t> filtered(filtered_size);
  const auto decoded = tinfl_decompress_mem_to_mem(
      filtered.data(), filtered.size(), compressed.data(), compressed.size(),
      TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (decoded == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
      decoded != filtered.size())
    return failure("PNG image data could not be decompressed");

  std::vector<std::uint8_t> rgba(row_bytes * height);
  for (std::size_t y = 0; y < height; ++y) {
    const auto filter = filtered[y * (row_bytes + 1U)];
    if (filter > 4U) return failure("PNG uses an unsupported row filter");
    const auto *source = filtered.data() + y * (row_bytes + 1U) + 1U;
    auto *row = rgba.data() + y * row_bytes;
    const auto *previous = y == 0 ? nullptr : row - row_bytes;
    for (std::size_t x = 0; x < row_bytes; ++x) {
      const std::uint8_t left =
          x < png_channel_count ? 0U : row[x - png_channel_count];
      const std::uint8_t above = previous == nullptr ? 0U : previous[x];
      const std::uint8_t upper_left =
          previous == nullptr || x < png_channel_count
              ? 0U
              : previous[x - png_channel_count];
      std::uint8_t predictor{};
      switch (filter) {
      case 0: break;
      case 1: predictor = left; break;
      case 2: predictor = above; break;
      case 3:
        predictor = static_cast<std::uint8_t>(
            (static_cast<unsigned>(left) + static_cast<unsigned>(above)) / 2U);
        break;
      case 4: predictor = paeth(left, above, upper_left); break;
      default: break;
      }
      row[x] = static_cast<std::uint8_t>(source[x] + predictor);
    }
  }

  layerc::core::TemplateImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height);
  image.alpha.resize(image.pixels.size());
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    image.pixels[index] = {rgba[index * 4U], rgba[index * 4U + 1U],
                           rgba[index * 4U + 2U]};
    image.alpha[index] = rgba[index * 4U + 3U];
  }
  if (!image.valid())
    return failure("template requires at least 64 nontransparent pixels");
  if (!layerc::core::template_image_has_structure(image))
    return failure("template asset lacks enough visual structure for matching");
  return image;
}

} // namespace

TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            const std::string_view sha256) {
  if (!layerc::core::is_lowercase_sha256(sha256))
    return failure("template asset id is not a lower-case SHA-256 digest");
  const auto path = profile_visual_asset_path(profile_path, sha256);
  auto bytes = read_profile_bytes(path);
  if (!bytes.has_value())
    return failure("template asset is missing or exceeds the 4 MiB limit");
  if (layerc::core::sha256(std::as_bytes(std::span(*bytes))) != sha256)
    return failure("template asset hash does not match its profile reference");
  auto decoded = decode_canonical_png(*bytes);
  if (auto *image = std::get_if<layerc::core::TemplateImage>(&decoded))
    image->asset_sha256 = std::string(sha256);
  return decoded;
}

// Linux has no NTFS change journal. Content identity falls back to the
// metadata fields filled by callers that can observe the file.
VisualAssetSignature asset_signature(const std::string_view) {
  return {};
}

} // namespace layerc::platform
