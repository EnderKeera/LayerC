#pragma once

// The 4-byte little-endian length prefix shared by every length-framed
// channel: the live-edit pipe, the calibration pipe, and the isolated
// visual-worker pipe all frame payloads with this header so the framing
// cannot drift between transports.

#include <cstddef>
#include <cstdint>

namespace layerc::core {

inline constexpr std::size_t frame_header_bytes = 4;

inline void append_frame_header(std::byte *const destination,
                                const std::uint32_t length) noexcept {
  destination[0] = static_cast<std::byte>(length & 0xFFU);
  destination[1] = static_cast<std::byte>((length >> 8U) & 0xFFU);
  destination[2] = static_cast<std::byte>((length >> 16U) & 0xFFU);
  destination[3] = static_cast<std::byte>((length >> 24U) & 0xFFU);
}

[[nodiscard]] inline std::uint32_t
read_frame_header(const std::byte *const source) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<unsigned>(source[0])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(source[1]))
          << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(source[2]))
          << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(source[3]))
          << 24U);
}

} // namespace layerc::core
