#include "editor_file_chooser.hpp"

#include "layerc/platform/text.hpp"
#include "platform/editor_options.hpp"
#include "platform/services.hpp"

#include <FL/Fl_Native_File_Chooser.H>

#include <utility>

#ifdef _WIN32
#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "editor_app.hpp"
#include "editor_style.hpp"
#include "platform/window_chrome.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <string_view>
#include <variant>
#endif

namespace layerc::editor {
namespace {

NativeFilePresenter native_presenter;
FilePromptObserver prompt_observer;

std::string *remembered_directory(const EditorFileRequest::DirectoryMemory memory) {
  static std::string profiles, traces;
  switch (memory) {
  case EditorFileRequest::DirectoryMemory::profiles: return &profiles;
  case EditorFileRequest::DirectoryMemory::traces: return &traces;
  default: return nullptr;
  }
}

std::optional<std::filesystem::path> present_native_file(const EditorFileRequest &request,
                                                         const std::string &directory) {
  const bool save = request.mode == EditorFileRequest::Mode::save;
  Fl_Native_File_Chooser chooser(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                                     : Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(request.title.c_str());
  if (!directory.empty()) chooser.directory(directory.c_str());
  if (request.extensions.size() != 0) {
    auto filter = request.filter_name + "\t*.";
    const bool multiple = request.extensions.size() > 1;
    if (multiple) filter += '{';
    for (auto it = request.extensions.begin(); it != request.extensions.end(); ++it) {
      if (it != request.extensions.begin()) filter += ',';
      filter += *it;
    }
    if (multiple) filter += '}';
    chooser.filter(filter.c_str());
  }
  if (save) chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (chooser.show() != 0 || !chooser.filename() || !*chooser.filename()) return std::nullopt;
  return platform::path_from_utf8(chooser.filename());
}

#ifdef _WIN32
constexpr auto capture_notice =
    "Screenshots, screen sharing and Magnifier can see the Windows file dialog, including its folders "
    "and file names; the editor cannot hide it. Type the file's full path here, or open the file dialog anyway.";
constexpr auto capture_refusal =
    "Could not hide this window from screen capture, so typing a path is off. Open the file dialog anyway, or cancel.";
constexpr auto replace_question = "That file already exists. Replace it?";
constexpr auto drive_error =
    "Could not use that path: typed paths start with a drive letter, like C:\\. Use Open file dialog... for other locations.";
constexpr auto name_error =
    "Could not use that path: Windows does not allow that file name. Type a full path with a valid file name.";
constexpr auto extension_error =
    "Could not use that path: the file extension is not supported. Type a file name for this file type.";
constexpr auto file_error =
    "Could not use that path: no file is there. Type the full path to an existing file.";
constexpr auto parent_error =
    "Could not use that path: the parent folder does not exist or is unavailable. Type a path in an existing folder.";
constexpr auto target_error =
    "Could not use that path: the destination is not an available file. Type a path to a file, not a folder.";
constexpr std::array reason_texts{capture_refusal, replace_question, drive_error, name_error,
                                  extension_error, file_error, parent_error, target_error};

std::string_view trim_blanks(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == text.npos) return {};
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

char ascii_lower(const char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool ascii_matches(const std::string_view left, const std::string_view right) {
  return std::ranges::equal(left, right, {}, ascii_lower, ascii_lower);
}

bool reserved_device(const std::string_view component) {
  const auto name = trim_blanks(component.substr(0, component.find('.')));
  constexpr std::array devices{"CON", "PRN", "AUX", "NUL"};
  if (std::ranges::any_of(devices, [&](const auto *device) { return ascii_matches(name, device); }))
    return true;
  return name.size() == 4 && name[3] >= '1' && name[3] <= '9' &&
         (ascii_matches(name.substr(0, 3), "COM") || ascii_matches(name.substr(0, 3), "LPT"));
}

struct TypedPath {
  std::filesystem::path path;
  bool replace{};
};

std::variant<TypedPath, const char *> typed_path(const EditorFileRequest &request,
                                                std::string_view input) {
  input = trim_blanks(input);
  if (input.size() >= 2 && input.front() == '"' && input.back() == '"') {
    input.remove_prefix(1);
    input.remove_suffix(1);
  }
  if (input.size() < 3 || ascii_lower(input[0]) < 'a' || ascii_lower(input[0]) > 'z' ||
      input[1] != ':' || (input[2] != '\\' && input[2] != '/'))
    return drive_error;

  auto rest = input.substr(3);
  while (!rest.empty()) {
    const auto next = rest.find_first_of("\\/");
    const auto component = rest.substr(0, next);
    if (!component.empty() &&
        (component.back() == '.' || component.back() == ' ' || reserved_device(component) ||
         component.find_first_of(":*?\"<>|") != component.npos ||
         std::ranges::any_of(component, [](const char c) { return static_cast<unsigned char>(c) < 32; })))
      return name_error;
    if (next == rest.npos) break;
    rest.remove_prefix(next + 1);
  }
  auto path = platform::path_from_utf8(input);
  if (!path) return name_error;

  const bool save = request.mode == EditorFileRequest::Mode::save;
  std::error_code ec;
  if (path->has_filename() && request.extensions.size() != 0) {
    const auto extension = platform::path_to_utf8(path->extension());
    if (!extension) return name_error;
    if (extension->empty()) {
      // A directory is never an extensionless file name to complete.
      if (std::filesystem::is_directory(*path, ec)) return save ? target_error : file_error;
      *path += ".";
      *path += *request.extensions.begin();
    } else if (!std::ranges::any_of(request.extensions, [&](const auto &allowed) {
                 return ascii_matches(std::string_view(*extension).substr(1), allowed);
               })) {
      return extension_error;
    }
  }

  if (!save) {
    if (!std::filesystem::is_regular_file(*path, ec)) return file_error;
    return TypedPath{std::move(*path)};
  }
  if (!path->has_filename() || !std::filesystem::is_directory(path->parent_path(), ec))
    return !path->has_filename() ? target_error : parent_error;
  const auto status = std::filesystem::status(*path, ec);
  if (status.type() == std::filesystem::file_type::not_found)
    return TypedPath{std::move(*path)};
  if (ec || !std::filesystem::is_regular_file(status)) return target_error;
  return TypedPath{std::move(*path), true};
}

constexpr int margin = 12, gap = 8, min_button_width = 88, preferred_width = 560;
constexpr int mark_size = 12;

int measured_width(const char *text) {
  int width = 0, height = 0;
  const auto saved = fl_draw_shortcut;
  fl_draw_shortcut = 2;
  fl_measure(text, width, height, 0);
  fl_draw_shortcut = saved;
  return width;
}

int wrapped_height(const char *text, int width) {
  int height = 0;
  fl_measure(text, width, height, 0);
  return height;
}

std::string action_label(const std::string &label) { return '&' + label; }

class FilePathDialog final : public Fl_Window {
public:
  FilePathDialog(const EditorFileRequest &request, EditorAccessibility *accessibility)
      : Fl_Window(1, 1), request_(request), accessibility_(accessibility),
        action_label_(action_label(request.action)) {
    copy_label(request.title.c_str());
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    const auto button_width = [](const char *text) {
      return std::max(min_button_width, measured_width(text) + 2 * margin);
    };
    constexpr auto open_dialog = "Open file &dialog...";
    const int primary_width = std::max(button_width(action_label_.c_str()), button_width("&Replace"));
    const int alternative_width = button_width(open_dialog);
    const int cancel_width = button_width("&Cancel");
    const int actions_width = primary_width + alternative_width + cancel_width + 2 * gap;

    auto &owner = *request.app.main_window();
    int work_x{}, work_y{}, work_width{}, work_height{};
    Fl::screen_work_area(work_x, work_y, work_width, work_height, owner.screen_num());
    const int minimum_width = actions_width + 2 * margin;
    const int width = std::clamp(preferred_width, minimum_width, std::max(minimum_width, work_width));
    const int text_x = margin + mark_size + gap;
    const int text_width = width - margin - text_x;

    begin();
    int y = margin;
    auto *notice = new EditorStatusMark(margin, y, mark_size, fl_height());
    notice->kind(StatusKind::warning);
    message_ = line(text_x, y, text_width, wrapped_height(capture_notice, text_width), capture_notice);
    y += message_->h() + kit::metric(kit::Metric::gap_dialog);
    constexpr auto label = "&File path";
    const int input_x = margin + measured_width(label) + gap;
    input_ = new EditorField<Fl_Input>(input_x, y, width - margin - input_x, kit::metric(kit::Metric::control_height), label);
    input_->shortcut_label(1);
    accessibility::describe_with(*input_, *message_);
    y += kit::metric(kit::Metric::control_height) + gap;

    int reason_height = 0;
    for (const auto *text : reason_texts)
      reason_height = std::max(reason_height, wrapped_height(text, text_width));
    mark_ = new EditorStatusMark(margin, y, mark_size, fl_height());
    reason_ = line(text_x, y, text_width, reason_height, nullptr);
    y += reason_height + kit::metric(kit::Metric::gap_dialog);

    int x = width - margin - actions_width;
    primary_ = new Fl_Button(x, y, primary_width, kit::metric(kit::Metric::control_height), action_label_.c_str());
    primary_button(*primary_);
    primary_->callback([](Fl_Widget *, void *self) {
      static_cast<FilePathDialog *>(self)->submit();
    }, this);
    x += primary_width + gap;
    alternative_ = new Fl_Button(x, y, alternative_width, kit::metric(kit::Metric::control_height), open_dialog);
    alternative_->shortcut(FL_ALT | 'd');
    alternative_->callback([](Fl_Widget *, void *self) {
      static_cast<FilePathDialog *>(self)->alternate();
    }, this);
    x += alternative_width + gap;
    cancel_ = new Fl_Button(x, y, cancel_width, kit::metric(kit::Metric::control_height), "&Cancel");
    cancel_->callback([](Fl_Widget *, void *self) { static_cast<FilePathDialog *>(self)->hide(); }, this);
    input_->deactivate();
    primary_->deactivate();
    end();
    size(width, y + kit::metric(kit::Metric::control_height) + margin);
    const auto fit = [](int position, int start, int extent, int length) {
      return std::clamp(position, start, std::max(start, start + extent - length));
    };
    position(fit(owner.x() + (owner.w() - w()) / 2, work_x, work_width, w()),
             fit(owner.y() + (owner.h() - h()) / 2, work_y, work_height, h()));
    set_modal();
  }

  void run() {
    auto &owner = *request_.app.main_window();
    if (owner.shown()) Fl::first_window(&owner);
    apply_editor_appearance(request_.app.appearance(), *this, nullptr, nullptr);
    show();
    // Nothing private is in the prompt until the user types. Verify capture
    // exclusion synchronously after show(), before control returns to
    // Fl::wait and before any key or drop can reach the input.
    if (verify_editor_window_capture(*this)) {
      input_->activate();
      primary_->activate();
      input_->take_focus();
    } else {
      reason(capture_refusal, StatusKind::warning);
      accessibility::describe_with(*input_, *reason_);
      accessibility::describe_with_tooltip(*primary_, capture_refusal);
      cancel_->take_focus();
    }
    observe(FilePromptStage::shown_and_verified);
    while (shown()) Fl::wait();
  }

  std::optional<std::filesystem::path> path;
  bool consent{};

private:
  static Fl_Box *line(int x, int y, int width, int height, const char *label) {
    auto *result = new Fl_Box(x, y, width, height, label);
    result->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    return result;
  }

  int handle(const int event) override {
    if (event == FL_SHORTCUT) {
      if (Fl::event_key() == FL_Escape) { hide(); return 1; }
      if (input_->active() && input_->test_shortcut()) return input_->take_focus();
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && primary_->active()) {
        submit();
        return 1;
      }
    }
    return Fl_Window::handle(event);
  }

  void draw() override {
    ++draw_count_;
    Fl_Window::draw();
  }

  void observe(const FilePromptStage stage) {
    if (prompt_observer)
      prompt_observer({*this, *input_, *primary_, *alternative_, *cancel_,
                       *message_, *reason_, draw_count_}, stage);
  }

  void reason(const char *text, const StatusKind kind) {
    reason_->copy_label(text);
    reason_->redraw();
    mark_->kind(kind);
  }

  void submit() {
    if (!primary_->active()) return;
    if (replacement_) {
      path = std::move(replacement_);
      hide();
      return;
    }
    auto result = typed_path(request_, std::string_view(input_->value(), static_cast<std::size_t>(input_->size())));
    if (const auto *error = std::get_if<const char *>(&result)) {
      reason(*error, StatusKind::failure);
      if (accessibility_) accessibility_->announce(reason_, *error);
      input_->take_focus();
      observe(FilePromptStage::error);
      return;
    }
    auto &accepted = std::get<TypedPath>(result);
    if (!accepted.replace) {
      path = std::move(accepted.path);
      hide();
      return;
    }
    replacement_ = std::move(accepted.path);
    input_->deactivate();
    primary_->copy_label("&Replace");
    alternative_->copy_label("&Back");
    alternative_->shortcut(0);
    reason(replace_question, StatusKind::warning);
    if (accessibility_) accessibility_->announce(reason_, replace_question);
    primary_->take_focus();
    observe(FilePromptStage::confirm_replace);
  }

  void alternate() {
    if (replacement_) {
      replacement_.reset();
      primary_->copy_label(action_label_.c_str());
      alternative_->copy_label("Open file &dialog...");
      alternative_->shortcut(FL_ALT | 'd');
      reason(nullptr, StatusKind::none);
      input_->activate();
      input_->take_focus();
      return;
    }
    consent = true;
    hide();
  }

  const EditorFileRequest &request_;
  EditorAccessibility *accessibility_;
  const std::string action_label_;
  std::optional<std::filesystem::path> replacement_;
  Fl_Input *input_{};
  Fl_Box *message_{}, *reason_{};
  EditorStatusMark *mark_{};
  Fl_Button *primary_{}, *alternative_{}, *cancel_{};
  unsigned draw_count_{};
};
#endif

} // namespace

std::optional<std::filesystem::path> choose_editor_file(const EditorFileRequest &request) {
  std::optional<std::filesystem::path> chosen;
  if (editor_capture_exclusion_requested()) {
#ifdef _WIN32
    FilePathDialog prompt(request, request.app.accessibility.get());
    prompt.run();
    chosen = std::move(prompt.path);
    if (!chosen && !prompt.consent) return std::nullopt;
#else
    return std::nullopt;
#endif
  }
  auto *remembered = remembered_directory(request.memory);
  if (!chosen) {
    const auto directory = !remembered ? std::string{} :
        remembered->empty() ? file_chooser_start_directory() : *remembered;
    chosen = native_presenter ? native_presenter(request, directory) : present_native_file(request, directory);
  }
  if (chosen && remembered) *remembered = platform::path_to_utf8(chosen->parent_path()).value_or("");
  return chosen;
}

void set_native_file_presenter_for_test(NativeFilePresenter presenter) {
  native_presenter = std::move(presenter);
}

void set_file_prompt_observer_for_test(FilePromptObserver observer) {
  prompt_observer = std::move(observer);
}

} // namespace layerc::editor
