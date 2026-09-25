#pragma once

#include <string>

#include <windows.h>

namespace layerc::platform {

[[nodiscard]] std::wstring windows_error_text(DWORD error);

} // namespace layerc::platform
