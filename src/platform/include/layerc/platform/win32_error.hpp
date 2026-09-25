#pragma once

#include <string>

#include <windows.h>

namespace layerc::platform {

[[nodiscard]] std::wstring windows_error_text(DWORD error);
// UTF-8 system message for an HRESULT (or Win32 error) code.
[[nodiscard]] std::string hresult_message(HRESULT result);

} // namespace layerc::platform
