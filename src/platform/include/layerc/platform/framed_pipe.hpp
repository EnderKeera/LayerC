#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::platform {

struct FramedPipeRead {
  std::string payload;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error.empty();
  }
};

[[nodiscard]] bool write_framed_pipe(HANDLE pipe, std::string_view payload,
                                     std::size_t maximum_bytes,
                                     std::string &error);
[[nodiscard]] FramedPipeRead read_framed_pipe(HANDLE pipe,
                                               std::size_t maximum_bytes);

} // namespace layerc::platform
