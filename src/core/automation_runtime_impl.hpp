#pragma once

#include "layerc/core/automation.hpp"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::core {

[[nodiscard]] std::string runtime_effect_key(const AutomationEffect &effect);
[[nodiscard]] bool runtime_effect_precedes(const AutomationEffect &left,
                                           const AutomationEffect &right);

struct AutomationRuntime::Impl {
  AutomationProfile profile;
  std::vector<CooldownGroup> cooldown_groups;
  AutomationRuntimeSnapshot state;
  AutomationSessionEdges edges;
  std::set<std::string, std::less<>> faulted;
  bool persistent_dirty{};
  bool trace_enabled{};
  std::uint64_t trace_sequence{};
  std::vector<AutomationTraceEntry> trace_entries;

  Impl(AutomationProfile source, AutomationPersistentValues persistent,
       std::span<const CooldownGroup> groups);

  void consume_timer_firing(std::string_view id, TimePoint now);
  void make_trace_room();
  void add_source_trace(const AutomationEvent &event);
  void add_trace(const AutomationEvent &event, std::string automation,
                 std::string rule, std::string phase, std::string detail);

  [[nodiscard]] std::optional<AutomationValue>
  eval(const AutomationExpression &expression, const AutomationEvent &event,
       const AutomationRuntimeSnapshot &snapshot, std::size_t &evaluations,
       AutomationError &error) const;
  [[nodiscard]] std::vector<std::string>
  propagated_cooldowns(std::span<const std::string> seeds) const;
  [[nodiscard]] std::vector<std::string>
  group_cooldowns(std::string_view group_id, bool propagate) const;
  [[nodiscard]] std::vector<std::string>
  reset_cooldowns(std::string_view cooldown_id) const;
  bool apply_internal(const AutomationEffect &effect, TimePoint now,
                      AutomationRuntimeSnapshot &target, AutomationError &error,
                      std::vector<AutomationEvent> &derived) const;

  struct EvaluationStage {
    AutomationRuntimeSnapshot root_snapshot;
    AutomationRuntimeSnapshot staged;
    std::vector<AutomationEvent> queue;
    std::set<std::uint64_t> fired;
    std::map<std::string, AutomationEffect, std::less<>> winners;
    TimePoint deadline;
    AutomationEvaluationResult &result;
    AutomationError &fault;
  };

  // Returns the automation to fault and retry after a transactional failure;
  // nullopt means the attempt committed, rejected, or hit a cap.
  [[nodiscard]] std::optional<std::string>
  evaluate_event(const AutomationEvent &root, const AutomationEvent &event,
                 EvaluationStage &stage);
  [[nodiscard]] std::optional<std::string>
  commit_batch(EvaluationStage &stage, const EffectValidator &validator,
               const EffectCommitter &committer);
  [[nodiscard]] std::optional<std::string>
  evaluate(const AutomationEvent &root, const EffectValidator &validator,
           const EffectCommitter &committer, TimePoint deadline,
           AutomationEvaluationResult &result, AutomationError &fault);
};

} // namespace layerc::core
