#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace layerc::editor {

// The pairs Appearance measures inside one role and refuses below their
// floors (DesignSystem.md, Roles): Text / Fill 4.5:1, Disabled / Fill 3:1,
// Text / Hover 4.5:1, Text / Selection 4.5:1. One wording for both editors.
enum class ContrastPairKind : std::uint8_t { text_fill, disabled_fill, text_hover, text_selection };

// 3.0 for Disabled / Fill, else 4.5.
[[nodiscard]] double contrast_floor(ContrastPairKind pair) noexcept;
// "Text / Fill", "Disabled / Fill", "Text / Hover", "Text / Selection".
[[nodiscard]] std::string_view contrast_pair_label(ContrastPairKind pair) noexcept;
// "12.9:1", rounded down to one decimal so a ratio just below a floor never
// reads as it; ratios below 1 print as 1.0:1.
[[nodiscard]] std::string contrast_ratio_label(double ratio);
// "Buttons: Text / Hover contrast is 3.2:1; at least 4.5:1 is required."
// UTF-8; the floor prints without decimals when it is whole. A `surface`
// names where the text is painted when that is not the role's own colour:
// "Labels: Text / Fill contrast is 1.3:1 on the Window fill; at least 4.5:1
// is required."
[[nodiscard]] std::string contrast_refusal_text(std::string_view role_label,
                                                ContrastPairKind pair, double ratio,
                                                std::string_view surface = {});

} // namespace layerc::editor
