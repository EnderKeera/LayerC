#include "accessible_text.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/text.hpp"
#include "test_support.hpp"

#include <string>

// What a screen reader says (DesignSystem.md, Screen readers): the name a
// color button and a palette swatch are read out with, the hex form a profile
// writes, and when a status message is spoken.
namespace {

using layerc::editor::color_button_name;
using layerc::editor::color_hex;
using layerc::editor::StatusVoice;
using layerc::editor::status_is_announced;
using layerc::editor::swatch_name;
using layerc::test::expect;

} // namespace

void editor_accessibility_contracts() {
  expect(color_button_name(L"&Fill", L"#DDBC17") == L"Fill, color #DDBC17",
         "a color button drops its access-key marker and adds its color");
  expect(color_button_name(L"Text && &more", L"#010203") ==
             L"Text & more, color #010203",
         "a doubled ampersand stays one literal ampersand");
  expect(swatch_name(L"Copper", L"#D88E4B", 7, 24) ==
             L"Copper #D88E4B, 7 of 24",
         "a swatch carries its name and its 1-based place");
  expect(color_hex(0xDDBC17) == L"#DDBC17",
         "a color reads as the uppercase #RRGGBB a profile writes");
  expect(color_hex(0xDDBC17) ==
             layerc::platform::utf8_to_wide(
                 layerc::core::json::color_text(0xDDBC17)),
         "the editor and the profile writer agree on one hex form");
  expect(layerc::core::json::parse_color_text("#DDBC17") == 0xDDBC17U,
         "the name's hex form round-trips through the profile parser");

  // A successful autosave follows every edit, so it stays silent; an explicit
  // outcome is spoken every time, and a repeating failure only once.
  expect(!status_is_announced(L"", L"", StatusVoice::announced) &&
             !status_is_announced(L"Saved", L"", StatusVoice::announced),
         "an empty status line is never spoken");
  expect(!status_is_announced(L"", L"Saved", StatusVoice::silent) &&
             !status_is_announced(L"Failed", L"Failed", StatusVoice::silent),
         "a silent message is never spoken");
  expect(status_is_announced(L"", L"Saved", StatusVoice::announced) &&
             status_is_announced(L"Saved", L"Saved", StatusVoice::announced),
         "an explicit outcome is spoken again even when it repeats");
  expect(status_is_announced(L"", L"Failed", StatusVoice::changed) &&
             !status_is_announced(L"Failed", L"Failed",
                                  StatusVoice::changed) &&
             status_is_announced(L"Failed", L"Other", StatusVoice::changed),
         "a repeating failure is spoken once, then only after another one");
}
