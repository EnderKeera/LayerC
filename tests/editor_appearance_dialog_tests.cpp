#include "appearance_draft.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

namespace layerc::test::editor_dialog {

void appearance_draft_contracts(bool &passed) {
  layerc::platform::EditorAppearanceSettings initial_appearance;
  initial_appearance.theme = layerc::platform::EditorTheme::copper;
  initial_appearance.accent = 0xD88E4BU;
  layerc::editor::AppearanceDraft appearance_draft;
  appearance_draft.begin(initial_appearance);
  appearance_draft.staged().theme = layerc::platform::EditorTheme::teal;
  appearance_draft.staged().overrides.front().fields =
      layerc::platform::editor_fill;
  appearance_draft.cancel();
  passed &= expect(
      appearance_draft.staged().theme == initial_appearance.theme &&
          appearance_draft.staged().overrides.front().fields == 0,
      "cancelling appearance edits should restore the persisted settings");

  appearance_draft.staged().theme = layerc::platform::EditorTheme::plum;
  bool save_received_staged{};
  passed &= expect(
      appearance_draft.apply(
          [&save_received_staged](
              const layerc::platform::EditorAppearanceSettings &settings) {
            save_received_staged =
                settings.theme == layerc::platform::EditorTheme::plum;
            return true;
          }) &&
          save_received_staged,
      "applying appearance edits should persist the complete staged value");
  appearance_draft.staged().theme = layerc::platform::EditorTheme::dark;
  passed &= expect(
      !appearance_draft.apply(
          [](const layerc::platform::EditorAppearanceSettings &) {
            return false;
          }),
      "a failed appearance save should report failure");
  appearance_draft.cancel();
  passed &= expect(
      appearance_draft.staged().theme == layerc::platform::EditorTheme::plum,
      "a failed appearance save should preserve the last persisted settings");
}

void appearance_dialog_contracts(const HINSTANCE instance, bool &passed) {
  const HWND appearance_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_APPEARANCE), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      appearance_dialog != nullptr &&
          GetDlgItem(appearance_dialog, IDC_THEME_PRESET) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_ADVANCED) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_RESET_ALL) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_APPLY) != nullptr,
      "Appearance should expose preset-first controls and staged actions");
  if (appearance_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        appearance_dialog,
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND advanced =
        GetDlgItem(appearance_dialog, IDC_APPEARANCE_ADVANCED);
    const auto advanced_type =
        GetWindowLongPtrW(advanced, GWL_STYLE) & BS_TYPEMASK;
    passed &= expect(
        advanced_type == BS_PUSHBUTTON || advanced_type == BS_OWNERDRAW,
        "advanced appearance colors must be a disclosure button, not a "
        "checkbox that looks like a saved setting");
    passed &= expect(
        LOWORD(SendMessageW(appearance_dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "OK should remain the Appearance default action");
    DestroyWindow(appearance_dialog);
  }
}

} // namespace layerc::test::editor_dialog
