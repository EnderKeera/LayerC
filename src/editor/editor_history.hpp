#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace layerc::editor {

class EditorProfileHistory {
public:
  static constexpr std::size_t capacity = 50;

  void reset(const layerc::core::GameProfile &profile) {
    undo_.clear();
    redo_.clear();
    baseline_ = profile;
    saved_ = profile;
    coalescing_text_ = false;
  }

  [[nodiscard]] bool
  record(const layerc::core::GameProfile &profile, const bool text_change) {
    if (profile == baseline_) return false;
    if (!(text_change && coalescing_text_)) {
      push_bounded(undo_, baseline_);
      redo_.clear();
    }
    baseline_ = profile;
    coalescing_text_ = text_change;
    return true;
  }

  void rebase(const layerc::core::GameProfile &profile) {
    baseline_ = profile;
    saved_ = profile;
    coalescing_text_ = false;
  }

  void end_text_coalescing() noexcept { coalescing_text_ = false; }

  [[nodiscard]] bool is_baseline(
      const layerc::core::GameProfile &profile) const noexcept {
    return profile == baseline_;
  }
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  [[nodiscard]] bool
  is_saved(const layerc::core::GameProfile &profile) const noexcept {
    return profile == saved_;
  }
  [[nodiscard]] bool
  is_dirty(const layerc::core::GameProfile &profile,
           const bool persisted) const noexcept {
    // A draft has no file to fall back to, even when its current value matches
    // the initial history snapshot.  It must keep prompting until first save.
    return !persisted || !is_saved(profile);
  }

  [[nodiscard]] std::optional<layerc::core::GameProfile>
  undo(const layerc::core::GameProfile &current) {
    if (undo_.empty()) return std::nullopt;
    push_bounded(redo_, current);
    auto result = std::move(undo_.back());
    undo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

  [[nodiscard]] std::optional<layerc::core::GameProfile>
  redo(const layerc::core::GameProfile &current) {
    if (redo_.empty()) return std::nullopt;
    push_bounded(undo_, current);
    auto result = std::move(redo_.back());
    redo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

private:
  static void push_bounded(std::deque<layerc::core::GameProfile> &stack,
                           const layerc::core::GameProfile &profile) {
    stack.push_back(profile);
    if (stack.size() > capacity) stack.pop_front();
  }

  layerc::core::GameProfile baseline_;
  layerc::core::GameProfile saved_;
  std::deque<layerc::core::GameProfile> undo_;
  std::deque<layerc::core::GameProfile> redo_;
  bool coalescing_text_{};
};

} // namespace layerc::editor
