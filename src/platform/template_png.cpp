// Template-asset PNG codec shared by the Windows and Linux stores. Decoding
// verifies chunk CRCs, rejects unknown critical chunks and trailing data,
// and bounds every size before allocating; encoding is deterministic miniz.

#include "layerc/platform/template_png.hpp"

#include "miniz.h"
#include "miniz_tdef.h"
#include "miniz_tinfl.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

using layerc::core::max_template_axis;

constexpr std::array<std::uint8_t, 8> png_signature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
// Changing the level changes every canonical asset hash.
constexpr mz_uint canonical_level = MZ_DEFAULT_LEVEL;

enum class ColorType : std::uint8_t {
  gray = 0,
  rgb = 2,
  palette = 3,
  gray_alpha = 4,
  rgba = 6,
};

struct Header {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint8_t depth{};
  ColorType color{};
  bool interlaced{};
};

struct ParsedPng {
  Header header;
  std::vector<std::uint8_t> compressed;
  std::vector<std::array<std::uint8_t, 4>> palette;
  // tRNS key for gray ([0]) or RGB images.
  std::optional<std::array<std::uint16_t, 3>> transparent;
};

struct Pass {
  std::uint32_t x0, y0, dx, dy;
};

constexpr std::array<Pass, 7> adam7_passes{{{0, 0, 8, 8},
                                            {4, 0, 8, 8},
                                            {0, 4, 4, 8},
                                            {2, 0, 4, 4},
                                            {0, 2, 2, 4},
                                            {1, 0, 2, 2},
                                            {0, 1, 1, 2}}};
constexpr std::array<Pass, 1> whole_image{{{0, 0, 1, 1}}};

[[nodiscard]] VisualAssetError failure(std::string message) {
  return {.message = std::move(message)};
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
}

[[nodiscard]] bool chunk_is(const std::uint8_t *type,
                            const char (&name)[5]) noexcept {
  return std::equal(type, type + 4, name,
                    [](const std::uint8_t byte, const char letter) {
                      return byte == static_cast<std::uint8_t>(letter);
                    });
}

[[nodiscard]] std::size_t channel_count(const ColorType color) noexcept {
  switch (color) {
  case ColorType::gray:
  case ColorType::palette: return 1;
  case ColorType::gray_alpha: return 2;
  case ColorType::rgb: return 3;
  case ColorType::rgba: return 4;
  }
  return 0;
}

[[nodiscard]] bool depth_allowed(const std::uint8_t color,
                                 const std::uint8_t depth) noexcept {
  const bool low = depth == 1 || depth == 2 || depth == 4;
  switch (color) {
  case 0: return low || depth == 8 || depth == 16;
  case 3: return low || depth == 8;
  case 2:
  case 4:
  case 6: return depth == 8 || depth == 16;
  default: return false;
  }
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

[[nodiscard]] std::variant<Header, VisualAssetError>
read_header(const std::uint8_t *data, const std::size_t length) {
  if (length != 13U) return failure("PNG header is malformed");
  Header header{.width = read_u32(data),
                .height = read_u32(data + 4U),
                .depth = data[8],
                .color = static_cast<ColorType>(data[9]),
                .interlaced = data[12] == 1U};
  if (header.width == 0 || header.height == 0 ||
      header.width > max_template_axis || header.height > max_template_axis)
    return failure("template dimensions must be between 1 and " +
                   std::to_string(max_template_axis) + " pixels");
  if (!depth_allowed(data[9], data[8]) || data[10] != 0U || data[11] != 0U ||
      data[12] > 1U)
    return failure("PNG uses an unsupported color type, depth, or method");
  return header;
}

[[nodiscard]] std::variant<ParsedPng, VisualAssetError>
parse_chunks(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < png_signature.size() + 12U ||
      !std::equal(png_signature.begin(), png_signature.end(), bytes.begin()))
    return failure("asset is not a PNG image");

  ParsedPng png;
  bool saw_header{};
  bool saw_data{};
  bool saw_end{};
  std::size_t cursor = png_signature.size();
  while (cursor <= bytes.size() - 12U) {
    const auto length =
        static_cast<std::size_t>(read_u32(bytes.data() + cursor));
    if (length > bytes.size() - cursor - 12U)
      return failure("PNG chunk exceeds the asset bounds");
    const auto *type = bytes.data() + cursor + 4U;
    const auto *data = type + 4U;
    if (static_cast<std::uint32_t>(mz_crc32(0, type, length + 4U)) !=
        read_u32(data + length))
      return failure("PNG chunk checksum is invalid");
    if (!saw_header && !chunk_is(type, "IHDR"))
      return failure("PNG header is malformed");

    if (chunk_is(type, "IHDR")) {
      if (saw_header) return failure("PNG header is malformed");
      auto header = read_header(data, length);
      if (auto *error = std::get_if<VisualAssetError>(&header)) return *error;
      png.header = std::get<Header>(header);
      saw_header = true;
    } else if (chunk_is(type, "PLTE")) {
      const auto color = png.header.color;
      if (color == ColorType::gray || color == ColorType::gray_alpha ||
          saw_data || !png.palette.empty() || length == 0 ||
          length % 3U != 0 || length / 3U > 256U ||
          (color == ColorType::palette &&
           length / 3U > (std::size_t{1} << png.header.depth)))
        return failure("PNG palette is malformed");
      for (std::size_t index = 0; index < length; index += 3U)
        png.palette.push_back(
            {data[index], data[index + 1U], data[index + 2U], 0xffU});
    } else if (chunk_is(type, "tRNS")) {
      const auto color = png.header.color;
      const bool valid =
          !saw_data && !png.transparent.has_value() &&
          ((color == ColorType::palette && !png.palette.empty() &&
            length <= png.palette.size()) ||
           (color == ColorType::gray && length == 2U) ||
           (color == ColorType::rgb && length == 6U));
      if (!valid) return failure("PNG transparency is malformed");
      if (color == ColorType::palette) {
        for (std::size_t index = 0; index < length; ++index)
          png.palette[index][3] = data[index];
      } else if (color == ColorType::gray) {
        png.transparent = std::array<std::uint16_t, 3>{read_u16(data), 0, 0};
      } else {
        png.transparent = std::array<std::uint16_t, 3>{
            read_u16(data), read_u16(data + 2U), read_u16(data + 4U)};
      }
    } else if (chunk_is(type, "IDAT")) {
      if (saw_end || length > max_template_asset_bytes - png.compressed.size())
        return failure("PNG image data is malformed or too large");
      png.compressed.insert(png.compressed.end(), data, data + length);
      saw_data = true;
    } else if (chunk_is(type, "IEND")) {
      if (!saw_data || length != 0U)
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
  if (png.header.color == ColorType::palette && png.palette.empty())
    return failure("PNG palette is missing");
  return png;
}

// One sample of `depth` bits at sample position `index` within a row.
[[nodiscard]] std::uint16_t sample(const std::uint8_t *row,
                                   const std::size_t index,
                                   const std::uint8_t depth) noexcept {
  if (depth == 16U) return read_u16(row + index * 2U);
  if (depth == 8U) return row[index];
  const std::size_t bit = index * depth;
  const auto shift = 8U - depth - static_cast<unsigned>(bit % 8U);
  return static_cast<std::uint16_t>((row[bit / 8U] >> shift) &
                                    ((1U << depth) - 1U));
}

[[nodiscard]] std::uint8_t to_8bit(const std::uint16_t value,
                                   const std::uint8_t depth) noexcept {
  if (depth == 16U) return static_cast<std::uint8_t>(value >> 8U);
  if (depth == 8U) return static_cast<std::uint8_t>(value);
  return static_cast<std::uint8_t>(value * 255U / ((1U << depth) - 1U));
}

// Writes pixel `x` of one unfiltered row as RGBA; false on a palette index
// outside the palette.
[[nodiscard]] bool write_pixel(const ParsedPng &png, const std::uint8_t *row,
                               const std::size_t x, std::uint8_t *out) {
  const auto &header = png.header;
  const auto depth = header.depth;
  const auto channels = channel_count(header.color);
  const auto at = [&](const std::size_t channel) {
    return sample(row, x * channels + channel, depth);
  };
  switch (header.color) {
  case ColorType::palette: {
    const auto index = at(0);
    if (index >= png.palette.size()) return false;
    std::copy(png.palette[index].begin(), png.palette[index].end(), out);
    return true;
  }
  case ColorType::gray:
  case ColorType::gray_alpha: {
    const auto gray = at(0);
    out[0] = out[1] = out[2] = to_8bit(gray, depth);
    if (header.color == ColorType::gray_alpha)
      out[3] = to_8bit(at(1), depth);
    else
      out[3] = png.transparent.has_value() && gray == (*png.transparent)[0]
                   ? std::uint8_t{0}
                   : std::uint8_t{0xff};
    return true;
  }
  case ColorType::rgb:
  case ColorType::rgba: {
    const std::array<std::uint16_t, 3> rgb{at(0), at(1), at(2)};
    for (std::size_t channel = 0; channel < 3U; ++channel)
      out[channel] = to_8bit(rgb[channel], depth);
    if (header.color == ColorType::rgba)
      out[3] = to_8bit(at(3), depth);
    else
      out[3] = png.transparent == rgb ? std::uint8_t{0} : std::uint8_t{0xff};
    return true;
  }
  }
  return false;
}

[[nodiscard]] std::uint32_t pass_extent(const std::uint32_t size,
                                        const std::uint32_t start,
                                        const std::uint32_t step) noexcept {
  return size > start ? (size - start + step - 1U) / step : 0U;
}

[[nodiscard]] TemplateImageResult decode_pixels(const ParsedPng &png) {
  const auto &header = png.header;
  const std::size_t bits_per_pixel =
      channel_count(header.color) * header.depth;
  const std::size_t filter_stride = std::max<std::size_t>(1, bits_per_pixel / 8U);
  const auto row_bytes = [&](const std::uint32_t width) {
    return (static_cast<std::size_t>(width) * bits_per_pixel + 7U) / 8U;
  };
  const std::span<const Pass> passes =
      header.interlaced ? std::span<const Pass>(adam7_passes)
                        : std::span<const Pass>(whole_image);

  std::size_t filtered_size{};
  for (const auto &pass : passes) {
    const auto width = pass_extent(header.width, pass.x0, pass.dx);
    const auto height = pass_extent(header.height, pass.y0, pass.dy);
    if (width != 0 && height != 0)
      filtered_size += (row_bytes(width) + 1U) * height;
  }
  std::vector<std::uint8_t> filtered(filtered_size);
  const auto decoded = tinfl_decompress_mem_to_mem(
      filtered.data(), filtered.size(), png.compressed.data(),
      png.compressed.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (decoded == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
      decoded != filtered.size())
    return failure("PNG image data could not be decompressed");

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(header.width) *
                                 header.height * 4U);
  std::size_t cursor{};
  for (const auto &pass : passes) {
    const auto width = pass_extent(header.width, pass.x0, pass.dx);
    const auto height = pass_extent(header.height, pass.y0, pass.dy);
    if (width == 0 || height == 0) continue;
    const auto stride = row_bytes(width);
    std::vector<std::uint8_t> previous(stride);
    std::vector<std::uint8_t> current(stride);
    for (std::uint32_t y = 0; y < height; ++y) {
      const auto filter = filtered[cursor];
      if (filter > 4U) return failure("PNG uses an unsupported row filter");
      const auto *source = filtered.data() + cursor + 1U;
      cursor += stride + 1U;
      for (std::size_t x = 0; x < stride; ++x) {
        const std::uint8_t left = x < filter_stride ? 0U : current[x - filter_stride];
        const std::uint8_t above = previous[x];
        const std::uint8_t upper_left =
            x < filter_stride ? 0U : previous[x - filter_stride];
        std::uint8_t predictor{};
        switch (filter) {
        case 1: predictor = left; break;
        case 2: predictor = above; break;
        case 3:
          predictor = static_cast<std::uint8_t>(
              (static_cast<unsigned>(left) + static_cast<unsigned>(above)) / 2U);
          break;
        case 4: predictor = paeth(left, above, upper_left); break;
        default: break;
        }
        current[x] = static_cast<std::uint8_t>(source[x] + predictor);
      }
      const std::size_t image_y = pass.y0 + static_cast<std::size_t>(y) * pass.dy;
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::size_t image_x = pass.x0 + static_cast<std::size_t>(x) * pass.dx;
        auto *out = rgba.data() + (image_y * header.width + image_x) * 4U;
        if (!write_pixel(png, current.data(), x, out))
          return failure("PNG palette index is out of range");
      }
      std::swap(previous, current);
    }
  }

  auto image =
      layerc::core::template_image_from_rgba(header.width, header.height, rgba);
  if (!image.has_value())
    return failure("template requires at least 64 nontransparent pixels");
  return std::move(*image);
}

} // namespace

TemplateImageResult decode_template_png(const std::span<const std::uint8_t> bytes) {
  auto parsed = parse_chunks(bytes);
  if (auto *error = std::get_if<VisualAssetError>(&parsed)) return *error;
  return decode_pixels(std::get<ParsedPng>(parsed));
}

EncodedTemplatePng encode_template_png(const layerc::core::TemplateImage &image) {
  if (!image.valid()) return failure("template pixels or alpha mask are invalid");
  std::vector<std::uint8_t> rgba(image.pixels.size() * 4U);
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    rgba[index * 4U] = image.pixels[index].red;
    rgba[index * 4U + 1U] = image.pixels[index].green;
    rgba[index * 4U + 2U] = image.pixels[index].blue;
    rgba[index * 4U + 3U] = image.alpha[index];
  }
  std::size_t size{};
  const std::unique_ptr<void, decltype(&mz_free)> encoded(
      tdefl_write_image_to_png_file_in_memory_ex(
          rgba.data(), static_cast<int>(image.width),
          static_cast<int>(image.height), 4, &size, canonical_level, MZ_FALSE),
      &mz_free);
  if (encoded == nullptr || size == 0)
    return failure("PNG pixels could not be encoded");
  if (size > max_template_asset_bytes)
    return failure("encoded PNG exceeds the 4 MiB asset limit");
  const auto *data = static_cast<const std::uint8_t *>(encoded.get());
  return std::vector<std::uint8_t>(data, data + size);
}

bool is_canonical_template_png(const std::span<const std::uint8_t> bytes) noexcept {
  // Signature, then IHDR first: length(4) type(4) width(4) height(4) depth
  // color compression filter interlace.
  constexpr std::size_t header_end = png_signature.size() + 8U + 13U;
  if (bytes.size() < header_end ||
      !std::equal(png_signature.begin(), png_signature.end(), bytes.begin()))
    return false;
  const auto *chunk = bytes.data() + png_signature.size();
  const auto *data = chunk + 8U;
  return read_u32(chunk) == 13U && chunk_is(chunk + 4U, "IHDR") &&
         data[8] == 8U && data[9] == static_cast<std::uint8_t>(ColorType::rgba) &&
         data[10] == 0U && data[11] == 0U && data[12] == 0U;
}

EncodedTemplatePng canonical_template_png(const std::span<const std::uint8_t> bytes) {
  auto decoded = decode_template_png(bytes);
  if (auto *error = std::get_if<VisualAssetError>(&decoded)) return *error;
  if (is_canonical_template_png(bytes))
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
  return encode_template_png(std::get<layerc::core::TemplateImage>(decoded));
}

} // namespace layerc::platform
