#include "calibration_mode.hpp"
#include "calibration_preview.hpp"
#include "calibration_resource.h"
#include "test_support.hpp"

#include <array>
#include <algorithm>

#include <windows.h>

void calibration_reference_geometry_contracts();
void calibration_reference_loaded_contracts();
void calibration_reference_negative_contracts();
void calibration_sampling_readiness_contracts();

namespace {

void calibration_modes_round_trip() {
  using namespace layerc::calibration;
  constexpr std::array expected{
      CalibrationMode::average_color,
      CalibrationMode::brightness,
      CalibrationMode::coverage,
      CalibrationMode::template_match,
  };
  const auto modes = calibration_modes();
  layerc::test::expect(modes.size() == expected.size(),
                       "calibration mode table should list every supported mode");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto &spec = calibration_mode_spec(expected[index]);
    layerc::test::expect(
        calibration_mode_from_index(static_cast<std::ptrdiff_t>(index)) ==
                expected[index] &&
            calibration_mode_index(expected[index]) == index &&
            spec.mode == expected[index] && !spec.label.empty(),
        "calibration mode indices, labels, and typed values should round-trip");
  }
  layerc::test::expect(
      calibration_mode_from_index(-1) == CalibrationMode::average_color &&
          calibration_mode_from_index(
              static_cast<std::ptrdiff_t>(expected.size())) ==
              CalibrationMode::average_color,
      "invalid combo selections should use the documented average-color fallback");
  layerc::test::expect(
      modes[0].color_controls && !modes[0].brightness_controls &&
          modes[1].brightness_controls && modes[2].coverage_controls &&
          modes[3].template_controls,
      "each calibration mode should own its control-group policy");
}

void calibration_preview_components() {
  using namespace layerc::calibration;

  CapturePreview preview;
  preview.begin_drag({7, 9});
  layerc::test::expect(preview.dragging() && preview.drag_start().x == 7 &&
                           preview.drag_start().y == 9,
                       "capture preview should own drag state");
  preview.finish_drag();
  layerc::test::expect(!preview.dragging(),
                       "capture preview should finish a drag atomically");

  auto frame = std::make_shared<layerc::capture::CaptureFrame>();
  frame->width = 2;
  frame->height = 1;
  frame->pixels = {{.red = 1, .green = 2, .blue = 3},
                   {.red = 4, .green = 5, .blue = 6}};
  preview.set_frame(frame);
  layerc::test::expect(
      preview.frame() == frame && preview.pixels().size() == 2 &&
          preview.pixels()[0] == 0x010203U &&
          preview.pixels()[1] == 0x040506U,
      "capture preview should own the captured frame and display conversion");
  preview.clear_frame();
  layerc::test::expect(!preview.frame() && preview.pixels().empty(),
                       "capture preview should clear frame state together");

  layerc::core::TemplateImage image;
  image.width = 16;
  image.height = 16;
  image.pixels.assign(256, {.red = 20, .green = 40, .blue = 60});
  image.alpha.assign(256, 255);
  MaskEditor mask;
  mask.begin(image);
  layerc::test::expect(mask.editing() && !mask.changed() &&
                           mask.pixels().size() == image.pixels.size(),
                       "mask editor should begin with a complete preview");
  const RECT bounds{0, 0, 160, 160};
  layerc::test::expect(
      mask.update(image, {80, 80}, bounds, false) && mask.changed() &&
          image.alpha[8U * image.width + 8U] == 0,
      "mask editor should apply an ignore brush and own dirty state");
  layerc::test::expect(!mask.update(image, {-1, 80}, bounds, false),
                       "mask editor should reject points outside the image");
  mask.set_brush_opaque(true);
  layerc::test::expect(
      mask.update(image, {80, 80}, bounds, mask.brush_opaque()) &&
          image.alpha[8U * image.width + 8U] == 255,
      "mask editor should restore ignored pixels with the opaque brush");
  mask.finish();
  layerc::test::expect(!mask.editing() && !mask.changed() &&
                           mask.pixels().empty(),
                       "mask editor should release transient state on finish");
}

INT_PTR calibration_test_dialog_proc(HWND, const UINT message, WPARAM, LPARAM) {
  return message == WM_INITDIALOG ? TRUE : FALSE;
}

void calibration_resource_contract() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_CALIBRATION), nullptr,
                         &calibration_test_dialog_proc, 0);
  if (dialog == nullptr) {
    layerc::test::expect(false, "the native calibration dialog should load");
    return;
  }
  layerc::test::expect(true, "the native calibration dialog should load");
  layerc::test::expect(
      GetDlgItem(dialog, IDC_CAL_CAPTURE_TEMPLATE) != nullptr &&
          GetDlgItem(dialog, IDC_CAL_IMPORT_TEMPLATE) != nullptr &&
          GetDlgItem(dialog, IDC_CAL_EDIT_TEMPLATE_MASK) != nullptr,
      "the calibration resource should expose capture, import, and mask controls");

  RECT window_rect{};
  const bool has_window_rect = GetWindowRect(dialog, &window_rect) != FALSE;
  layerc::test::expect(has_window_rect,
                       "the calibration window rectangle should be readable");
  const UINT dpi = std::max<UINT>(96, GetDpiForWindow(dialog));
  const int logical_width = MulDiv(window_rect.right - window_rect.left, 96, dpi);
  const int logical_height =
      MulDiv(window_rect.bottom - window_rect.top, 96, dpi);
  layerc::test::expect(
      has_window_rect && logical_width <= 1600 && logical_height <= 1040,
      "the calibration dialog should fit a 1920x1080 work area");

  RECT client{};
  const bool has_client_rect = GetClientRect(dialog, &client) != FALSE;
  layerc::test::expect(has_client_rect,
                       "the calibration client rectangle should be readable");
  const int button_ids[] = {IDC_CAL_APPLY, IDCANCEL};
  for (const int id : button_ids) {
    const HWND control = GetDlgItem(dialog, id);
    if (control == nullptr) {
      layerc::test::expect(false,
                           "calibration Apply and Cancel controls should exist");
      continue;
    }
    RECT button{};
    const bool has_button_rect = GetWindowRect(control, &button) != FALSE;
    if (has_button_rect)
      MapWindowPoints(HWND_DESKTOP, dialog,
                      reinterpret_cast<POINT *>(&button), 2);
    layerc::test::expect(
        has_client_rect && has_button_rect && button.left >= client.left &&
            button.top >= client.top && button.right <= client.right &&
            button.bottom <= client.bottom,
        "calibration Apply and Cancel should remain inside the client area");
  }
  DestroyWindow(dialog);
}

} // namespace

int main() {
  return layerc::test::run({
      {"calibration_modes_round_trip", calibration_modes_round_trip},
      {"calibration_preview_components", calibration_preview_components},
      {"calibration_resource_contract", calibration_resource_contract},
      {"calibration_reference_geometry_contracts",
       calibration_reference_geometry_contracts},
      {"calibration_reference_loaded_contracts",
       calibration_reference_loaded_contracts},
      {"calibration_reference_negative_contracts",
       calibration_reference_negative_contracts},
      {"calibration_sampling_readiness_contracts",
       calibration_sampling_readiness_contracts},
  });
}
