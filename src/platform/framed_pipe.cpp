#include "layerc/platform/framed_pipe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace layerc::platform {
namespace {

bool write_all(const HANDLE pipe, const void *data, std::size_t size,
               std::string &error) {
  const auto *bytes = static_cast<const std::byte *>(data);
  while (size != 0) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
        size, std::numeric_limits<DWORD>::max()));
    DWORD written{};
    if (WriteFile(pipe, bytes, chunk, &written, nullptr) == FALSE ||
        written == 0) {
      error = "calibration pipe write failed";
      return false;
    }
    bytes += written;
    size -= written;
  }
  return true;
}

bool read_all(const HANDLE pipe, void *data, std::size_t size,
              std::string &error) {
  auto *bytes = static_cast<std::byte *>(data);
  while (size != 0) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
        size, std::numeric_limits<DWORD>::max()));
    DWORD read{};
    if (ReadFile(pipe, bytes, chunk, &read, nullptr) == FALSE || read == 0) {
      error = "calibration pipe closed before its frame completed";
      return false;
    }
    bytes += read;
    size -= read;
  }
  return true;
}

} // namespace

bool write_framed_pipe(const HANDLE pipe, const std::string_view payload,
                       const std::size_t maximum_bytes, std::string &error) {
  error.clear();
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    error = "calibration pipe handle is invalid";
    return false;
  }
  if (payload.size() > maximum_bytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "calibration packet exceeds its bounded frame size";
    return false;
  }
  const auto size = static_cast<std::uint32_t>(payload.size());
  const std::array<std::byte, 4> header{
      static_cast<std::byte>(size & 0xFFU),
      static_cast<std::byte>((size >> 8U) & 0xFFU),
      static_cast<std::byte>((size >> 16U) & 0xFFU),
      static_cast<std::byte>((size >> 24U) & 0xFFU)};
  return write_all(pipe, header.data(), header.size(), error) &&
         write_all(pipe, payload.data(), payload.size(), error);
}

FramedPipeRead read_framed_pipe(const HANDLE pipe,
                                const std::size_t maximum_bytes) {
  FramedPipeRead result;
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    result.error = "calibration pipe handle is invalid";
    return result;
  }
  std::array<std::byte, 4> header{};
  if (!read_all(pipe, header.data(), header.size(), result.error))
    return result;
  const auto byte = [&](const std::size_t index) {
    return static_cast<std::uint32_t>(header[index]);
  };
  const auto size = byte(0) | (byte(1) << 8U) | (byte(2) << 16U) |
                    (byte(3) << 24U);
  if (size > maximum_bytes) {
    result.error = "calibration frame length exceeds its bounded limit";
    return result;
  }
  result.payload.resize(size);
  if (!read_all(pipe, result.payload.data(), result.payload.size(),
                result.error))
    result.payload.clear();
  return result;
}

} // namespace layerc::platform
