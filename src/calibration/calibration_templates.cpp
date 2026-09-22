#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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

} // namespace

void CalibrationWindow::install_template_reference(
    const layerc::core::NormalizedRegion &selected) {
  template_state_.expected_width = selected.width;
  template_state_.expected_height = selected.height;
  template_state_.search_region = expanded_template_search_region(selected);
  rule_.region = template_state_.search_region;
  template_matcher_.clear();
  refresh_template_reference_label();
  update_match_status();
}

void CalibrationWindow::capture_template_reference() {
  const auto &frame = preview_.frame();
  if (!frame) {
    MessageBoxW(dialog_, L"Wait for a captured frame first.", L"LayerC",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (template_state_.references.size() >=
      layerc::core::max_template_references) {
    MessageBoxW(dialog_, L"A template state supports at most four references.",
                L"Template reference limit", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto &region = rule_.region;
  const auto left = static_cast<std::uint32_t>(std::clamp(
      std::floor(region.x * frame->width), 0.0,
      static_cast<double>(frame->width)));
  const auto top = static_cast<std::uint32_t>(std::clamp(
      std::floor(region.y * frame->height), 0.0,
      static_cast<double>(frame->height)));
  const auto right = static_cast<std::uint32_t>(std::clamp(
      std::ceil((region.x + region.width) * frame->width), 0.0,
      static_cast<double>(frame->width)));
  const auto bottom = static_cast<std::uint32_t>(std::clamp(
      std::ceil((region.y + region.height) * frame->height), 0.0,
      static_cast<double>(frame->height)));
  if (right <= left || bottom <= top ||
      static_cast<std::uint64_t>(right - left) * (bottom - top) < 64U) {
    MessageBoxW(dialog_,
                L"Select a region containing at least 64 captured pixels.",
                L"Template region too small", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto source_width = right - left;
  const auto source_height = bottom - top;
  const double factor =
      std::min(1.0, 512.0 / std::max(source_width, source_height));
  layerc::core::TemplateImage image;
  image.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_width * factor)));
  image.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_height * factor)));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  image.alpha.assign(image.pixels.size(), 255);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto source_y =
        top + std::min(source_height - 1,
                       static_cast<std::uint32_t>(
                           (static_cast<std::uint64_t>(y) * 2U + 1U) *
                           source_height /
                           (static_cast<std::uint64_t>(image.height) * 2U)));
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source_x =
          left + std::min(source_width - 1,
                          static_cast<std::uint32_t>(
                              (static_cast<std::uint64_t>(x) * 2U + 1U) *
                              source_width /
                              (static_cast<std::uint64_t>(image.width) * 2U)));
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          frame->pixels[static_cast<std::size_t>(source_y) * frame->width +
                         source_x];
    }
  }
  auto stored = layerc::platform::store_template_asset(image);
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&stored);
  if (asset == nullptr) {
    const auto &error =
        std::get<layerc::platform::VisualAssetError>(stored);
    MessageBoxW(dialog_, layerc::platform::utf8_to_wide(error.message).c_str(),
                L"Template capture failed", MB_OK | MB_ICONERROR);
    return;
  }
  if (std::ranges::none_of(template_state_.references, [&](const auto &item) {
        return item.asset_sha256 == asset->reference.asset_sha256;
      })) {
    template_state_.references.push_back(asset->reference);
    template_images_.push_back(asset->image);
  }
  install_template_reference(rule_.region);
  preview_.invalidate();
}

void CalibrationWindow::import_template_reference() {
  if (template_state_.references.size() >=
      layerc::core::max_template_references) {
    MessageBoxW(dialog_, L"A template state supports at most four references.",
                L"Template reference limit", MB_OK | MB_ICONINFORMATION);
    return;
  }
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
    const auto &error =
        std::get<layerc::platform::VisualAssetError>(imported);
    MessageBoxW(dialog_, layerc::platform::utf8_to_wide(error.message).c_str(),
                L"Template import failed", MB_OK | MB_ICONERROR);
    return;
  }
  if (std::ranges::none_of(template_state_.references, [&](const auto &item) {
        return item.asset_sha256 == asset->reference.asset_sha256;
      })) {
    template_state_.references.push_back(asset->reference);
    template_images_.push_back(asset->image);
  }
  install_template_reference(rule_.region);
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
      const auto &error =
          std::get<layerc::platform::VisualAssetError>(stored);
      MessageBoxW(dialog_,
                  layerc::platform::utf8_to_wide(error.message).c_str(),
                  L"Mask save failed", MB_OK | MB_ICONERROR);
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

void CalibrationWindow::refresh_template_mask_preview() {
  if (template_images_.empty())
    return;
  mask_editor_.refresh(template_images_.back());
  preview_.invalidate();
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
