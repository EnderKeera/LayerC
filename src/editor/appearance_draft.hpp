#pragma once

#include "layerc/platform/windows.hpp"

#include <utility>

namespace layerc::editor {

class AppearanceDraft {
public:
  void begin(const layerc::platform::EditorAppearanceSettings &settings) {
    persisted_ = settings;
    staged_ = settings;
  }

  void cancel() { staged_ = persisted_; }

  [[nodiscard]] const layerc::platform::EditorAppearanceSettings &staged() const {
    return staged_;
  }

  [[nodiscard]] layerc::platform::EditorAppearanceSettings &staged() {
    return staged_;
  }

  template <typename Save> [[nodiscard]] bool apply(Save &&save) {
    if (!std::forward<Save>(save)(staged_))
      return false;
    persisted_ = staged_;
    return true;
  }

private:
  layerc::platform::EditorAppearanceSettings persisted_{};
  layerc::platform::EditorAppearanceSettings staged_{};
};

} // namespace layerc::editor
