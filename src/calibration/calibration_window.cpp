#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"
#include "layerc/platform/editor_appearance.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

namespace layerc::calibration {
namespace {


std::wstring color_text(const std::uint32_t color) {
  return layerc::platform::utf8_to_wide(layerc::core::json::color_text(color));
}

bool contains_binding(const layerc::core::GameProfile &profile,
                      const std::string_view cooldown_id) {
  return std::ranges::any_of(profile.bindings, [&](const auto &binding) {
    return binding.cooldown_id == cooldown_id;
  });
}

// Combo-box indices are 0=first choice, 1=second, 2=third; CB_ERR (-1) and any
// unexpected value fall back to the middle default, matching the historical
// nested-ternary behavior.
std::uint8_t sample_rate_from_combobox(const LRESULT index) {
  if (index == 0) return 5;
  if (index == 2) return 15;
  return 10;
}

layerc::core::CaptureQuality capture_quality_from_combobox(
    const LRESULT index) {
  if (index == 0) return layerc::core::CaptureQuality::low;
  if (index == 2) return layerc::core::CaptureQuality::exact;
  return layerc::core::CaptureQuality::balanced;
}

WPARAM sample_rate_combobox_index(const std::uint8_t sample_rate_hz) {
  if (sample_rate_hz == 5) return 0;
  if (sample_rate_hz == 15) return 2;
  return 1;
}

WPARAM capture_quality_combobox_index(
    const layerc::core::CaptureQuality quality) {
  if (quality == layerc::core::CaptureQuality::low) return 0;
  if (quality == layerc::core::CaptureQuality::exact) return 2;
  return 1;
}

layerc::core::PixelRepeatPolicy repeat_policy_from_combobox(
    const LRESULT index) {
  return index == 1 ? layerc::core::PixelRepeatPolicy::when_fully_ready
                    : layerc::core::PixelRepeatPolicy::edge;
}

} // namespace

CalibrationWindow::CalibrationWindow(layerc::core::CalibrationRequest request)
    : request_(std::move(request)) {}

int CalibrationWindow::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  const auto migration = layerc::platform::migrate_editor_appearance();
  if (!migration) {
    MessageBoxW(nullptr, L"Could not migrate the editor appearance settings.",
                L"LayerC Calibration", MB_OK | MB_ICONWARNING);
  }
  layerc::platform::enable_native_dark_mode();
  const auto result =
      DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_CALIBRATION), nullptr,
                      &dialog_proc, reinterpret_cast<LPARAM>(this));
  return result == -1 ? 1 : 0;
}

INT_PTR CALLBACK CalibrationWindow::dialog_proc(const HWND window,
                                                const UINT message,
                                                const WPARAM wparam,
                                                const LPARAM lparam) {
  auto *calibration = reinterpret_cast<CalibrationWindow *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    calibration = reinterpret_cast<CalibrationWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER,
                      reinterpret_cast<LONG_PTR>(calibration));
    return calibration->initialize(window) ? TRUE : FALSE;
  }
  return calibration == nullptr
             ? FALSE
             : calibration->handle_message(window, message, wparam, lparam);
}

LRESULT CALLBACK CalibrationWindow::preview_proc(const HWND window,
                                                 const UINT message,
                                                 const WPARAM wparam,
                                                 const LPARAM lparam,
                                                 const UINT_PTR subclass_id,
                                                 const DWORD_PTR reference) {
  (void)subclass_id;
  auto *calibration = reinterpret_cast<CalibrationWindow *>(reference);
  if (calibration != nullptr) {
    const auto result =
        calibration->handle_preview(window, message, wparam, lparam);
    if (result != 0 || message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MOUSEMOVE || message == WM_LBUTTONUP ||
        message == WM_RBUTTONUP || message == WM_CAPTURECHANGED)
      return result;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

bool CalibrationWindow::initialize(const HWND window) {
  dialog_ = window;
  const auto reject = [this](const char *error) {
    response_.error = error;
    EndDialog(dialog_, IDCANCEL);
    return false;
  };
  target_window_ = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(request_.target_window));
  auto parsed = layerc::core::parse_profile_json(request_.profile_json);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (profile == nullptr || !IsWindow(target_window_)) {
    return reject("calibration profile or target is invalid");
  }
  profile_ = *profile;
  auto matching_profile = profile_;
  matching_profile.enabled = true;
  const auto candidate =
      layerc::platform::describe_attachable_window(target_window_);
  if (!candidate.has_value() ||
      !layerc::platform::profile_matches_window(*candidate, matching_profile)) {
    return reject("calibration target does not match the selected profile");
  }
  const auto cooldown =
      std::ranges::find_if(profile_.cooldowns, [&](const auto &definition) {
        return definition.id == request_.cooldown_id;
      });
  if (cooldown == profile_.cooldowns.end()) {
    return reject("selected calibration cooldown is invalid");
  }

  if (!request_.visual_json.empty()) {
    auto visual =
        layerc::core::parse_visual_profile_json(request_.visual_json, profile_);
    if (const auto *value = std::get_if<layerc::core::VisualProfile>(&visual)) {
      existing_visual_ = *value;
      sample_rate_hz_ = value->sample_rate_hz;
      quality_ = value->quality;
    } else {
      return reject("existing visual sidecar is invalid");
    }
  }

  trigger_ = {.cooldown_id = cooldown->id,
              .observation_id = cooldown->id + "-visual",
              .enabled = true,
              .mode = layerc::core::VisualTriggerMode::pixel_only,
              .keep_input = false};
  rule_ = {.id = trigger_.observation_id,
           .name = cooldown->name + " Ready",
           .region = {0.4, 0.4, 0.2, 0.2},
           .predicate = layerc::core::AverageColorPredicate{0xFFFFFF, 16}};
  template_state_ = {.id = trigger_.observation_id,
                     .name = cooldown->name + " Ready",
                     .search_region = rule_.region,
                     .expected_width = rule_.region.width,
                     .expected_height = rule_.region.height,
                     .references = {}};
  bool existing_template_assignment = false;
  if (existing_visual_) {
    const auto assignment = std::ranges::find_if(
        existing_visual_->triggers, [&](const auto &candidate_trigger) {
          return candidate_trigger.cooldown_id == cooldown->id;
        });
    if (assignment != existing_visual_->triggers.end()) {
      trigger_ = *assignment;
      rule_.id = trigger_.observation_id;
      template_state_.id = trigger_.observation_id;
      if (const auto *existing_rule = layerc::core::find_pixel_rule(
              *existing_visual_, trigger_.observation_id)) {
        rule_ = *existing_rule;
      } else if (const auto *existing_template =
                     layerc::core::find_template_state(
                         *existing_visual_, trigger_.observation_id)) {
        template_state_ = *existing_template;
        existing_template_assignment = true;
        restore_reference_selection();
        for (const auto &reference : template_state_.references) {
          auto loaded =
              layerc::platform::load_template_asset(reference.asset_sha256);
          if (auto *image = std::get_if<layerc::core::TemplateImage>(&loaded))
            template_images_.push_back(std::move(*image));
        }
      }
    } else {
      std::string base = trigger_.observation_id;
      unsigned suffix = 2;
      while (layerc::core::has_observation(*existing_visual_,
                                           trigger_.observation_id))
        trigger_.observation_id = base + "-" + std::to_string(suffix++);
      rule_.id = trigger_.observation_id;
      template_state_.id = trigger_.observation_id;
    }
  }
  if (!existing_template_assignment)
    set_reference_selection(rule_.region);
  if (const auto *average =
          std::get_if<layerc::core::AverageColorPredicate>(&rule_.predicate))
    target_color_ = average->target_color;
  else if (const auto *coverage =
               std::get_if<layerc::core::ColorCoveragePredicate>(
                   &rule_.predicate))
    target_color_ = coverage->target_color;

  initialize_palette();
  layerc::platform::set_dark_title_bar(dialog_);
  layerc::platform::set_window_app_icon(dialog_);
  background_brush_ = CreateSolidBrush(palette_.background);
  panel_brush_ = CreateSolidBrush(palette_.panel);
  input_brush_ = CreateSolidBrush(palette_.input);
  const UINT dpi = GetDpiForWindow(dialog_);
  font_ = layerc::platform::create_native_dialog_font(dpi);
  layerc::platform::style_native_dialog_children(dialog_, font_);
  preview_.attach(GetDlgItem(dialog_, IDC_CAL_PREVIEW));
  SetWindowSubclass(preview_.window(), &preview_proc, 1,
                    reinterpret_cast<DWORD_PTR>(this));
  initialize_controls();
  set_control_text(IDC_CAL_TARGET,
                   candidate->executable_name + L" — " + candidate->title);

  capture_ = std::make_unique<layerc::capture::WindowCaptureSession>(
      target_window_,
      layerc::capture::CaptureOptions{.allow_desktop_duplication = true,
                                      .quality = quality_});
  if (!capture_->start()) {
    set_control_text(IDC_CAL_BACKEND, L"Capture could not start.");
  }
  SetTimer(dialog_, capture_timer, 66, nullptr);
  return true;
}

void CalibrationWindow::initialize_palette() {
  const auto loaded = layerc::platform::read_editor_appearance();
  const auto appearance = loaded.value.value_or(
      layerc::platform::EditorAppearanceSettings{});
  const auto colors = [&](const layerc::platform::EditorControlRole role) {
    return layerc::platform::native_theme_colors(appearance, role);
  };
  const auto window =
      colors(layerc::platform::EditorControlRole::window);
  const auto panel = colors(layerc::platform::EditorControlRole::button);
  const auto input = colors(layerc::platform::EditorControlRole::edit);
  button_colors_ = panel;
  palette_ = {.background = window.fill,
              .panel = panel.fill,
              .input = input.fill,
              .border = panel.border,
              .text = window.text,
              .muted = window.disabled,
              .selection = panel.selection};
}

void CalibrationWindow::initialize_controls() {
  const auto add = [&](const int id, const std::wstring_view text_value) {
    SendDlgItemMessageW(dialog_, id, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(text_value.data()));
  };
  for (const auto &mode_spec : calibration_modes())
    add(IDC_CAL_PREDICATE, mode_spec.label);
  add(IDC_CAL_COMPARISON, L"Above");
  add(IDC_CAL_COMPARISON, L"Below");
  add(IDC_CAL_REPEAT, L"Edge");
  add(IDC_CAL_REPEAT, L"When ready");
  add(IDC_CAL_TRIGGER_MODE, L"Pixel only");
  add(IDC_CAL_TRIGGER_MODE, L"Confirm input");
  add(IDC_CAL_UNAVAILABLE, L"Ignore input (fail closed)");
  add(IDC_CAL_UNAVAILABLE, L"Trigger from input");
  add(IDC_CAL_SAMPLE_RATE, L"5 Hz");
  add(IDC_CAL_SAMPLE_RATE, L"10 Hz");
  add(IDC_CAL_SAMPLE_RATE, L"15 Hz");
  add(IDC_CAL_QUALITY, L"Low");
  add(IDC_CAL_QUALITY, L"Balanced");
  add(IDC_CAL_QUALITY, L"Exact");

  CalibrationMode selected_mode = CalibrationMode::average_color;
  int tolerance = 16;
  int threshold = 128;
  int comparison{};
  int minimum_percent = 50;
  if (!template_state_.references.empty()) {
    selected_mode = CalibrationMode::template_match;
    tolerance = template_state_.color_tolerance;
  } else if (const auto *average =
                 std::get_if<layerc::core::AverageColorPredicate>(
                     &rule_.predicate)) {
    tolerance = average->tolerance;
  } else if (const auto *brightness =
                 std::get_if<layerc::core::BrightnessPredicate>(
                     &rule_.predicate)) {
    selected_mode = CalibrationMode::brightness;
    threshold = brightness->threshold;
    comparison =
        brightness->comparison == layerc::core::BrightnessComparison::below ? 1
                                                                            : 0;
  } else if (const auto *coverage =
                 std::get_if<layerc::core::ColorCoveragePredicate>(
                     &rule_.predicate)) {
    selected_mode = CalibrationMode::coverage;
    tolerance = coverage->tolerance;
    minimum_percent =
        static_cast<int>(std::lround(coverage->minimum_fraction * 100.0));
  }
  SendDlgItemMessageW(dialog_, IDC_CAL_PREDICATE, CB_SETCURSEL,
                      calibration_mode_index(selected_mode), 0);
  SendDlgItemMessageW(dialog_, IDC_CAL_COMPARISON, CB_SETCURSEL, comparison, 0);
  SendDlgItemMessageW(
      dialog_, IDC_CAL_REPEAT, CB_SETCURSEL,
      (selected_mode == CalibrationMode::template_match ? template_state_.repeat
                                                        : rule_.repeat) ==
              layerc::core::PixelRepeatPolicy::when_fully_ready
          ? 1
          : 0,
      0);
  SendDlgItemMessageW(
      dialog_, IDC_CAL_TRIGGER_MODE, CB_SETCURSEL,
      trigger_.mode == layerc::core::VisualTriggerMode::confirm_input ? 1 : 0,
      0);
  SendDlgItemMessageW(
      dialog_, IDC_CAL_UNAVAILABLE, CB_SETCURSEL,
      trigger_.unavailable_policy ==
              layerc::core::CaptureUnavailablePolicy::trigger_from_input
          ? 1
          : 0,
      0);
  SendDlgItemMessageW(dialog_, IDC_CAL_SAMPLE_RATE, CB_SETCURSEL,
                      sample_rate_combobox_index(sample_rate_hz_), 0);
  SendDlgItemMessageW(dialog_, IDC_CAL_QUALITY, CB_SETCURSEL,
                      capture_quality_combobox_index(quality_), 0);
  CheckDlgButton(dialog_, IDC_CAL_KEEP_INPUT,
                 trigger_.keep_input ? BST_CHECKED : BST_UNCHECKED);
  set_control_text(IDC_CAL_COLOR_VALUE, color_text(target_color_));
  set_control_text(IDC_CAL_TOLERANCE, std::to_wstring(tolerance));
  set_control_text(IDC_CAL_THRESHOLD, std::to_wstring(threshold));
  set_control_text(IDC_CAL_MINIMUM_PERCENT, std::to_wstring(minimum_percent));
  set_control_text(IDC_CAL_STABLE_SAMPLES,
                   std::to_wstring(
                       selected_mode == CalibrationMode::template_match
                           ? template_state_.stable_samples
                           : rule_.stable_samples));
  set_control_text(IDC_CAL_CONFIDENCE,
                   std::to_wstring(static_cast<int>(std::lround(
                       template_state_.confidence_threshold * 100.0))));
  set_control_text(IDC_CAL_SCALE_TOLERANCE,
                   std::to_wstring(static_cast<int>(
                       std::lround(template_state_.scale_tolerance * 100.0))));
  set_control_text(IDC_CAL_TIMEOUT,
                   std::to_wstring(trigger_.confirmation_timeout.count()));
  refresh_template_reference_label();
  update_control_enablement();
}

void CalibrationWindow::update_control_enablement() {
  const auto &mode_spec = calibration_mode_spec(mode());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_PREDICATE), !mask_editor_.editing());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_SAMPLE_COLOR),
               mode_spec.color_controls);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COLOR_VALUE),
               mode_spec.color_controls);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_TOLERANCE),
               mode_spec.color_controls);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COMPARISON),
               mode_spec.brightness_controls);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_THRESHOLD),
               mode_spec.brightness_controls);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_MINIMUM_PERCENT),
               mode_spec.coverage_controls);
  constexpr std::array template_controls{
      IDC_CAL_TEMPLATE_GROUP,    IDC_CAL_CAPTURE_TEMPLATE,
      IDC_CAL_IMPORT_TEMPLATE,   IDC_CAL_TEMPLATE_REFERENCES,
      IDC_CAL_CONFIDENCE,        IDC_CAL_SCALE_TOLERANCE,
      IDC_CAL_CLEAR_TEMPLATES,   IDC_CAL_CONFIDENCE_LABEL,
      IDC_CAL_SCALE_LABEL,       IDC_CAL_TEMPLATE_HELP,
      IDC_CAL_EDIT_TEMPLATE_MASK};
  for (const int id : template_controls)
    ShowWindow(GetDlgItem(dialog_, id), mode_spec.template_controls ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CLEAR_TEMPLATES),
               mode_spec.template_controls && !mask_editor_.editing() &&
                   !template_state_.references.empty());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CAPTURE_TEMPLATE),
               mode_spec.template_controls && !mask_editor_.editing());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_IMPORT_TEMPLATE),
               mode_spec.template_controls && !mask_editor_.editing());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK),
               mode_spec.template_controls &&
                   (mask_editor_.editing() || (template_images_.size() ==
                                          template_state_.references.size() &&
                                      !template_images_.empty())));

  const bool confirmation = SendDlgItemMessageW(dialog_, IDC_CAL_TRIGGER_MODE,
                                                CB_GETCURSEL, 0, 0) == 1;
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_KEEP_INPUT), !confirmation);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_TIMEOUT), confirmation);
}

bool CalibrationWindow::handle_command(const WPARAM wparam) {
  const int id = LOWORD(wparam);
  const int notification = HIWORD(wparam);
  if ((id == IDC_CAL_PREDICATE || id == IDC_CAL_TRIGGER_MODE) &&
      notification == CBN_SELCHANGE) {
    update_control_enablement();
    update_match_status();
    return true;
  }
  if (id == IDC_CAL_QUALITY && notification == CBN_SELCHANGE) {
    const auto selection =
        SendDlgItemMessageW(dialog_, IDC_CAL_QUALITY, CB_GETCURSEL, 0, 0);
    quality_ = capture_quality_from_combobox(selection);
    if (capture_)
      capture_->set_quality(quality_);
    if (fallback_snapshot_ && capture_) {
      fallback_snapshot_ = false;
      preview_.clear_frame();
      begin_fallback_snapshot();
    }
    return true;
  }
  if (id == IDC_CAL_SAMPLE_COLOR && notification == BN_CLICKED) {
    sample_target_color();
    return true;
  }
  if (id == IDC_CAL_CAPTURE_TEMPLATE && notification == BN_CLICKED) {
    capture_template_reference();
    return true;
  }
  if (id == IDC_CAL_IMPORT_TEMPLATE && notification == BN_CLICKED) {
    import_template_reference();
    return true;
  }
  if (id == IDC_CAL_CLEAR_TEMPLATES && notification == BN_CLICKED) {
    clear_template_references();
    return true;
  }
  if (id == IDC_CAL_EDIT_TEMPLATE_MASK && notification == BN_CLICKED) {
    toggle_template_mask_editing();
    return true;
  }
  if (id == IDC_CAL_APPLY && notification == BN_CLICKED) {
    if (apply())
      EndDialog(dialog_, IDOK);
    return true;
  }
  if (id == IDCANCEL) {
    EndDialog(dialog_, IDCANCEL);
    return true;
  }
  return false;
}

INT_PTR CalibrationWindow::handle_message(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  const auto color_control = [&](const COLORREF background,
                                 const HBRUSH brush) {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    const HWND control = reinterpret_cast<HWND>(lparam);
    SetTextColor(dc,
                 IsWindowEnabled(control) ? palette_.text : palette_.muted);
    SetBkColor(dc, background);
    return reinterpret_cast<INT_PTR>(brush);
  };
  switch (message) {
  case WM_TIMER:
    if (wparam == capture_timer) {
      update_capture();
      return TRUE;
    }
    break;
  case WM_COMMAND:
    if (handle_command(wparam))
      return TRUE;
    break;
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlID == IDC_CAL_PREVIEW) {
      paint_preview(*draw);
      return TRUE;
    }
    if (draw->CtlType == ODT_BUTTON) {
      layerc::platform::paint_native_button(*draw, button_colors_, font_);
      return TRUE;
    }
    break;
  }
  case WM_CTLCOLORDLG:
    return reinterpret_cast<INT_PTR>(background_brush_);
  case WM_CTLCOLORSTATIC:
    return color_control(palette_.background, background_brush_);
  case WM_CTLCOLORBTN:
    return color_control(palette_.panel, panel_brush_);
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    return color_control(palette_.input, input_brush_);
  case WM_ERASEBKGND: {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
    return TRUE;
  }
  case WM_CLOSE:
    EndDialog(window, IDCANCEL);
    return TRUE;
  case WM_DESTROY:
    KillTimer(window, capture_timer);
    if (capture_)
      capture_->stop();
    if (preview_.window() != nullptr)
      RemoveWindowSubclass(preview_.window(), &preview_proc, 1);
    if (font_ != nullptr)
      DeleteObject(font_);
    if (input_brush_ != nullptr)
      DeleteObject(input_brush_);
    if (panel_brush_ != nullptr)
      DeleteObject(panel_brush_);
    if (background_brush_ != nullptr)
      DeleteObject(background_brush_);
    return TRUE;
  default:
    break;
  }
  return FALSE;
}

LRESULT CalibrationWindow::handle_preview(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  (void)wparam;
  const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  switch (message) {
  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
    if (mask_editor_.editing()) {
      preview_.begin_drag(point);
      mask_editor_.set_brush_opaque(message == WM_RBUTTONDOWN);
      SetCapture(window);
      update_template_mask(point, mask_editor_.brush_opaque());
      return 0;
    }
    if (message == WM_RBUTTONDOWN)
      return 0;
    preview_.begin_drag(point);
    SetCapture(window);
    update_region_from_pointer(point, false);
    return 0;
  case WM_MOUSEMOVE:
    if (preview_.dragging()) {
      if (mask_editor_.editing())
        update_template_mask(point, mask_editor_.brush_opaque());
      else
        update_region_from_pointer(point, false);
    }
    return 0;
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
    if (preview_.dragging()) {
      if (mask_editor_.editing())
        update_template_mask(point, mask_editor_.brush_opaque());
      else if (message == WM_LBUTTONUP)
        update_region_from_pointer(point, true);
      preview_.finish_drag();
      if (GetCapture() == window)
        ReleaseCapture();
    }
    return 0;
  case WM_CAPTURECHANGED:
    preview_.finish_drag();
    return 0;
  default:
    return 0;
  }
}

void CalibrationWindow::sample_target_color() {
  const auto &frame = preview_.frame();
  if (!frame) {
    MessageBoxW(dialog_, L"Wait for a captured frame first.", L"LayerC",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  auto sample = rule_;
  sample.predicate = layerc::core::AverageColorPredicate{0, 255};
  const auto evaluation =
      layerc::core::evaluate_pixel_rule(sample, frame->view(), quality_);
  if (evaluation.sampled_pixels == 0)
    return;
  target_color_ = evaluation.average_color;
  set_control_text(IDC_CAL_COLOR_VALUE, color_text(target_color_));
  update_match_status();
}

CalibrationMode CalibrationWindow::mode() const {
  return dialog_ == nullptr
             ? CalibrationMode::average_color
             : calibration_mode_from_index(SendDlgItemMessageW(
                   dialog_, IDC_CAL_PREDICATE, CB_GETCURSEL, 0, 0));
}

layerc::core::TemplateState
CalibrationWindow::current_template_from_controls() const {
  auto state = template_state_;
  state.confidence_threshold =
      integer_control(IDC_CAL_CONFIDENCE, 90, 0, 100) / 100.0;
  state.scale_tolerance =
      integer_control(IDC_CAL_SCALE_TOLERANCE, 5, 0, 20) / 100.0;
  state.color_tolerance =
      static_cast<std::uint8_t>(integer_control(IDC_CAL_TOLERANCE, 24, 0, 255));
  state.stable_samples = static_cast<std::uint8_t>(
      integer_control(IDC_CAL_STABLE_SAMPLES, 2, 1, 15));
  state.repeat = repeat_policy_from_combobox(
      SendDlgItemMessageW(dialog_, IDC_CAL_REPEAT, CB_GETCURSEL, 0, 0));
  return state;
}

void CalibrationWindow::refresh_template_reference_label() {
  std::wstring label =
      std::to_wstring(template_state_.references.size()) + L" / " +
      std::to_wstring(layerc::core::max_template_references) + L" references";
  if (template_images_.size() != template_state_.references.size())
    label += L" (one or more assets unavailable)";
  set_control_text(IDC_CAL_TEMPLATE_REFERENCES, label);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CLEAR_TEMPLATES),
               !template_state_.references.empty());
}

layerc::core::PixelRule CalibrationWindow::current_rule_from_controls() const {
  auto rule = rule_;
  rule.stable_samples = static_cast<std::uint8_t>(
      integer_control(IDC_CAL_STABLE_SAMPLES, rule.stable_samples, 1, 15));
  rule.repeat = repeat_policy_from_combobox(
      SendDlgItemMessageW(dialog_, IDC_CAL_REPEAT, CB_GETCURSEL, 0, 0));
  switch (mode()) {
  case CalibrationMode::brightness:
    rule.predicate = layerc::core::BrightnessPredicate{
        static_cast<std::uint8_t>(
            integer_control(IDC_CAL_THRESHOLD, 128, 0, 255)),
        SendDlgItemMessageW(dialog_, IDC_CAL_COMPARISON, CB_GETCURSEL, 0, 0) ==
                1
            ? layerc::core::BrightnessComparison::below
            : layerc::core::BrightnessComparison::above};
    break;
  case CalibrationMode::coverage:
    rule.predicate = layerc::core::ColorCoveragePredicate{
        target_color_,
        static_cast<std::uint8_t>(
            integer_control(IDC_CAL_TOLERANCE, 16, 0, 255)),
        integer_control(IDC_CAL_MINIMUM_PERCENT, 50, 0, 100) / 100.0};
    break;
  case CalibrationMode::average_color:
  case CalibrationMode::template_match:
    rule.predicate = layerc::core::AverageColorPredicate{
        target_color_, static_cast<std::uint8_t>(
                           integer_control(IDC_CAL_TOLERANCE, 16, 0, 255))};
    break;
  }
  return rule;
}

void CalibrationWindow::update_match_status(const bool force) {
  if (mask_editor_.editing()) {
    set_control_text(IDC_CAL_MATCH, L"Editing alpha mask — left drag ignores "
                                    L"pixels; right drag restores them.");
    return;
  }
  const auto &frame = preview_.frame();
  if (!frame)
    return;
  if (mode() == CalibrationMode::template_match) {
    if (template_state_.references.empty()) {
      set_control_text(IDC_CAL_MATCH,
                       L"Capture or import at least one template reference.");
      return;
    }
    if (template_images_.size() != template_state_.references.size()) {
      set_control_text(
          IDC_CAL_MATCH,
          L"One or more referenced template assets are unavailable.");
      return;
    }
    const auto now = GetTickCount64();
    if (!force && now < next_template_match_at_)
      return;
    // Exact can consume the full 8M-comparison allowance. Keep that bounded
    // evaluation off the 15 Hz paint/capture cadence so the modal UI remains
    // responsive while still refreshing live status twice per second.
    next_template_match_at_ = now + 500;
    const auto state = current_template_from_controls();
    const auto result = template_matcher_.evaluate(
        state, template_images_, frame->view(), quality_,
        frame->available_region, frame->unavailable_regions);
    if (!result.search_region_available) {
      set_control_text(
          IDC_CAL_MATCH,
          L"Template search region is outside the capturable window area.");
      return;
    }
    std::wstring status =
        result.matched ? L"TEMPLATE MATCHED" : L"Template not matched";
    status += L"\nConfidence " +
              std::to_wstring(
                  static_cast<int>(std::lround(result.confidence * 100.0))) +
              L"%, candidates " + std::to_wstring(result.candidates_evaluated);
    set_control_text(IDC_CAL_MATCH, status);
    return;
  }
  const auto rule = current_rule_from_controls();
  const auto result =
      layerc::core::evaluate_pixel_rule(rule, frame->view(), quality_);
  std::wstring status = result.matched ? L"MATCHED now" : L"Not matched now";
  status += L"\nAverage " + color_text(result.average_color) +
            L", brightness " +
            std::to_wstring(
                static_cast<int>(std::lround(result.average_brightness))) +
            L", coverage " +
            std::to_wstring(
                static_cast<int>(std::lround(result.color_coverage * 100.0))) +
            L"%";
  set_control_text(IDC_CAL_MATCH, status);
}

bool CalibrationWindow::apply() {
  if (!finish_template_mask_editing())
    return false;
  const bool use_template = mode() == CalibrationMode::template_match;
  if (use_template) {
    template_state_ = current_template_from_controls();
    template_state_.id = trigger_.observation_id;
    if (template_state_.references.empty()) {
      MessageBoxW(dialog_,
                  L"Capture or import at least one template reference first.",
                  L"Template reference required", MB_OK | MB_ICONINFORMATION);
      return false;
    }
    if (template_images_.size() != template_state_.references.size() ||
        !std::ranges::all_of(template_images_,
                             layerc::core::template_image_has_structure)) {
      MessageBoxW(dialog_,
                  L"Every referenced template asset must be available and "
                  L"contain visible visual structure.",
                  L"Template assets unavailable", MB_OK | MB_ICONWARNING);
      return false;
    }
  } else {
    rule_ = current_rule_from_controls();
    rule_.id = trigger_.observation_id;
  }
  const auto rate_index =
      SendDlgItemMessageW(dialog_, IDC_CAL_SAMPLE_RATE, CB_GETCURSEL, 0, 0);
  sample_rate_hz_ = sample_rate_from_combobox(rate_index);
  const auto quality_index =
      SendDlgItemMessageW(dialog_, IDC_CAL_QUALITY, CB_GETCURSEL, 0, 0);
  quality_ = capture_quality_from_combobox(quality_index);
  if (quality_ == layerc::core::CaptureQuality::exact &&
      MessageBoxW(
          dialog_,
          L"Exact retains full-resolution pixel and template data and is not "
          L"covered by the V2.1 Low/Balanced CPU/memory budget. Continue?",
          L"Exact quality", MB_YESNO | MB_ICONWARNING) != IDYES)
    return false;

  trigger_.enabled = true;
  trigger_.mode = SendDlgItemMessageW(dialog_, IDC_CAL_TRIGGER_MODE,
                                      CB_GETCURSEL, 0, 0) == 1
                      ? layerc::core::VisualTriggerMode::confirm_input
                      : layerc::core::VisualTriggerMode::pixel_only;
  trigger_.keep_input =
      IsDlgButtonChecked(dialog_, IDC_CAL_KEEP_INPUT) == BST_CHECKED;
  trigger_.confirmation_timeout = layerc::core::Milliseconds(
      integer_control(IDC_CAL_TIMEOUT, 500, 50, 5'000));
  trigger_.unavailable_policy =
      SendDlgItemMessageW(dialog_, IDC_CAL_UNAVAILABLE, CB_GETCURSEL, 0, 0) == 1
          ? layerc::core::CaptureUnavailablePolicy::trigger_from_input
          : layerc::core::CaptureUnavailablePolicy::ignore;
  if (trigger_.mode == layerc::core::VisualTriggerMode::confirm_input &&
      !contains_binding(profile_, trigger_.cooldown_id)) {
    MessageBoxW(dialog_,
                L"Input confirmation requires a captured input binding for "
                L"this cooldown.",
                L"LayerC", MB_OK | MB_ICONWARNING);
    return false;
  }

  layerc::core::VisualProfile result;
  result.profile_id = profile_.id;
  result.sample_rate_hz = sample_rate_hz_;
  result.quality = quality_;
  if (use_template)
    result.templates.push_back(template_state_);
  else
    result.rules.push_back(rule_);
  result.triggers.push_back(trigger_);
  const auto validation =
      layerc::core::validate_visual_profile(result, profile_);
  if (!validation.message.empty()) {
    MessageBoxW(dialog_,
                layerc::platform::utf8_to_wide(validation.message).c_str(),
                L"Visual rule is invalid", MB_OK | MB_ICONWARNING);
    return false;
  }
  response_.applied = true;
  response_.visual_json = layerc::core::serialize_visual_profile_json(result);
  return true;
}

int CalibrationWindow::integer_control(const int id, const int fallback,
                                       const int minimum,
                                       const int maximum) const {
  BOOL translated{};
  const auto value = GetDlgItemInt(dialog_, id, &translated, FALSE);
  return translated ? std::clamp<int>(static_cast<int>(value), minimum, maximum)
                    : fallback;
}

void CalibrationWindow::set_control_text(const int id,
                                         const std::wstring_view value) const {
  SetDlgItemTextW(dialog_, id, std::wstring(value).c_str());
}

} // namespace layerc::calibration
