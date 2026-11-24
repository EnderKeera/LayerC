#pragma once

#include <optional>
#include <sys/select.h>

namespace layerc::editor {
namespace detail {
inline std::optional<int> fltk_fd_test_limit;
} // namespace detail

// FLTK may use select even when it comes from the distribution.
[[nodiscard]] inline bool fltk_fd_watchable(const int fd) noexcept {
  return fd >= 0 && fd < FD_SETSIZE &&
      fd < detail::fltk_fd_test_limit.value_or(FD_SETSIZE);
}

inline void fltk_fd_limit_for_test(const std::optional<int> limit) noexcept {
  detail::fltk_fd_test_limit = limit;
}

} // namespace layerc::editor
