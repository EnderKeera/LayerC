#include "layerc/platform/framed_pipe.hpp"

#include "layerc/core/frame.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace layerc::platform {
namespace {

using layerc::core::append_frame_header;
using layerc::core::frame_header_bytes;
using layerc::core::read_frame_header;

bool write_all(const HANDLE pipe, const void *data, std::size_t size,
               std::string &error) {
  const auto *bytes = static_cast<const std::byte *>(data);
  while (size != 0) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
        size, std::numeric_limits<DWORD>::max()));
    DWORD written{};
    if (WriteFile(pipe, bytes, chunk, &written, nullptr) == FALSE ||
        written == 0) {
      error = "framed pipe write failed";
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
      error = "framed pipe closed before its frame completed";
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
    error = "framed pipe handle is invalid";
    return false;
  }
  if (payload.size() > maximum_bytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "framed packet exceeds its bounded frame size";
    return false;
  }
  std::array<std::byte, frame_header_bytes> header{};
  append_frame_header(header.data(),
                      static_cast<std::uint32_t>(payload.size()));
  return write_all(pipe, header.data(), header.size(), error) &&
         write_all(pipe, payload.data(), payload.size(), error);
}

FramedPipeRead read_framed_pipe(const HANDLE pipe,
                                const std::size_t maximum_bytes) {
  FramedPipeRead result;
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    result.error = "framed pipe handle is invalid";
    return result;
  }
  std::array<std::byte, frame_header_bytes> header{};
  if (!read_all(pipe, header.data(), header.size(), result.error))
    return result;
  const auto size = read_frame_header(header.data());
  if (size > maximum_bytes) {
    result.error = "framed pipe length exceeds its bounded limit";
    return result;
  }
  result.payload.resize(size);
  if (!read_all(pipe, result.payload.data(), result.payload.size(),
                result.error))
    result.payload.clear();
  return result;
}

} // namespace layerc::platform
