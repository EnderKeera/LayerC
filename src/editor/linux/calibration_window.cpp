#include "calibration_window.hpp"

#include "profile_mutations.hpp"
#include "x11_capture.hpp"
#include "x11_window.hpp"
#include "layerc/core/calibration_readiness.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/platform/template_png.hpp"
#include "layerc/platform/visual_assets_linux.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <chrono>
#include <span>
#include <utility>

namespace layerc::editor {
namespace {
namespace core = layerc::core;
namespace platform = layerc::platform;

class CalibrationDialog final {
public:
  CalibrationDialog(std::filesystem::path path, CalibrationModel seed, const CalibrationAccept &accept,
                    std::span<const platform::EncodedTemplateAsset> owned)
      : path_(std::move(path)), model_(std::move(seed)), controls_(model_.initial_controls()),
        accept_(accept) {
    window_.begin();
    auto *scroll = new Fl_Scroll(0, 0, 1060, 750);
    scroll->begin();
    // Keep outside labels and form margins inside the scrollable bounds.
    auto *canvas = new Fl_Group(0, 0, 1060, 750);
    canvas->begin();
    target_ = choice(20, 45, 405, "Calibration target window", nullptr);
    rescan_ = button(435, 45, 135, "Rescan windows");
    notice_ = check(20, 80, 550, "I acknowledge passive capture of the selected game window");
    live_ = check(20, 128, 220, "Live preview");
    tool_ = choice(270, 128, 300, "Preview tool", "Select region|Erase template mask|Restore template mask");
    preview_ = new CalibrationPreview(20, 163, 550, 220);
    preview_->selected = [this](const auto region) {
      model_.set_selection(region); fill_region(); evaluate();
    };
    preview_->painted = [this](const double x, const double y, const bool opaque) {
      if (!images_.empty() && images_.size() == model_.references().size() &&
          paint_template_mask(images_.back(), x, y, opaque)) {
        mask_dirty_ = true; matcher_.clear();
      }
    };
    region_x_ = input(20, 410, 265, "Region X (0–1)");
    region_y_ = input(305, 410, 265, "Region Y (0–1)");
    region_w_ = input(20, 465, 265, "Region width");
    region_h_ = input(305, 465, 265, "Region height");
    sample_ = button(20, 510, 265, "Sample selection color");
    capture_ = button(305, 510, 265, "Capture reference");
    import_ = button(20, 550, 265, "Import PNG reference...");
    clear_ = button(305, 550, 265, "Clear references");
    references_ = new Fl_Output(20, 605, 550, 28, "Template references");
    references_->align(FL_ALIGN_TOP_LEFT);
    outcome_ = new Fl_Multiline_Output(20, 665, 550, 65, "Capture / live match status");
    outcome_->wrap(1);
    outcome_->align(FL_ALIGN_TOP_LEFT); outcome_->textsize(11);

    mode_ = choice(610, 45, 420, "Calibration predicate", "Average color|Brightness|Color coverage|Template match");
    color_ = input(610, 105, 200, "Target color #RRGGBB");
    tolerance_ = input(830, 105, 200, "Color tolerance (0–255)");
    threshold_ = input(610, 165, 200, "Brightness threshold");
    comparison_ = choice(830, 165, 200, "Brightness comparison", "Above|Below");
    coverage_ = input(610, 225, 200, "Minimum coverage (%)");
    confidence_ = input(830, 225, 200, "Template confidence (%)");
    scale_ = input(610, 285, 200, "Scale tolerance (0–20%)");
    stable_ = input(830, 285, 200, "Stable samples (1–15)");
    repeat_ = choice(610, 345, 200, "Repeat policy", "Edge|When fully ready");
    rate_ = input(830, 345, 200, "Sample rate (Hz)");
    quality_ = choice(610, 405, 200, "Capture quality", "Low|Balanced|Exact");
    trigger_ = choice(830, 405, 200, "Trigger mode", "Pixel only|Confirm input");
    keep_ = check(610, 450, 420, "Keep input alongside the visual trigger");
    timeout_ = input(610, 510, 200, "Input timeout (50–5000 ms)");
    unavailable_ = choice(830, 510, 200, "Capture unavailable", "Ignore|Trigger from input");
    auto *note = new Fl_Multiline_Output(610, 560, 420, 80, "Calibration semantics");
    note->wrap(1);
    note->align(FL_ALIGN_TOP_LEFT); note->textsize(11);
    note->value("Passive X11 capture; no input is blocked or synthesized.\n"
                "Confirm input requires an existing cooldown binding.\n"
                "Cancel discards all calibration edits and captured/imported assets.");
    apply_ = button(730, 700, 150, "Apply calibration");
    apply_->shortcut(FL_CTRL | FL_Enter);
    cancel_ = button(890, 700, 140, "Cancel calibration");
    cancel_->shortcut(FL_Escape);
    canvas->end();
    scroll->end(); window_.end(); window_.resizable(scroll);
    window_.size_range(640, 480); window_.set_modal();
    window_.callback([](Fl_Widget *, void *data) {
      static_cast<CalibrationDialog *>(data)->close();
    }, this);
    fill_controls();
    for (const auto &reference : model_.references()) {
      auto loaded = platform::load_profile_template_asset(path_, reference.asset_sha256, owned);
      if (const auto *error = std::get_if<platform::VisualAssetError>(&loaded))
        load_fault_ += "Unavailable template: " + error->message + "\n";
      else images_.push_back(std::get<core::TemplateImage>(std::move(loaded)));
    }
    refresh_references();
    refresh_apply_readiness();
    message(load_fault_.empty() ? "Stopped. No capture session. Rescan windows to begin." : load_fault_);
  }
  ~CalibrationDialog() { stop_capture(); }
  [[nodiscard]] std::optional<AcceptedCalibration> run() {
    window_.show();
    while (window_.shown()) Fl::wait();
    return std::move(result_);
  }
private:
  Fl_Double_Window window_{1060, 750, "LayerE — X11 visual calibration"};
  std::filesystem::path path_;
  CalibrationModel model_;
  CalibrationControls controls_;
  const CalibrationAccept &accept_;
  std::optional<AcceptedCalibration> result_;
  // Reverse destruction releases capture and discovery before their display.
  std::optional<platform::X11Display> display_;
  std::unique_ptr<platform::X11WindowTracker> tracker_;
  std::unique_ptr<platform::X11CaptureSession> capture_session_;
  std::vector<platform::X11WindowSnapshot> windows_;
  std::vector<core::TemplateImage> images_;
  core::CpuTemplateMatcher matcher_;
  core::CalibrationReadiness readiness_;
  std::string load_fault_;
  bool mask_dirty_{}, region_dirty_{};
  CalibrationPreview *preview_{};
  Fl_Choice *target_{}, *tool_{}, *mode_{}, *comparison_{}, *repeat_{}, *quality_{}, *trigger_{}, *unavailable_{};
  Fl_Input *region_x_{}, *region_y_{}, *region_w_{}, *region_h_{}, *color_{}, *tolerance_{}, *threshold_{},
      *coverage_{}, *confidence_{}, *scale_{}, *stable_{}, *rate_{}, *timeout_{};
  Fl_Check_Button *notice_{}, *live_{}, *keep_{};
  Fl_Button *rescan_{}, *sample_{}, *capture_{}, *import_{}, *clear_{}, *apply_{}, *cancel_{};
  Fl_Output *references_{};
  Fl_Multiline_Output *outcome_{};

  void bind(Fl_Widget &widget) {
    widget.callback([](Fl_Widget *w, void *data) {
      static_cast<CalibrationDialog *>(data)->handle(w);
    }, this);
  }
  Fl_Button *button(int x, int y, int width, const char *label) {
    auto *w = new Fl_Button(x, y, width, 30, label); bind(*w); return w;
  }
  Fl_Input *input(int x, int y, int width, const char *label) {
    auto *w = new Fl_Input(x, y, width, 28, label);
    w->align(FL_ALIGN_TOP_LEFT); w->when(FL_WHEN_CHANGED); bind(*w); return w;
  }
  Fl_Choice *choice(int x, int y, int width, const char *label, const char *items) {
    auto *w = new Fl_Choice(x, y, width, 28, label);
    w->align(FL_ALIGN_TOP_LEFT); if (items) w->add(items);
    w->value(0); bind(*w); return w;
  }
  Fl_Check_Button *check(int x, int y, int width, const char *label) {
    auto *w = new Fl_Check_Button(x, y, width, 28, label); bind(*w); return w;
  }
  void message(const std::string &text) { outcome_->value(text.c_str()); }
  void fill_region() {
    const auto &r = model_.selection(); preview_->selection = r;
    region_x_->value(format_decimal(r.x).c_str()); region_y_->value(format_decimal(r.y).c_str());
    region_w_->value(format_decimal(r.width).c_str()); region_h_->value(format_decimal(r.height).c_str());
    preview_->redraw();
  }
  void fill_controls() {
    fill_region();
    mode_->value(static_cast<int>(controls_.predicate));
    color_->value(format_rgb(model_.target_color()).c_str());
    tolerance_->value(std::to_string(controls_.tolerance).c_str());
    threshold_->value(std::to_string(controls_.threshold).c_str());
    comparison_->value(static_cast<int>(controls_.comparison));
    coverage_->value(std::to_string(controls_.minimum_percent).c_str());
    confidence_->value(std::to_string(controls_.confidence_percent).c_str());
    scale_->value(std::to_string(controls_.scale_percent).c_str());
    stable_->value(std::to_string(controls_.stable_samples).c_str());
    repeat_->value(static_cast<int>(controls_.repeat)); rate_->value(std::to_string(controls_.sample_rate_hz).c_str());
    quality_->value(static_cast<int>(controls_.quality)); trigger_->value(static_cast<int>(controls_.trigger_mode));
    keep_->value(controls_.keep_input); timeout_->value(std::to_string(controls_.timeout_ms).c_str());
    unavailable_->value(static_cast<int>(controls_.unavailable));
  }
  bool read_controls() {
    const auto x = parse_anchor(region_x_->value()), y = parse_anchor(region_y_->value()),
        w = parse_anchor(region_w_->value()), h = parse_anchor(region_h_->value());
    const auto color = parse_rgb(color_->value());
    const auto tolerance = parse_count(tolerance_->value(), 0, 255), threshold = parse_count(threshold_->value(), 0, 255),
        coverage = parse_count(coverage_->value(), 0, 100), confidence = parse_count(confidence_->value(), 0, 100),
        scale = parse_count(scale_->value(), 0, 20), stable = parse_count(stable_->value(), 1, 15),
        rate = parse_count(rate_->value(), 1, 60), timeout = parse_count(timeout_->value(), 50, 5000);
    if (!x || !y || !w || !h || !core::valid_normalized_region({*x, *y, *w, *h}) || !color ||
        !tolerance || !threshold || !coverage || !confidence || !scale || !stable || !rate || !timeout) {
      readiness_.invalidate();
      refresh_apply_readiness();
      message("Invalid calibration input. All values and the nonempty region must be in range."); return false;
    }
    if (region_dirty_) { model_.set_selection({*x, *y, *w, *h}); region_dirty_ = false; }
    preview_->selection = model_.selection();
    model_.set_target_color(*color);
    controls_.predicate = static_cast<CalibrationPredicate>(mode_->value());
    controls_.tolerance = static_cast<int>(*tolerance); controls_.threshold = static_cast<int>(*threshold);
    controls_.comparison = static_cast<core::BrightnessComparison>(comparison_->value());
    controls_.minimum_percent = static_cast<int>(*coverage); controls_.confidence_percent = static_cast<int>(*confidence);
    controls_.scale_percent = static_cast<int>(*scale); controls_.stable_samples = static_cast<int>(*stable);
    controls_.repeat = static_cast<core::PixelRepeatPolicy>(repeat_->value());
    controls_.sample_rate_hz = static_cast<std::uint8_t>(*rate);
    controls_.quality = static_cast<core::CaptureQuality>(quality_->value());
    controls_.trigger_mode = static_cast<core::VisualTriggerMode>(trigger_->value());
    controls_.keep_input = keep_->value() != 0; controls_.timeout_ms = static_cast<int>(*timeout);
    controls_.unavailable = static_cast<core::CaptureUnavailablePolicy>(unavailable_->value());
    return true;
  }
  bool open_display() {
    if (display_) return true;
    std::string error; display_ = platform::X11Display::open(error);
    if (!display_) { message("Unavailable: " + error); return false; }
    tracker_ = std::make_unique<platform::X11WindowTracker>(*display_); return true;
  }
  void rescan() {
    stop_capture(); live_->value(0);
    preview_->frame.reset(); preview_->redraw();
    readiness_.invalidate(); refresh_apply_readiness();
    target_->clear(); windows_.clear();
    if (!open_display()) return;
    const auto scan = tracker_->scan();
    for (const auto &window : scan.windows) {
      if (window.hidden) continue;
      platform::X11WindowScan one; one.windows.push_back(window);
      if (!platform::match_window(one, model_.profile().game)) continue;
      windows_.push_back(window);
      const auto index = target_->add("item");
      target_->replace(index, (window.title + " — " + window.class_name).c_str());
    }
    target_->value(windows_.empty() ? -1 : 0);
    message(windows_.empty() ? "Unavailable: no visible X11 window matches this profile." :
            "Stopped. Select a matching window and acknowledge capture. " + scan.warning);
  }
  void stop_capture() {
    Fl::remove_timeout(tick, this);
    capture_session_.reset(); tracker_.reset(); display_.reset();
  }
  void close() { stop_capture(); live_->value(0); window_.hide(); }
  void start_capture() {
    stop_capture(); preview_->frame.reset(); preview_->redraw();
    readiness_.invalidate(); refresh_apply_readiness();
    const auto selected = target_->value();
    if (!notice_->value() || selected < 0 || static_cast<std::size_t>(selected) >= windows_.size()) {
      live_->value(0); message("Unavailable: select a matching window and acknowledge passive capture first."); return;
    }
    if (!read_controls() || !open_display()) { live_->value(0); return; }
    const auto &expected = windows_[static_cast<std::size_t>(selected)];
    const auto scan = tracker_->scan();
    const auto current = std::ranges::find(scan.windows, expected.window,
                                          &platform::X11WindowSnapshot::window);
    const bool same_identity = current != scan.windows.end() && !current->hidden &&
        current->title == expected.title && current->class_name == expected.class_name &&
        current->instance_name == expected.instance_name && current->pid == expected.pid &&
        current->executable_name == expected.executable_name &&
        current->executable_path == expected.executable_path;
    platform::X11WindowScan candidate;
    if (same_identity) candidate.windows.push_back(*current);
    if (!same_identity || !platform::match_window(candidate, model_.profile().game)) {
      stop_capture(); live_->value(0); notice_->value(0);
      target_->clear(); windows_.clear();
      message("Unavailable: the selected window disappeared or changed identity. Rescan and acknowledge capture again.");
      return;
    }
    capture_session_ = std::make_unique<platform::X11CaptureSession>(
        *display_, core::WindowId{current->window}, controls_.quality);
    (void)capture_session_->start(); tick(this);
  }
  static void tick(void *data) {
    auto &self = *static_cast<CalibrationDialog *>(data);
    if (!self.capture_session_ || !self.window_.shown()) return;
    // Drain the private connection every tick. Damage remains accumulated on
    // the server and is queried by capture.poll(), even after events are drained.
    (void)self.tracker_->poll_changes();
    const auto selected = self.target_->value();
    const auto current = selected >= 0 && static_cast<std::size_t>(selected) < self.windows_.size()
        ? self.tracker_->find(self.windows_[static_cast<std::size_t>(selected)].window)
        : std::nullopt;
    const auto same_identity = [&] {
      if (!current || current->hidden) return false;
      const auto &expected = self.windows_[static_cast<std::size_t>(selected)];
      return current->title == expected.title && current->class_name == expected.class_name &&
             current->instance_name == expected.instance_name && current->pid == expected.pid &&
             current->executable_name == expected.executable_name &&
             current->executable_path == expected.executable_path;
    };
    if (!same_identity()) {
      self.preview_->frame.reset(); self.preview_->redraw(); self.stop_capture();
      self.readiness_.invalidate(); self.refresh_apply_readiness();
      self.live_->value(0); self.notice_->value(0); self.target_->clear(); self.windows_.clear();
      self.message("Unavailable: the selected window disappeared or changed identity. Rescan and acknowledge capture again.");
      return;
    }
    const auto poll = self.capture_session_->poll(std::chrono::steady_clock::now());
    if (!poll.result.frame || poll.result.availability != core::CaptureAvailability::available) {
      self.preview_->frame.reset(); self.preview_->redraw();
      self.readiness_.invalidate(); self.refresh_apply_readiness();
      self.message("Unavailable: " + poll.result.diagnostic);
      self.stop_capture(); self.live_->value(0); return;
    }
    if (self.preview_->frame != poll.result.frame) {
      self.preview_->frame = poll.result.frame; self.preview_->redraw(); self.evaluate(poll.result.diagnostic);
    }
    Fl::repeat_timeout(1.0 / self.controls_.sample_rate_hz, tick, &self);
  }
  core::VisualObservation current_observation() const {
    if (controls_.predicate == CalibrationPredicate::template_match)
      return model_.template_state(controls_);
    return model_.pixel_rule(controls_);
  }
  void refresh_apply_readiness() {
    if (readiness_.ready_for(current_observation(), controls_.quality))
      apply_->activate();
    else
      apply_->deactivate();
  }
  void evaluate(const std::string &diagnostic = {}) {
    const auto &frame = preview_->frame;
    if (!frame) {
      readiness_.invalidate();
      refresh_apply_readiness();
      message("Stopped. No available captured frame."); return;
    }
    if (controls_.predicate == CalibrationPredicate::template_match) {
      if (images_.size() != model_.references().size() || images_.empty()) {
        readiness_.invalidate();
        refresh_apply_readiness();
        message("Unavailable: capture or import every template reference first."); return;
      }
      const auto state = model_.template_state(controls_);
      const auto result = matcher_.evaluate(state, images_, frame->view(),
          controls_.quality, frame->available_region, frame->unavailable_regions);
      readiness_.observe(state, result, controls_.quality);
      refresh_apply_readiness();
      message(std::string(result.matched ? "Match" : "No match") + ": template confidence " +
          std::to_string(static_cast<int>(result.confidence * 100)) + "%; candidates " +
          std::to_string(result.candidates_scored) + ". " + diagnostic);
    } else {
      const auto rule = model_.pixel_rule(controls_);
      if (!layerc::capture::region_is_available(rule.region, *frame)) {
        readiness_.invalidate();
        refresh_apply_readiness();
        message("Unavailable: selected region lies outside available capture."); return;
      }
      const auto result = core::evaluate_pixel_rule(rule, frame->view(), controls_.quality);
      readiness_.observe(rule, result, controls_.quality, true);
      refresh_apply_readiness();
      message(std::string(result.matched ? "Match" : "No match") + ": average " + format_rgb(result.average_color) +
          "; pixels " + std::to_string(result.sampled_pixels) + ". " + diagnostic);
    }
  }
  void refresh_references() {
    references_->value((std::to_string(model_.references().size()) + "/" +
        std::to_string(core::max_template_references) + " references; " +
        std::to_string(images_.size()) + " loaded").c_str());
    preview_->tool = tool_->value();
    preview_->mask = preview_->tool && !images_.empty() && images_.size() == model_.references().size()
        ? &images_.back() : nullptr;
    preview_->redraw();
  }
  bool stage_image(core::TemplateImage image, bool replace = false) {
    if (!core::template_image_has_structure(image)) {
      message("Reference refused: at least 64 visible pixels with light/color variation are required."); return false;
    }
    auto encoded = platform::encode_template_png(image);
    if (const auto *error = std::get_if<platform::VisualAssetError>(&encoded)) { message(error->message); return false; }
    image.asset_sha256 = core::sha256(std::as_bytes(std::span(std::get<std::vector<std::uint8_t>>(encoded))));
    const core::TemplateReference reference{image.asset_sha256};
    if (replace ? !model_.replace_last_reference(reference) : !model_.add_reference(reference)) {
      message("Reference refused: duplicate reference or maximum reference count reached."); return false;
    }
    if (replace) images_.back() = std::move(image); else images_.push_back(std::move(image));
    mask_dirty_ = false; matcher_.clear(); refresh_references(); return true;
  }
  bool finalize_mask() { return !mask_dirty_ || stage_image(images_.back(), true); }
  void handle(Fl_Widget *widget) {
    if (widget == region_x_ || widget == region_y_ || widget == region_w_ || widget == region_h_)
      region_dirty_ = true;
    if ((widget == capture_ || widget == import_) && images_.size() != model_.references().size()) {
      message("Unavailable template references must be repaired or explicitly cleared before adding new ones."); return;
    }
    if (widget == cancel_) { close(); return; }
    if (widget == rescan_) { rescan(); return; }
    if (widget == notice_ && !notice_->value()) {
      stop_capture(); live_->value(0); message("Stopped. Capture acknowledgement withdrawn."); return;
    }
    if (widget == live_ || widget == target_) {
      if (widget == target_) {
        preview_->frame.reset(); preview_->redraw();
        readiness_.invalidate(); refresh_apply_readiness();
      }
      if (live_->value()) start_capture();
      else { stop_capture(); message("Stopped. Capture session released; preview is frozen."); }
      return;
    }
    if (widget == clear_) {
      preview_->mask = nullptr; model_.clear_references(); images_.clear(); load_fault_.clear(); mask_dirty_ = false;
      matcher_.clear(); refresh_references(); message("References cleared in the calibration draft only."); return;
    }
    if (widget == tool_) {
      // Erase can temporarily invalidate a mask. Always permit Restore;
      // validation belongs to Apply or adding another reference, not tool changes.
      refresh_references(); return;
    }
    if (widget == quality_ && quality_->value() == static_cast<int>(core::CaptureQuality::exact) &&
        controls_.quality != core::CaptureQuality::exact) {
      fl_message_title("Exact capture quality");
      if (fl_choice("Exact capture quality uses more memory and CPU. Continue?", "Cancel",
                    "Use Exact", nullptr) != 1) {
        quality_->value(static_cast<int>(controls_.quality)); return;
      }
    }
    if (!read_controls()) return;
    if (capture_session_) capture_session_->set_quality(controls_.quality);
    if (widget == sample_) {
      if (!preview_->frame || !layerc::capture::region_is_available(model_.selection(), *preview_->frame)) {
        message("Unavailable: start capture and select an available region before sampling color."); return;
      }
      const auto color = model_.sample_color(preview_->frame->view(), controls_.quality);
      if (!color) { message("Unavailable: the selection contains no captured pixels."); return; }
      model_.set_target_color(*color); color_->value(format_rgb(*color).c_str());
    } else if (widget == capture_) {
      if (!finalize_mask()) return;
      if (!preview_->frame || !layerc::capture::region_is_available(model_.selection(), *preview_->frame)) {
        message("Unavailable: select a nonempty region in an available captured frame."); return;
      }
      auto image = template_from_selection(preview_->frame->view(), model_.selection());
      if (!image) { message("Reference refused: select at least 64 captured pixels."); return; }
      if (!stage_image(std::move(*image))) return;
      controls_.predicate = CalibrationPredicate::template_match; mode_->value(static_cast<int>(controls_.predicate));
    } else if (widget == import_) {
      if (!finalize_mask()) return;
      Fl_Native_File_Chooser chooser; chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
      chooser.title("Import a PNG template reference"); chooser.filter("PNG image\t*.png");
      if (chooser.show() != 0 || !chooser.filename()) return;
      const auto bytes = platform::read_template_asset_bytes(chooser.filename());
      if (!bytes) { message("Reference refused: PNG must be a readable plain file of at most 4 MiB."); return; }
      auto decoded = platform::decode_template_png(*bytes);
      if (const auto *error = std::get_if<platform::VisualAssetError>(&decoded)) { message(error->message); return; }
      if (!stage_image(std::get<core::TemplateImage>(std::move(decoded)))) return;
      controls_.predicate = CalibrationPredicate::template_match; mode_->value(static_cast<int>(controls_.predicate));
    } else if (widget == apply_) {
      if (!finalize_mask()) return;
      // Evaluate only the already captured preview: Apply never starts capture.
      evaluate();
      if (!readiness_.ready_for(current_observation(), controls_.quality)) {
        message(core::calibration_sampling_required);
        refresh_apply_readiness();
        return;
      }
      auto built = model_.build_result(controls_, images_);
      if (const auto *error = std::get_if<std::string>(&built)) { message("Not applied: " + *error); return; }
      CalibrationDraft draft{
          std::get<core::VisualProfile>(std::move(built)),
          controls_.predicate == CalibrationPredicate::template_match
              ? images_ : std::vector<core::TemplateImage>{}};
      auto accepted = accept_(draft);
      if (const auto *refusal = std::get_if<std::string>(&accepted)) {
        message("Not applied: " + *refusal);
        return;
      }
      result_ = std::get<AcceptedCalibration>(std::move(accepted));
      close();
      return;
    }
    preview_->redraw(); evaluate();
  }
};
} // namespace

std::optional<AcceptedCalibration> run_x11_calibration(
    const std::filesystem::path &profile_path, CalibrationModel model,
    const CalibrationAccept &accept, const std::span<const platform::EncodedTemplateAsset> owned) {
  // The modal owns all preview timers and X11 resources. No worker, service,
  // polling timer or captured image survives its lifetime.
  CalibrationDialog dialog(profile_path, std::move(model), accept, owned);
  return dialog.run();
}
} // namespace layerc::editor
