#pragma once

#include "layerc/core/overlay_item.hpp"

namespace layerc::core::detail {

[[nodiscard]] json::Value serialize_overlay_item(const OverlayItem &item);

} // namespace layerc::core::detail
