#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <cstddef>
#include <filesystem>
#include <variant>
#include <vector>

#include <commdlg.h>

namespace layerc::calibration {
namespace {

[[nodiscard]] bool
reject_when_reference_limit_reached(const HWND dialog,
                                    const std::size_t reference_count) {
  if (reference_count < layerc::core::max_template_references)
    return false;
  MessageBoxW(dialog, L"A template state supports at most four references.",
              L"Template reference limit", MB_OK | MB_ICONINFORMATION);
  return true;
}

void show_asset_error(const HWND dialog,
                      const layerc::platform::VisualAssetError &error,
                      const wchar_t *const title) {
  MessageBoxW(dialog, layerc::platform::utf8_to_wide(error.message).c_str(),
              title, MB_OK | MB_ICONERROR);
}

} // namespace

std::optional<layerc::core::TemplateImage>
CalibrationWindow::captured_reference_image() const {
  const auto &frame = preview_.frame();
  if (!frame)
    return std::nullopt;
  return layerc::core::template_from_selection(frame->view(),
                                               model_->selection());
}

bool CalibrationWindow::append_template_reference(
    const layerc::platform::ImportedTemplateAsset &asset) {
  if (!model_ || !model_->add_reference(asset.reference))
    return false;
  template_images_.push_back(asset.image);
  template_matcher_.clear();
  refresh_template_reference_label();
  update_match_status();
  preview_.invalidate();
  return true;
}

void CalibrationWindow::capture_template_reference() {
  const auto &frame = preview_.frame();
  if (!frame) {
    MessageBoxW(dialog_, L"Wait for a captured frame first.", L"LayerC",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (reject_when_reference_limit_reached(dialog_, model_->references().size()))
    return;
  const auto image = captured_reference_image();
  if (!image) {
    MessageBoxW(dialog_,
                L"Select a region containing at least 64 captured pixels.",
                L"Template region too small", MB_OK | MB_ICONINFORMATION);
    return;
  }
  auto stored = layerc::platform::store_template_asset(*image);
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&stored);
  if (asset == nullptr) {
    show_asset_error(dialog_,
                     std::get<layerc::platform::VisualAssetError>(stored),
                     L"Template capture failed");
    return;
  }
  (void)append_template_reference(*asset);
}

void CalibrationWindow::import_template_reference() {
  if (reject_when_reference_limit_reached(dialog_, model_->references().size()))
    return;
  std::vector<wchar_t> path(32'768);
  OPENFILENAMEW picker{};
  picker.lStructSize = sizeof(OPENFILENAMEW);
  picker.hwndOwner = dialog_;
  picker.lpstrFilter = L"PNG images (*.png)\0*.png\0\0";
  picker.lpstrFile = path.data();
  picker.nMaxFile = static_cast<DWORD>(path.size());
  picker.lpstrDefExt = L"png";
  picker.Flags =
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
  if (GetOpenFileNameW(&picker) == FALSE) return;
  auto imported =
      layerc::platform::import_template_asset(std::filesystem::path(path.data()));
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&imported);
  if (asset == nullptr) {
    show_asset_error(dialog_,
                     std::get<layerc::platform::VisualAssetError>(imported),
                     L"Template import failed");
    return;
  }
  (void)append_template_reference(*asset);
}

void CalibrationWindow::clear_template_references() {
  mask_editor_.clear();
  model_->clear_references();
  template_images_.clear();
  template_matcher_.clear();
  refresh_template_reference_label();
  update_match_status();
}

void CalibrationWindow::toggle_template_mask_editing() {
  if (mask_editor_.editing()) {
    (void)finish_template_mask_editing();
    return;
  }
  if (template_images_.empty() ||
      template_images_.size() != model_->references().size()) {
    MessageBoxW(dialog_, L"All template references must be available first.",
                L"Mask unavailable", MB_OK | MB_ICONINFORMATION);
    return;
  }
  mask_editor_.begin(template_images_.back());
  SetDlgItemTextW(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK, L"Done");
  preview_.invalidate();
  update_control_enablement();
  update_match_status();
}

bool CalibrationWindow::finish_template_mask_editing() {
  if (!mask_editor_.editing()) return true;
  auto &image = template_images_.back();
  if (!image.valid()) {
    MessageBoxW(dialog_, L"Keep at least 64 opaque pixels in the template mask.",
                L"Mask is too small", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (mask_editor_.changed()) {
    auto stored = layerc::platform::store_template_asset(image);
    const auto *asset =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&stored);
    if (asset == nullptr) {
      show_asset_error(dialog_,
                       std::get<layerc::platform::VisualAssetError>(stored),
                       L"Mask save failed");
      return false;
    }
    if (!model_->replace_last_reference(asset->reference)) {
      MessageBoxW(dialog_,
                  L"This mask duplicates an earlier reference. Change it or clear the duplicate reference.",
                  L"Duplicate template", MB_OK | MB_ICONWARNING);
      return false;
    }
    image = asset->image;
    template_matcher_.clear();
  }
  mask_editor_.finish();
  SetDlgItemTextW(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK, L"Mask...");
  refresh_template_reference_label();
  update_control_enablement();
  preview_.invalidate();
  refresh_preview_frame();
  return true;
}

void CalibrationWindow::update_template_mask(const POINT point,
                                             const bool opaque) {
  if (template_images_.empty())
    return;
  if (mask_editor_.update(template_images_.back(), point, preview_image_rect(),
                          opaque))
    preview_.invalidate();
}

} // namespace layerc::calibration
