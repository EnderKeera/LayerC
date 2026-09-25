#pragma once

#include <windows.h>

namespace layerc::platform {

// Move-only owner for any CloseHandle-released kernel handle. Null and
// INVALID_HANDLE_VALUE are both treated as empty.
class OwnedHandle {
public:
  OwnedHandle() = default;
  explicit OwnedHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~OwnedHandle() { reset(); }
  OwnedHandle(const OwnedHandle &) = delete;
  OwnedHandle &operator=(const OwnedHandle &) = delete;
  OwnedHandle(OwnedHandle &&other) noexcept : handle_(other.release()) {}
  OwnedHandle &operator=(OwnedHandle &&other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE release() noexcept {
    const HANDLE old = handle_;
    handle_ = nullptr;
    return old;
  }
  void reset(HANDLE handle = nullptr) noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = handle;
  }

private:
  HANDLE handle_{};
};

// Named mutex held for the process lifetime; `acquired()` is false when
// another instance already owns the name.
class SingleInstanceGuard {
public:
  enum class State { acquired, already_running, failed };

  explicit SingleInstanceGuard(const wchar_t *name) noexcept
      : mutex_(CreateMutexW(nullptr, TRUE, name)),
        state_(!mutex_.valid()                          ? State::failed
               : GetLastError() == ERROR_ALREADY_EXISTS ? State::already_running
                                                        : State::acquired) {}
  ~SingleInstanceGuard() {
    if (state_ == State::acquired) ReleaseMutex(mutex_.get());
  }
  SingleInstanceGuard(const SingleInstanceGuard &) = delete;
  SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;

  [[nodiscard]] State state() const noexcept { return state_; }

private:
  OwnedHandle mutex_;
  State state_;
};

} // namespace layerc::platform
