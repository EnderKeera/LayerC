#pragma once

#include <string_view>

namespace layerc::test {

extern int failures;
bool expect(bool condition, std::string_view message);

} // namespace layerc::test
