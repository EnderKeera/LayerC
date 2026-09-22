#pragma once

// The 4-byte little-endian length prefix shared by every length-framed
// channel: the live-edit pipe, the calibration pipe, and the isolated
// visual-worker pipe all frame payloads with this header so the framing
// cannot drift between transports.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

// Transport-independent framing. Bounds are checked before allocation; a fault
// remains sticky until the transport explicitly starts a new stream with clear().
class FrameDecoder {
public:
  enum class Fault : std::uint8_t { none, frame_too_large, buffer_too_large };

  FrameDecoder(std::size_t frame_limit, std::size_t buffer_limit)
      : frame_limit_(frame_limit), buffer_limit_(buffer_limit) {}

  void feed(std::span<const std::byte> bytes) {
    if (fault_ != Fault::none) return;
    if (bytes.size() > buffer_limit_ ||
        buffered_bytes() > buffer_limit_ - bytes.size()) {
      fail(Fault::buffer_too_large);
      return;
    }
    if (consumed_ != 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
      consumed_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  }

  [[nodiscard]] std::optional<std::string> next_frame() {
    if (fault_ != Fault::none || buffered_bytes() < frame_header_bytes)
      return std::nullopt;
    const auto length = read_frame_header(buffer_.data() + consumed_);
    if (length > frame_limit_) {
      fail(Fault::frame_too_large);
      return std::nullopt;
    }
    if (buffered_bytes() - frame_header_bytes < length) return std::nullopt;
    const auto *payload = reinterpret_cast<const char *>(
        buffer_.data() + consumed_ + frame_header_bytes);
    std::string result(payload, length);
    consumed_ += frame_header_bytes + length;
    if (consumed_ == buffer_.size()) {
      buffer_.clear();
      consumed_ = 0;
    }
    return result;
  }

  void clear() noexcept {
    std::vector<std::byte>().swap(buffer_);
    consumed_ = 0;
    fault_ = Fault::none;
  }
  [[nodiscard]] Fault fault() const noexcept { return fault_; }
  [[nodiscard]] std::size_t buffered_bytes() const noexcept {
    return buffer_.size() - consumed_;
  }
  [[nodiscard]] std::size_t reserved_bytes() const noexcept {
    return buffer_.capacity();
  }

private:
  void fail(Fault fault) noexcept {
    clear();
    fault_ = fault;
  }
  std::size_t frame_limit_;
  std::size_t buffer_limit_;
  std::vector<std::byte> buffer_;
  std::size_t consumed_{};
  Fault fault_{Fault::none};
};

} // namespace layerc::core
