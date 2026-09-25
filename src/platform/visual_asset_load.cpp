#include "layerc/platform/visual_assets.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/platform/durable_file.hpp"

#include <cstddef>
#include <span>

namespace layerc::platform {

TemplateImageResult verified_template_from_bytes(
    const std::filesystem::path &path, const std::string_view sha256,
    const DecodeTemplateRgba decode_rgba) {
  if (!layerc::core::is_lowercase_sha256(sha256))
    return VisualAssetError{"template asset id is not a lower-case SHA-256 digest"};
  const auto read = read_durable_file(path, max_template_asset_bytes);
  if (!read.has_value() || read->empty())
    return VisualAssetError{"template asset is missing or exceeds the 4 MiB limit"};
  if (layerc::core::sha256(std::span<const std::byte>(*read)) != sha256)
    return VisualAssetError{"template asset hash does not match its profile reference"};
  auto bytes = std::vector<std::uint8_t>(read->size());
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = std::to_integer<std::uint8_t>((*read)[index]);
  auto decoded = decode_rgba(bytes);
  if (auto *image = std::get_if<layerc::core::TemplateImage>(&decoded)) {
    if (!layerc::core::template_image_has_structure(*image))
      return VisualAssetError{"template asset lacks enough visual structure for positional matching"};
    image->asset_sha256 = std::string(sha256);
  }
  return decoded;
}

} // namespace layerc::platform
