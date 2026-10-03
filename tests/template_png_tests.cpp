// Shared template PNG codec and pack-import canonicalization. Built on every
// platform: both stores decode and encode assets through this one codec.

#include "../src/bundle/pack_detail.hpp"
#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/visual.hpp"
#include "template_png_fixture.hpp"
#include "test_support.hpp"

#include "miniz.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using layerc::test::expect;
using Bytes = std::vector<std::uint8_t>;
using Rgba = std::array<std::uint8_t, 4>;

struct PngSpec {
  const char *name;
  std::uint8_t depth;
  std::uint8_t color;
  bool interlaced{};
  Bytes palette{};
  Bytes transparency{};
};

// Raw samples for pixel (x, y), and the RGBA the decoder must produce.
using SampleFn = std::function<std::vector<std::uint16_t>(std::uint32_t, std::uint32_t)>;
using ExpectedFn = std::function<Rgba(std::uint32_t, std::uint32_t)>;

constexpr std::uint32_t side = 16;

void append_u32(Bytes &out, const std::uint32_t value) {
  for (const unsigned shift : {24U, 16U, 8U, 0U})
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

void append_chunk(Bytes &out, const char (&type)[5], const Bytes &data) {
  append_u32(out, static_cast<std::uint32_t>(data.size()));
  Bytes typed;
  for (std::size_t index = 0; index < 4; ++index)
    typed.push_back(static_cast<std::uint8_t>(type[index]));
  typed.insert(typed.end(), data.begin(), data.end());
  out.insert(out.end(), typed.begin(), typed.end());
  append_u32(out, static_cast<std::uint32_t>(
                      mz_crc32(0, typed.data(), typed.size())));
}

std::size_t channels(const std::uint8_t color) {
  switch (color) {
  case 2: return 3;
  case 4: return 2;
  case 6: return 4;
  default: return 1;
  }
}

// Minimal unfiltered PNG writer covering every color type, bit depth, and
// Adam7, so decoding is tested against independently produced files.
Bytes write_png(const PngSpec &spec, const SampleFn &samples) {
  struct Pass { std::uint32_t x0, y0, dx, dy; };
  const std::vector<Pass> passes =
      spec.interlaced
          ? std::vector<Pass>{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                              {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                              {0, 1, 1, 2}}
          : std::vector<Pass>{{0, 0, 1, 1}};
  const std::size_t count = channels(spec.color);
  Bytes raw;
  for (const auto &pass : passes) {
    const std::uint32_t width = side > pass.x0 ? (side - pass.x0 + pass.dx - 1) / pass.dx : 0;
    const std::uint32_t height = side > pass.y0 ? (side - pass.y0 + pass.dy - 1) / pass.dy : 0;
    if (width == 0 || height == 0) continue;
    const std::size_t row_bytes = (width * count * spec.depth + 7U) / 8U;
    for (std::uint32_t y = 0; y < height; ++y) {
      raw.push_back(0);
      Bytes row(row_bytes);
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto values = samples(pass.x0 + x * pass.dx, pass.y0 + y * pass.dy);
        for (std::size_t channel = 0; channel < count; ++channel) {
          const std::size_t index = x * count + channel;
          const std::uint16_t value = values[channel];
          if (spec.depth == 16) {
            row[index * 2] = static_cast<std::uint8_t>(value >> 8U);
            row[index * 2 + 1] = static_cast<std::uint8_t>(value & 0xffU);
          } else if (spec.depth == 8) {
            row[index] = static_cast<std::uint8_t>(value);
          } else {
            const std::size_t bit = index * spec.depth;
            row[bit / 8] = static_cast<std::uint8_t>(
                row[bit / 8] | (value << (8U - spec.depth - bit % 8U)));
          }
        }
      }
      raw.insert(raw.end(), row.begin(), row.end());
    }
  }
  std::size_t compressed_size{};
  const std::unique_ptr<void, decltype(&mz_free)> compressed(
      tdefl_compress_mem_to_heap(raw.data(), raw.size(), &compressed_size,
                                 TDEFL_WRITE_ZLIB_HEADER | 128),
      &mz_free);
  const auto *data = static_cast<const std::uint8_t *>(compressed.get());
  Bytes png{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  Bytes header;
  append_u32(header, side);
  append_u32(header, side);
  header.insert(header.end(), {spec.depth, spec.color, 0, 0,
                               static_cast<std::uint8_t>(spec.interlaced)});
  append_chunk(png, "IHDR", header);
  if (!spec.palette.empty()) append_chunk(png, "PLTE", spec.palette);
  if (!spec.transparency.empty()) append_chunk(png, "tRNS", spec.transparency);
  append_chunk(png, "IDAT", Bytes(data, data + compressed_size));
  append_chunk(png, "IEND", {});
  return png;
}

bool light(const std::uint32_t x, const std::uint32_t y) {
  return ((x / 4U) + (y / 4U)) % 2U == 0U;
}

std::uint8_t level(const std::uint32_t x, const std::uint32_t y) {
  return static_cast<std::uint8_t>(light(x, y) ? 200U + x : 40U + y);
}

struct FormatCase {
  PngSpec spec;
  SampleFn samples;
  ExpectedFn expected;
};

std::vector<FormatCase> format_cases() {
  const auto rgb = [](std::uint32_t x, std::uint32_t y) {
    return std::vector<std::uint16_t>{level(x, y), static_cast<std::uint16_t>(x * 9U),
                                      static_cast<std::uint16_t>(y * 11U)};
  };
  const auto rgb_expected = [](std::uint32_t x, std::uint32_t y) {
    return Rgba{level(x, y), static_cast<std::uint8_t>(x * 9U),
                static_cast<std::uint8_t>(y * 11U), 0xff};
  };
  return {
      {{"rgb8", 8, 2}, rgb, rgb_expected},
      {{"rgb8 interlaced", 8, 2, true}, rgb, rgb_expected},
      {{"rgba8 interlaced", 8, 6, true},
       [](std::uint32_t x, std::uint32_t y) {
         return std::vector<std::uint16_t>{level(x, y), 1, 2,
                                           static_cast<std::uint16_t>(x == 15 ? 0 : 255)};
       },
       [](std::uint32_t x, std::uint32_t y) {
         return Rgba{level(x, y), 1, 2, static_cast<std::uint8_t>(x == 15 ? 0 : 255)};
       }},
      {{"rgba16", 16, 6},
       [](std::uint32_t x, std::uint32_t y) {
         const auto value = static_cast<std::uint16_t>(level(x, y) * 257U);
         return std::vector<std::uint16_t>{value, value, 0x1234, 0xffff};
       },
       [](std::uint32_t x, std::uint32_t y) {
         return Rgba{level(x, y), level(x, y), 0x12, 0xff};
       }},
      {{"gray2", 2, 0},
       [](std::uint32_t x, std::uint32_t y) {
         return std::vector<std::uint16_t>{
             static_cast<std::uint16_t>(light(x, y) ? 3U : x % 3U)};
       },
       [](std::uint32_t x, std::uint32_t y) {
         const auto value = static_cast<std::uint8_t>((light(x, y) ? 3U : x % 3U) * 85U);
         return Rgba{value, value, value, 0xff};
       }},
      {{"gray8 tRNS", 8, 0, false, {}, {0, 0}},
       [](std::uint32_t x, std::uint32_t y) {
         return std::vector<std::uint16_t>{
             static_cast<std::uint16_t>(x == 0 ? 0 : level(x, y))};
       },
       [](std::uint32_t x, std::uint32_t y) {
         const std::uint8_t value = x == 0 ? std::uint8_t{0} : level(x, y);
         return Rgba{value, value, value, static_cast<std::uint8_t>(x == 0 ? 0 : 255)};
       }},
      {{"palette4 tRNS", 4, 3, false,
        {10, 20, 30, 200, 210, 220, 90, 0, 45, 1, 2, 3}, {255, 255, 255, 0}},
       [](std::uint32_t x, std::uint32_t y) {
         return std::vector<std::uint16_t>{
             static_cast<std::uint16_t>(x == 0 ? 3 : light(x, y) ? 1 : y % 2 == 0 ? 0 : 2)};
       },
       [](std::uint32_t x, std::uint32_t y) {
         if (x == 0) return Rgba{1, 2, 3, 0};
         if (light(x, y)) return Rgba{200, 210, 220, 0xff};
         return y % 2 == 0 ? Rgba{10, 20, 30, 0xff} : Rgba{90, 0, 45, 0xff};
       }},
      {{"gray-alpha8", 8, 4},
       [](std::uint32_t x, std::uint32_t y) {
         return std::vector<std::uint16_t>{level(x, y),
                                           static_cast<std::uint16_t>(x == 15 ? 128 : 255)};
       },
       [](std::uint32_t x, std::uint32_t y) {
         return Rgba{level(x, y), level(x, y), level(x, y),
                     static_cast<std::uint8_t>(x == 15 ? 128 : 255)};
       }},
  };
}

bool image_matches(const layerc::core::TemplateImage &image, const ExpectedFn &expected) {
  if (image.width != side || image.height != side) return false;
  for (std::uint32_t y = 0; y < side; ++y) {
    for (std::uint32_t x = 0; x < side; ++x) {
      const auto index = y * side + x;
      const auto want = expected(x, y);
      const auto &pixel = image.pixels[index];
      if (pixel.red != want[0] || pixel.green != want[1] ||
          pixel.blue != want[2] || image.alpha[index] != want[3])
        return false;
    }
  }
  return true;
}

void test_canonical_round_trip() {
  const auto image = layerc::test::checker_template_image();
  const auto first = layerc::platform::encode_template_png(image);
  const auto second = layerc::platform::encode_template_png(image);
  const auto *bytes = std::get_if<Bytes>(&first);
  if (!expect(bytes != nullptr, "a valid template encodes")) return;
  expect(std::get_if<Bytes>(&second) != nullptr &&
             std::get<Bytes>(second) == *bytes,
         "canonical encoding is deterministic");
  expect(layerc::platform::is_canonical_template_png(*bytes),
         "encoded templates use the canonical stored format");
  const auto decoded = layerc::platform::decode_template_png(*bytes);
  const auto *round_trip = std::get_if<layerc::core::TemplateImage>(&decoded);
  expect(round_trip != nullptr && *round_trip == image,
         "canonical PNG round-trips exactly");
  const auto canonical = layerc::platform::canonical_template_png(*bytes);
  expect(std::get_if<Bytes>(&canonical) != nullptr &&
             std::get<Bytes>(canonical) == *bytes,
         "canonical input bytes are stored unchanged");
}

void test_decodes_every_png_format() {
  for (const auto &format : format_cases()) {
    const std::string name = format.spec.name;
    const auto png = write_png(format.spec, format.samples);
    expect(!layerc::platform::is_canonical_template_png(png),
           name + " is not the canonical stored format");
    const auto decoded = layerc::platform::decode_template_png(png);
    const auto *image = std::get_if<layerc::core::TemplateImage>(&decoded);
    if (!expect(image != nullptr, name + " decodes")) continue;
    expect(image_matches(*image, format.expected), name + " decodes exact RGBA");
    const auto canonical = layerc::platform::canonical_template_png(png);
    const auto *stored = std::get_if<Bytes>(&canonical);
    if (!expect(stored != nullptr && layerc::platform::is_canonical_template_png(*stored),
                name + " re-encodes to the canonical format"))
      continue;
    const auto again = layerc::platform::decode_template_png(*stored);
    expect(std::get_if<layerc::core::TemplateImage>(&again) != nullptr &&
               std::get<layerc::core::TemplateImage>(again) == *image,
           name + " canonical re-encoding preserves pixels");
  }
}

void test_rejects_malformed_png() {
  const auto &fixture = layerc::test::canonical_template_png_fixture();
  const Bytes good(fixture.bytes.begin(), fixture.bytes.end());
  const auto rejected = [](const Bytes &bytes) {
    return std::holds_alternative<layerc::platform::VisualAssetError>(
        layerc::platform::decode_template_png(bytes));
  };
  expect(rejected(Bytes(good.begin(), good.end() - 1)), "truncated PNG is rejected");
  auto corrupt = good;
  corrupt[40] ^= 0x01U;
  expect(rejected(corrupt), "a chunk checksum mismatch is rejected");
  auto trailing = good;
  trailing.push_back(0);
  expect(rejected(trailing), "trailing data is rejected");
  expect(rejected({'a', 'b', 'c'}), "non-PNG bytes are rejected");

  PngSpec small_palette{"palette", 4, 3, false, {1, 2, 3, 4, 5, 6}};
  expect(rejected(write_png(small_palette,
                            [](std::uint32_t x, std::uint32_t) {
                              return std::vector<std::uint16_t>{
                                  static_cast<std::uint16_t>(x % 3U)};
                            })),
         "a palette index outside the palette is rejected");
  PngSpec palette_without_plte{"palette", 8, 3};
  expect(rejected(write_png(palette_without_plte,
                            [](std::uint32_t, std::uint32_t) {
                              return std::vector<std::uint16_t>{0};
                            })),
         "a palette image without PLTE is rejected");
  PngSpec bad_depth{"rgb4", 4, 2};
  expect(rejected(write_png(bad_depth,
                            [](std::uint32_t, std::uint32_t) {
                              return std::vector<std::uint16_t>{1, 1, 1};
                            })),
         "an invalid color type/depth pair is rejected");
}

void test_pack_assets_are_canonicalized() {
  using namespace layerc;
  const auto &canonical = test::canonical_template_png_fixture();
  const PngSpec rgb_spec{"rgb8", 8, 2};
  const auto image = test::checker_template_image();
  const auto foreign = write_png(rgb_spec, [&](std::uint32_t x, std::uint32_t y) {
    const auto &pixel = image.pixels[y * side + x];
    return std::vector<std::uint16_t>{pixel.red, pixel.green, pixel.blue};
  });
  const auto to_bytes = [](const auto &source) {
    std::vector<std::byte> bytes;
    for (const auto value : source)
      bytes.push_back(static_cast<std::byte>(value));
    return bytes;
  };
  const auto foreign_bytes = to_bytes(foreign);
  const auto foreign_hash = core::sha256(foreign_bytes);

  bundle::ValidatedPack pack;
  pack.assets.emplace(foreign_hash,
                      bundle::PackAsset{.sha256 = foreign_hash, .bytes = foreign_bytes});
  pack.assets.emplace(canonical.sha256,
                      bundle::PackAsset{.sha256 = canonical.sha256,
                                        .bytes = to_bytes(canonical.bytes)});
  core::VisualProfile visual;
  visual.profile_id = "pack";
  visual.observations.push_back(
      layerc::core::TemplateState{.id = "ready",
       .name = "Ready",
       .search_region = {.x = 0.1, .y = 0.1, .width = 0.4, .height = 0.4},
       .expected_width = 0.2,
       .expected_height = 0.2,
       .references = {{.asset_sha256 = foreign_hash},
                      {.asset_sha256 = canonical.sha256}}});
  bundle::PackProfile item;
  item.profile.id = "pack";
  item.visual = bundle::ModulePayload<core::VisualProfile>{
      .json = core::serialize_visual_profile_json(visual), .profile = visual};
  item.asset_sha256 = {foreign_hash, canonical.sha256};
  pack.profiles.push_back(item);

  auto result = bundle::detail::canonicalize_pack_assets(pack);
  const auto *normalized = std::get_if<bundle::ValidatedPack>(&result);
  if (!expect(normalized != nullptr, "a decodable foreign asset is accepted")) return;
  expect(normalized->assets.size() == 1 &&
             normalized->assets.contains(canonical.sha256),
         "the RGB asset re-encodes to the same canonical bytes as its RGBA twin");
  const auto &state = std::get<layerc::core::TemplateState>(
      normalized->profiles.front().visual->profile.observations.front());
  expect(state.references.size() == 1 &&
             state.references.front().asset_sha256 == canonical.sha256,
         "references are rewritten to the canonical hash and deduplicated");
  const auto &json = normalized->profiles.front().visual->json;
  expect(json.find(canonical.sha256) != std::string::npos &&
             json.find(foreign_hash) == std::string::npos,
         "the visual sidecar payload carries only the canonical hash");
  expect(normalized->profiles.front().asset_sha256 ==
             std::vector<std::string>{canonical.sha256},
         "the manifest asset list follows the rewritten references");

  bundle::ValidatedPack untouched;
  untouched.assets.emplace(canonical.sha256,
                           bundle::PackAsset{.sha256 = canonical.sha256,
                                             .bytes = to_bytes(canonical.bytes)});
  auto kept = bundle::detail::canonicalize_pack_assets(untouched);
  expect(std::get_if<bundle::ValidatedPack>(&kept) != nullptr &&
             std::get<bundle::ValidatedPack>(kept).assets.begin()->second.bytes ==
                 to_bytes(canonical.bytes),
         "canonical assets keep their bytes and hash");

  bundle::ValidatedPack broken;
  const auto junk = to_bytes(std::string("abc"));
  broken.assets.emplace(core::sha256(junk),
                        bundle::PackAsset{.sha256 = core::sha256(junk), .bytes = junk});
  expect(std::holds_alternative<bundle::PackError>(
             bundle::detail::canonicalize_pack_assets(broken)),
         "an undecodable asset fails pack validation");
}

} // namespace

int main() {
  return layerc::test::run({
      {"test_canonical_round_trip", test_canonical_round_trip},
      {"test_decodes_every_png_format", test_decodes_every_png_format},
      {"test_rejects_malformed_png", test_rejects_malformed_png},
      {"test_pack_assets_are_canonicalized", test_pack_assets_are_canonicalized},
  });
}
