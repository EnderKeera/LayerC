#pragma once

#include "daemon_link.hpp"
#include "profile_document.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace layerc::editor {

// Text values are kept outside the document until the complete candidate
// validates. An invalid input must not truncate a duration or change a panel.
struct SliceFields {
  std::string name;
  std::string executable;
  bool enabled{};
  std::string cooldown_name;
  std::string duration_seconds;
  std::string panel_x;
  std::string panel_y;
};

struct SaveOutcome {
  CommitOutcome commit;
  std::optional<ReloadResult> reload;
  std::string text;
  [[nodiscard]] bool saved() const noexcept { return commit.ok(); }
};

// Toolkit-free controller for the initial Linux vertical slice. All files
// come from the normal XDG store; GUI tests supply disposable XDG roots.
class LinuxEditorModel final {
public:
  explicit LinuxEditorModel(
      std::filesystem::path directory = layerc::platform::profile_directory());
  void scan();
  [[nodiscard]] const layerc::platform::ProfileDirectoryScan &profiles() const {
    return scan_;
  }
  [[nodiscard]] std::optional<std::string> open(std::size_t row);
  // A Save can reorder the scan; selection must keep the intended file identity.
  [[nodiscard]] std::optional<std::string> open_path(const std::filesystem::path &path);
  [[nodiscard]] bool has_document() const noexcept { return opened_; }
  [[nodiscard]] ProfileDocument &document() noexcept { return document_; }
  [[nodiscard]] const ProfileDocument &document() const noexcept { return document_; }
  [[nodiscard]] SliceFields fields() const;
  // Returns a specific visible error, leaving the document unchanged on error.
  [[nodiscard]] std::optional<std::string> apply(const SliceFields &fields);
  [[nodiscard]] SaveOutcome save(const SliceFields &fields,
                                 const ConfirmUpgrade &confirm_upgrade);

private:
  std::filesystem::path directory_;
  layerc::platform::ProfileDirectoryScan scan_;
  ProfileDocument document_;
  bool opened_{};
};

} // namespace layerc::editor
