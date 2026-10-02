#pragma once

// The one PNG codec for template assets on every platform (miniz). Stored
// assets use the canonical format: non-interlaced 8-bit RGBA. Decoding
// accepts every standard PNG so imports can canonicalize foreign files.

#include "layerc/platform/visual_assets.hpp"

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace layerc::platform {

using EncodedTemplatePng = std::variant<std::vector<std::uint8_t>, VisualAssetError>;

// Any standard PNG (gray, RGB, palette, gray+alpha, RGBA; 1-16 bit depth;
// PLTE/tRNS; Adam7) up to core::max_template_axis per side.
[[nodiscard]] TemplateImageResult
decode_template_png(std::span<const std::uint8_t> bytes);

// Deterministic canonical encoding: equal images yield equal bytes, and so
// equal content hashes, on every platform.
[[nodiscard]] EncodedTemplatePng
encode_template_png(const layerc::core::TemplateImage &image);

// True when the PNG header declares the canonical stored format.
[[nodiscard]] bool
is_canonical_template_png(std::span<const std::uint8_t> bytes) noexcept;

// The bytes to store for an imported PNG: the input itself when it is
// canonical and decodable, otherwise its canonical re-encoding.
[[nodiscard]] EncodedTemplatePng
canonical_template_png(std::span<const std::uint8_t> bytes);

} // namespace layerc::platform
