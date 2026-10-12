#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace layerc::editor {

struct EditorProfileSnapshot {
  layerc::core::GameProfile profile;
  std::optional<layerc::core::VisualProfile> visual_profile;
  std::optional<layerc::core::AutomationProfile> automation_profile;

  bool operator==(const EditorProfileSnapshot &) const = default;
};

class EditorProfileHistory {
public:
  static constexpr std::size_t capacity = 50;

  void reset(const layerc::core::GameProfile &profile,
             const std::optional<layerc::core::VisualProfile> &visual_profile,
             const std::optional<layerc::core::AutomationProfile>
                 &automation_profile) {
    undo_.clear();
    redo_.clear();
    baseline_ = {profile, visual_profile, automation_profile};
    saved_ = baseline_;
    coalescing_text_ = false;
  }

  [[nodiscard]] bool record(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile,
      const std::optional<layerc::core::AutomationProfile> &automation_profile,
      const bool text_change = false) {
    const EditorProfileSnapshot next{profile, visual_profile,
                                     automation_profile};
    if (next == baseline_)
      return false;
    if (!(text_change && coalescing_text_)) {
      push_bounded(undo_, baseline_);
      redo_.clear();
    }
    baseline_ = next;
    coalescing_text_ = text_change;
    return true;
  }

  void rebase(const layerc::core::GameProfile &profile,
              const std::optional<layerc::core::VisualProfile> &visual_profile,
              const std::optional<layerc::core::AutomationProfile>
                  &automation_profile) {
    baseline_ = {profile, visual_profile, automation_profile};
    saved_ = baseline_;
    coalescing_text_ = false;
  }

  void end_text_coalescing() noexcept { coalescing_text_ = false; }

  [[nodiscard]] bool
  is_baseline(const layerc::core::GameProfile &profile,
              const std::optional<layerc::core::VisualProfile> &visual_profile,
              const std::optional<layerc::core::AutomationProfile>
                  &automation_profile) const noexcept {
    return EditorProfileSnapshot{profile, visual_profile, automation_profile} ==
           baseline_;
  }
  // Every snapshot history can still restore, for owners of resources that
  // retained snapshots reference.
  template <typename Visit> void for_each_snapshot(Visit &&visit) const {
    visit(baseline_);
    visit(saved_);
    for (const auto &snapshot : undo_) visit(snapshot);
    for (const auto &snapshot : redo_) visit(snapshot);
  }
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  [[nodiscard]] bool
  is_saved(const layerc::core::GameProfile &profile,
           const std::optional<layerc::core::VisualProfile> &visual_profile,
           const std::optional<layerc::core::AutomationProfile>
               &automation_profile) const noexcept {
    return EditorProfileSnapshot{profile, visual_profile, automation_profile} ==
           saved_;
  }
  [[nodiscard]] bool is_dirty(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile,
      const std::optional<layerc::core::AutomationProfile> &automation_profile,
      const bool persisted = true) const noexcept {
    return !persisted || !is_saved(profile, visual_profile, automation_profile);
  }

  [[nodiscard]] std::optional<EditorProfileSnapshot>
  undo(const layerc::core::GameProfile &current,
       const std::optional<layerc::core::VisualProfile> &visual_profile,
       const std::optional<layerc::core::AutomationProfile>
           &automation_profile) {
    if (undo_.empty())
      return std::nullopt;
    push_bounded(redo_, {current, visual_profile, automation_profile});
    auto result = std::move(undo_.back());
    undo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

  [[nodiscard]] std::optional<EditorProfileSnapshot>
  redo(const layerc::core::GameProfile &current,
       const std::optional<layerc::core::VisualProfile> &visual_profile,
       const std::optional<layerc::core::AutomationProfile>
           &automation_profile) {
    if (redo_.empty())
      return std::nullopt;
    push_bounded(undo_, {current, visual_profile, automation_profile});
    auto result = std::move(redo_.back());
    redo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

private:
  static void push_bounded(std::deque<EditorProfileSnapshot> &stack,
                           const EditorProfileSnapshot &snapshot) {
    stack.push_back(snapshot);
    if (stack.size() > capacity)
      stack.pop_front();
  }

  EditorProfileSnapshot baseline_;
  EditorProfileSnapshot saved_;
  std::deque<EditorProfileSnapshot> undo_;
  std::deque<EditorProfileSnapshot> redo_;
  bool coalescing_text_{};
};

} // namespace layerc::editor