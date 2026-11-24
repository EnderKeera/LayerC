#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Button;
class Fl_Input;
class Fl_Window;

namespace layerc::editor {

class EditorApp;

struct EditorFileRequest {
  enum class Mode { open, save };
  enum class DirectoryMemory { none, profiles, traces };
  EditorApp &app;
  Mode mode;
  std::string title;
  std::string action;
  std::string filter_name;
  std::vector<std::string> extensions;
  DirectoryMemory memory = DirectoryMemory::none;
};

[[nodiscard]] std::optional<std::filesystem::path>
choose_editor_file(const EditorFileRequest &request);

using NativeFilePresenter = std::function<std::optional<std::filesystem::path>(
    const EditorFileRequest &, const std::string &directory)>;
void set_native_file_presenter_for_test(NativeFilePresenter presenter);

enum class FilePromptStage { shown_and_verified, error, confirm_replace };

struct FilePromptView {
  Fl_Window &window;
  Fl_Input &input;
  Fl_Button &primary;
  Fl_Button &alternative;
  Fl_Button &cancel;
  const Fl_Box &message;
  const Fl_Box &reason;
  unsigned draw_count;
};

using FilePromptObserver = std::function<void(const FilePromptView &, FilePromptStage)>;
void set_file_prompt_observer_for_test(FilePromptObserver observer);

} // namespace layerc::editor
