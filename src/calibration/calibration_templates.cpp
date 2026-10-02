#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>
#include <variant>
#include <vector>

#include <commdlg.h>

namespace layerc::calibration {
namespace {

layerc::core::NormalizedRegion expanded_template_search_region(
    const layerc::core::NormalizedRegion selected) {
  const auto expand = [](const double origin, const double extent) {
    const double expanded = std::min(1.0, extent * 2.0);
    const double center = origin + extent * 0.5;
    return std::pair{std::clamp(center - expanded * 0.5, 0.0,
                                1.0 - expanded),
                     expanded};
  };
  const auto [x, width] = expand(selected.x, selected.width);
  const auto [y, height] = expand(selected.y, selected.height);
  return {x, y, width, height};
}

// Samples the source pixel under the center of each downscaled pixel so the
// stored reference never exceeds max_template_axis pixels on its longer side.
layerc::core::TemplateImage
downscale_capture_region(const layerc::capture::CaptureFrame &frame,
                         const std::uint32_t left, const std::uint32_t top,
                         const std::uint32_t source_width,
                         const std::uint32_t source_height) {
  const auto source_offset = [](const std::uint32_t index,
                                const std::uint32_t extent,
                                const std::uint32_t source_extent) {
    return std::min(source_extent - 1,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(index) * 2U + 1U) *
                        source_extent /
                        (static_cast<std::uint64_t>(extent) * 2U)));
  };
  const double factor =
      std::min(1.0, static_cast<double>(layerc::core::max_template_axis) /
                        std::max(source_width, source_height));
  layerc::core::TemplateImage image;
  image.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_width * factor)));
  image.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_height * factor)));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  image.alpha.assign(image.pixels.size(), 255);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto source_y = top + source_offset(y, image.height, source_height);
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source_x = left + source_offset(x, image.width, source_width);
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          frame.pixels[static_cast<std::size_t>(source_y) * frame.width +
                       source_x];
    }
  }
  return image;
}

[[nodiscard]] bool reject_when_reference_limit_reached(
    const HWND dialog, const layerc::core::TemplateState &state) {
  if (state.references.size() < layerc::core::max_template_references)
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

void CalibrationWindow::set_reference_selection(
    const layerc::core::NormalizedRegion &selected) {
  reference_selection_ = selected;
  rule_.region = selected;
  template_state_.expected_width = selected.width;
  template_state_.expected_height = selected.height;
  template_state_.search_region = expanded_template_search_region(selected);
  template_matcher_.clear();
}

void CalibrationWindow::restore_reference_selection() {
  // Older profiles persist dimensions/search, not the original crop origin.
  // Seed a same-sized central crop without changing persisted geometry.
  reference_selection_ = {
      std::clamp(template_state_.search_region.x +
                     (template_state_.search_region.width -
                      template_state_.expected_width) * 0.5,
                 0.0, 1.0 - template_state_.expected_width),
      std::clamp(template_state_.search_region.y +
                     (template_state_.search_region.height -
                      template_state_.expected_height) * 0.5,
                 0.0, 1.0 - template_state_.expected_height),
      template_state_.expected_width, template_state_.expected_height};
  rule_.region = reference_selection_;
}

std::optional<layerc::core::TemplateImage>
CalibrationWindow::captured_reference_image() const {
  const auto &frame = preview_.frame();
  if (!frame)
    return std::nullopt;
  const auto [left, top, right, bottom] = layerc::core::to_pixel_bounds(
      reference_selection_, frame->width, frame->height);
  if (right <= left || bottom <= top ||
      static_cast<std::uint64_t>(right - left) * (bottom - top) < 64U)
    return std::nullopt;
  return downscale_capture_region(*frame, left, top, right - left, bottom - top);
}

bool CalibrationWindow::append_template_reference(
    const layerc::platform::ImportedTemplateAsset &asset) {
  if (template_state_.references.size() >= layerc::core::max_template_references ||
      std::ranges::any_of(template_state_.references, [&](const auto &item) {
        return item.asset_sha256 == asset.reference.asset_sha256;
      }))
    return false;
  template_state_.references.push_back(asset.reference);
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
  if (reject_when_reference_limit_reached(dialog_, template_state_))
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
  if (reject_when_reference_limit_reached(dialog_, template_state_))
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
  template_state_.references.clear();
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
      template_images_.size() != template_state_.references.size()) {
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
    const bool duplicates_earlier = std::ranges::any_of(
        template_state_.references.begin(),
        template_state_.references.end() - 1, [&](const auto &reference) {
          return reference.asset_sha256 == asset->reference.asset_sha256;
        });
    if (duplicates_earlier) {
      MessageBoxW(dialog_,
                  L"This mask duplicates an earlier reference. Change it or clear the duplicate reference.",
                  L"Duplicate template", MB_OK | MB_ICONWARNING);
      return false;
    }
    template_state_.references.back() = asset->reference;
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
