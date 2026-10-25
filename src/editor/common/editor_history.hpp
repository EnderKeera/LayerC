#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

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

  // A copy draft has never been committed, so it has no saved snapshot and is
  // dirty by construction.
  void reset(const layerc::core::GameProfile &profile,
             const std::optional<layerc::core::VisualProfile> &visual_profile,
             const std::optional<layerc::core::AutomationProfile>
                 &automation_profile,
             const bool saved = true) {
    undo_.clear();
    redo_.clear();
    baseline_ = {profile, visual_profile, automation_profile};
    if (saved)
      saved_ = baseline_;
    else
      saved_.reset();
    coalescing_text_ = false;
  }

  [[nodiscard]] bool record(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile,
      const std::optional<layerc::core::AutomationProfile> &automation_profile,
      const bool text_change = false) {
    if (matches(baseline_, profile, visual_profile, automation_profile))
      return false;
    const EditorProfileSnapshot next{profile, visual_profile,
                                     automation_profile};
    if (!(text_change && coalescing_text_)) {
      push_bounded(undo_, redo_, baseline_);
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

  // A recorded edit inside a draft means the draft has unsaved content: undo
  // and redo never restore the saved snapshot; a commit rebases it again.
  void forget_saved() noexcept { saved_.reset(); }

  void end_text_coalescing() noexcept { coalescing_text_ = false; }

  [[nodiscard]] bool
  is_baseline(const layerc::core::GameProfile &profile,
              const std::optional<layerc::core::VisualProfile> &visual_profile,
              const std::optional<layerc::core::AutomationProfile>
                  &automation_profile) const noexcept {
    return matches(baseline_, profile, visual_profile, automation_profile);
  }
  // Every visual profile history can still restore, for owners of resources
  // that retained snapshots reference. Stored entries keep only that member
  // apart from the fields they changed, so no full snapshot is materialised.
  template <typename Visit> void for_each_visual_profile(Visit &&visit) const {
    visit(baseline_.visual_profile);
    if (saved_) visit(saved_->visual_profile);
    for (const auto &stored : undo_) visit(*stored.visual_profile);
    for (const auto &stored : redo_) visit(*stored.visual_profile);
  }
  // Distinct heavy-member copies retained across both stacks; diagnostic only.
  [[nodiscard]] std::size_t stored_member_copies() const noexcept {
    std::vector<const void *> seen;
    seen.reserve(10 * 2 * capacity);
    const auto note = [&](const void *pointer) {
      if (std::find(seen.begin(), seen.end(), pointer) == seen.end())
        seen.push_back(pointer);
    };
    for (const auto *stack : {&undo_, &redo_})
      for (const auto &stored : *stack) {
        note(stored.modules.get());
        note(stored.panels.get());
        note(stored.cooldowns.get());
        note(stored.cooldown_groups.get());
        note(stored.hid_bindings.get());
        note(stored.bindings.get());
        note(stored.extra_fields.get());
        note(stored.game_extra_fields.get());
        note(stored.visual_profile.get());
        note(stored.automation_profile.get());
      }
    return seen.size();
  }
  // Storage retained by heads, diagnostic only.
  [[nodiscard]] std::size_t stored_head_bytes() const noexcept {
    std::size_t bytes{};
    for (const auto *stack : {&undo_, &redo_})
      for (const auto &stored : *stack) {
        bytes += stored.head.modules.capacity() *
                 sizeof(layerc::core::ProfileModule);
        bytes += stored.head.panels.capacity() * sizeof(layerc::core::Panel);
        bytes += stored.head.cooldowns.capacity() *
                 sizeof(layerc::core::CooldownDefinition);
        bytes += stored.head.cooldown_groups.capacity() *
                 sizeof(layerc::core::CooldownGroup);
        bytes += stored.head.hid_bindings.capacity() *
                 sizeof(layerc::core::HidControlBinding);
        bytes += stored.head.bindings.capacity() *
                 sizeof(layerc::core::InputBinding);
      }
    return bytes;
  }
  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
  [[nodiscard]] bool
  is_saved(const layerc::core::GameProfile &profile,
           const std::optional<layerc::core::VisualProfile> &visual_profile,
           const std::optional<layerc::core::AutomationProfile>
               &automation_profile) const noexcept {
    return saved_ &&
           matches(*saved_, profile, visual_profile, automation_profile);
  }
  [[nodiscard]] bool is_dirty(
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile,
      const std::optional<layerc::core::AutomationProfile> &automation_profile)
      const noexcept {
    return !is_saved(profile, visual_profile, automation_profile);
  }

  [[nodiscard]] std::optional<EditorProfileSnapshot>
  undo(const layerc::core::GameProfile &current,
       const std::optional<layerc::core::VisualProfile> &visual_profile,
       const std::optional<layerc::core::AutomationProfile>
           &automation_profile) {
    if (undo_.empty())
      return std::nullopt;
    push_bounded(redo_, undo_, {current, visual_profile, automation_profile});
    auto result = materialize(undo_.back());
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
    push_bounded(undo_, redo_, {current, visual_profile, automation_profile});
    auto result = materialize(redo_.back());
    redo_.pop_back();
    baseline_ = result;
    coalescing_text_ = false;
    return result;
  }

private:
  // Entries share unchanged members, so history that differs in one field
  // costs that field instead of another full profile. head is a complete copy
  // of the profile with the heavy members cleared, so any GameProfile field
  // added later stays correct by construction. The game-level opaque fields
  // are shared too.
  struct StoredSnapshot {
    layerc::core::GameProfile head;
    std::shared_ptr<const std::vector<layerc::core::ProfileModule>> modules;
    std::shared_ptr<const std::vector<layerc::core::Panel>> panels;
    std::shared_ptr<const std::vector<layerc::core::CooldownDefinition>>
        cooldowns;
    std::shared_ptr<const std::vector<layerc::core::CooldownGroup>>
        cooldown_groups;
    std::shared_ptr<const std::vector<layerc::core::HidControlBinding>>
        hid_bindings;
    std::shared_ptr<const std::vector<layerc::core::InputBinding>> bindings;
    std::shared_ptr<const layerc::core::json::Value::Object> extra_fields;
    std::shared_ptr<const layerc::core::json::Value::Object> game_extra_fields;
    std::shared_ptr<const std::optional<layerc::core::VisualProfile>>
        visual_profile;
    std::shared_ptr<const std::optional<layerc::core::AutomationProfile>>
        automation_profile;
  };

  template <typename T>
  static std::shared_ptr<const std::remove_cvref_t<T>>
  share(T &&value, const std::shared_ptr<const std::remove_cvref_t<T>> *neighbour) {
    using Value = std::remove_cvref_t<T>;
    if (neighbour && *neighbour && **neighbour == value) return *neighbour;
    return std::make_shared<const Value>(std::forward<T>(value));
  }

  // clear() keeps a vector's allocation, so heads release storage by swapping
  // with an empty value instead.
  template <typename T> static void release(T &value) noexcept {
    T().swap(value);
  }

  static StoredSnapshot store(const EditorProfileSnapshot &snapshot,
                              const StoredSnapshot *neighbour) {
    StoredSnapshot stored;
    stored.head = snapshot.profile;
    stored.modules =
        share(std::move(stored.head.modules), neighbour ? &neighbour->modules : nullptr);
    stored.panels =
        share(std::move(stored.head.panels), neighbour ? &neighbour->panels : nullptr);
    stored.cooldowns = share(std::move(stored.head.cooldowns),
                             neighbour ? &neighbour->cooldowns : nullptr);
    stored.cooldown_groups =
        share(std::move(stored.head.cooldown_groups),
              neighbour ? &neighbour->cooldown_groups : nullptr);
    stored.hid_bindings = share(std::move(stored.head.hid_bindings),
                                neighbour ? &neighbour->hid_bindings : nullptr);
    stored.bindings = share(std::move(stored.head.bindings),
                            neighbour ? &neighbour->bindings : nullptr);
    stored.extra_fields = share(std::move(stored.head.extra_fields),
                                neighbour ? &neighbour->extra_fields : nullptr);
    stored.game_extra_fields =
        share(std::move(stored.head.game.extra_fields),
              neighbour ? &neighbour->game_extra_fields : nullptr);
    stored.visual_profile = share(
        snapshot.visual_profile, neighbour ? &neighbour->visual_profile : nullptr);
    stored.automation_profile =
        share(snapshot.automation_profile,
              neighbour ? &neighbour->automation_profile : nullptr);
    release(stored.head.modules);
    release(stored.head.panels);
    release(stored.head.cooldowns);
    release(stored.head.cooldown_groups);
    release(stored.head.hid_bindings);
    release(stored.head.bindings);
    release(stored.head.extra_fields);
    release(stored.head.game.extra_fields);
    return stored;
  }

  static EditorProfileSnapshot materialize(const StoredSnapshot &stored) {
    EditorProfileSnapshot snapshot;
    snapshot.profile = stored.head;
    snapshot.profile.modules = *stored.modules;
    snapshot.profile.panels = *stored.panels;
    snapshot.profile.cooldowns = *stored.cooldowns;
    snapshot.profile.cooldown_groups = *stored.cooldown_groups;
    snapshot.profile.hid_bindings = *stored.hid_bindings;
    snapshot.profile.bindings = *stored.bindings;
    snapshot.profile.extra_fields = *stored.extra_fields;
    snapshot.profile.game.extra_fields = *stored.game_extra_fields;
    snapshot.visual_profile = *stored.visual_profile;
    snapshot.automation_profile = *stored.automation_profile;
    return snapshot;
  }

  static bool matches(
      const EditorProfileSnapshot &snapshot,
      const layerc::core::GameProfile &profile,
      const std::optional<layerc::core::VisualProfile> &visual_profile,
      const std::optional<layerc::core::AutomationProfile> &automation_profile) noexcept {
    return snapshot.profile == profile &&
           snapshot.visual_profile == visual_profile &&
           snapshot.automation_profile == automation_profile;
  }

  static void push_bounded(std::deque<StoredSnapshot> &stack,
                           const std::deque<StoredSnapshot> &other,
                           const EditorProfileSnapshot &snapshot) {
    const StoredSnapshot *neighbour = nullptr;
    if (!stack.empty())
      neighbour = &stack.back();
    else if (!other.empty())
      neighbour = &other.back();
    stack.push_back(store(snapshot, neighbour));
    if (stack.size() > capacity)
      stack.pop_front();
  }

  EditorProfileSnapshot baseline_;
  std::optional<EditorProfileSnapshot> saved_;
  std::deque<StoredSnapshot> undo_;
  std::deque<StoredSnapshot> redo_;
  bool coalescing_text_{};
};

} // namespace layerc::editor
