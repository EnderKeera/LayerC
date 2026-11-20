#include "platform/services.hpp"

#include "layerc/core/utf8.hpp"

#include <windows.h>

#include <string>

namespace layerc::editor {

std::optional<std::string> desktop_font_description() {
  const UINT dpi = GetDpiForSystem();
  if (dpi == 0) return std::nullopt;

  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = static_cast<UINT>(sizeof(metrics));
  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                                  &metrics, 0, dpi)) {
    return std::nullopt;
  }

  const LOGFONTW &font = metrics.lfMessageFont;
  const long long pixel_height = font.lfHeight < 0
      ? -static_cast<long long>(font.lfHeight)
      : static_cast<long long>(font.lfHeight);
  if (pixel_height == 0 || pixel_height > 32'767 || dpi > 32'767)
    return std::nullopt;
  const int point_size = MulDiv(static_cast<int>(pixel_height), 72,
                                static_cast<int>(dpi));
  if (point_size <= 0) return std::nullopt;

  const auto family = core::try_wide_to_utf8(font.lfFaceName);
  if (!family || family->empty()) return std::nullopt;
  return *family + " " + std::to_string(point_size);
}

} // namespace layerc::editor
