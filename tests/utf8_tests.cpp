#include "core_test_cases.hpp"
#include "layerc/core/utf8.hpp"
#include "test_support.hpp"

#include <string>

namespace layerc::test {

void test_utf8_codec_and_casefold() {
  const std::string invalid{"\xF0\x28\x8C\x28", 4};
  expect(!core::is_valid_utf8(invalid) &&
             !core::try_utf8_to_wide(invalid).has_value() &&
             !core::try_casefold_utf8(invalid).has_value(),
         "UTF-8 helpers reject invalid input");

  const std::wstring wide = L"LayerC \u00C9 \U0001F642";
  const auto encoded = core::try_wide_to_utf8(wide);
  const auto decoded =
      encoded.has_value() ? core::try_utf8_to_wide(*encoded) : std::nullopt;
  expect(decoded.has_value() && *decoded == wide,
         "UTF-8 codec round-trips UTF-16 or UTF-32 wchar strings");

  const auto ascii = core::try_casefold_utf8("PROFILE.JSON");
  expect(ascii.has_value() && *ascii == "profile.json",
         "UTF-8 casefold lowercases ASCII deterministically");

  const auto accented = core::try_casefold_utf8("\xC3\x89.json");
  expect(accented.has_value() && *accented == "\xC3\xA9.json",
         "UTF-8 casefold normalizes invariant accented collisions");

  // Full Latin-1 rows: U+00C0-U+00D6 and U+00D8-U+00DE fold with +0x20;
  // U+00D7 (multiplication sign) and non-Latin scripts are byte-exact.
  const auto latin_top = core::try_casefold_utf8("\xC3\x80\xC3\x96\xC3\x98\xC3\x9E");
  expect(latin_top.has_value() &&
             *latin_top == "\xC3\xA0\xC3\xB6\xC3\xB8\xC3\xBE",
         "UTF-8 casefold covers the whole Latin-1 cased range");
  const auto no_case = core::try_casefold_utf8("\xC3\x97");
  expect(no_case.has_value() && *no_case == "\xC3\x97",
         "UTF-8 casefold leaves U+00D7 without case unchanged");
}

} // namespace layerc::test
