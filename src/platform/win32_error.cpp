#include "layerc/platform/win32_error.hpp"

#include "layerc/platform/text.hpp"

namespace layerc::platform {

std::wstring windows_error_text(const DWORD error) {
  if (error == ERROR_SUCCESS)
    return {};
  wchar_t *message{};
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  std::wstring result = length != 0 && message != nullptr
                            ? std::wstring(message, length)
                            : L"Windows error " + std::to_wstring(error);
  if (message != nullptr)
    LocalFree(message);
  while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                             result.back() == L' '))
    result.pop_back();
  return result;
}

std::string hresult_message(const HRESULT result) {
  return wide_to_utf8(windows_error_text(static_cast<DWORD>(result)));
}

} // namespace layerc::platform
