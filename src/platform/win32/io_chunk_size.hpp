#pragma once

// Private Win32 IO request bound shared by durable files and framed pipes.
#include <algorithm>
#include <cstddef>
#include <limits>
#include <windows.h>

namespace layerc::platform {

inline DWORD io_chunk_size(const std::size_t remaining) {
  return static_cast<DWORD>((std::min)(
      remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
}

} // namespace layerc::platform
