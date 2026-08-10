#pragma once

#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace layerc::editor {

struct EditorProfileSnapshot {
  layerc::core::GameProfile profile;
  std::optional<layerc::core::VisualProfile> visual_profile;

  bool operator==(const EditorProfileSnapshot &) const = default;
};

class EditorProfileHistory {
public:
  static constexpr std::size_t capacity = 50;

  void reset(const layerc::core::GameProfile &profile,
             const std::optional<layerc::core::VisualProfile> &visual_profile =
                 std::nullopt) {
    undo_.clear();
    redo_.clear();
    baseline_ = {profile, visual_profile};
    saved_ = baseline_;
    coalescing_text_ = false;
  }

  [[nodiscard]] bool record(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile =
          std::nullopt,
      const bool text_change = false) {
    const EditorProfileSnapshot next{profile, visual_profile};
    if (next == baseline_) return false;
    if (!(text_change && coalescing_text_)) {
      push_bounded(undo_, baseline_);
      redo_.clear();
    }
    baseline_ = next;
    coalescing_text_ = text_change;
    return true;
  }
  // Back-compat: record(profile, text_change) was the old signature.
  [[nodiscard]] bool record(const layerc::core::GameProfile &profile,
                            const bool text_change) {
    return record(profile, std::nullopt, text_change);
  }

  void rebase(const layerc::core::GameProfile &profile,
              const std::optional<layerc::core::VisualProfile> &visual_profile =
                  std::nullopt) {
    baseline_ = {profile, visual_profile};
    saved_ = baseline_;
    coalescing_text_ = false;
  }

  void end_text_coalescing() noexcept { coalescing_text_ = false; }

  // Call after recording and successfully persisting a visual-only change.
  // Only the current/saved snapshots advance: older undo/redo entries retain
  // the visual state they actually represented, so replay stays deterministic.
  void rebase_visual(
      const std::optional<layerc::core::VisualProfile> &visual_profile) {
    baseline_.visual_profile = visual_profile;
    saved_.visual_profile = visual_profile;
    coalescing_text_ = false;
  }

  [[nodiscard]] bool is_baseline(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile =
          std::nullopt) const noexcept {
    return EditorProfileSnapshot{profile, visual_profile} == baseline_;
  }
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  [[nodiscard]] bool is_saved(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile =
          std::nullopt) const noexcept {
    return EditorProfileSnapshot{profile, visual_profile} == saved_;
  }
  [[nodiscard]] bool is_dirty(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile =
          std::nullopt,
      const bool persisted = true) const noexcept {
    // A draft has no file to fall back to, even when its current value matches
    // the initial history snapshot.  It must keep prompting until first save.
    return !persisted || !is_saved(profile, visual_profile);
  }
  [[nodiscard]] bool is_dirty(const layerc::core::GameProfile &p,
                              const bool persisted) const noexcept {
    return is_dirty(p, std::nullopt, persisted);
  }

  [[nodiscard]] std::optional<layerc::core::GameProfile>
  undo(const layerc::core::GameProfile &current) {
    auto result = undo(current, std::nullopt);
    if (!result.has_value()) return std::nullopt;
    return std::move(result->profile);
  }
  [[nodiscard]] std::optional<EditorProfileSnapshot> undo(
      const layerc::core::GameProfile &current,
      const std::optional<layerc::core::VisualProfile> &visual_profile) {
    if (undo_.empty()) return std::nullopt;
    push_bounded(redo_, {current, visual_profile});
    auto result = std::move(undo_.back());
    undo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

  [[nodiscard]] std::optional<layerc::core::GameProfile>
  redo(const layerc::core::GameProfile &current) {
    auto result = redo(current, std::nullopt);
    if (!result.has_value()) return std::nullopt;
    return std::move(result->profile);
  }
  [[nodiscard]] std::optional<EditorProfileSnapshot> redo(
      const layerc::core::GameProfile &current,
      const std::optional<layerc::core::VisualProfile> &visual_profile) {
    if (redo_.empty()) return std::nullopt;
    push_bounded(undo_, {current, visual_profile});
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
    if (stack.size() > capacity) stack.pop_front();
  }

  EditorProfileSnapshot baseline_;
  EditorProfileSnapshot saved_;
  std::deque<EditorProfileSnapshot> undo_;
  std::deque<EditorProfileSnapshot> redo_;
  bool coalescing_text_{};
};

} // namespace layerc::editor
