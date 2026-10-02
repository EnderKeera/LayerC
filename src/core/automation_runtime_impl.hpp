#pragma once

// Internal definitions shared by the automation runtime translation units.
#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_groups.hpp"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

[[nodiscard]] std::string runtime_effect_key(const AutomationEffect &effect);
[[nodiscard]] bool runtime_effect_precedes(const AutomationEffect &left,
                                           const AutomationEffect &right);

struct AttemptComplete {};
struct AutomationFault {
  std::string automation_id;
  AutomationError error;
};
struct BudgetExhausted {
  AutomationError error;
};
struct AttemptRejected {
  AutomationError error;
};
using AttemptOutcome = std::variant<AttemptComplete, AutomationFault,
                                    BudgetExhausted, AttemptRejected>;
using ExpressionResult =
    std::variant<AutomationValue, AutomationError, BudgetExhausted>;

struct EvaluationBudget {
  TimePoint deadline;
  std::size_t &node_evaluations;
  const AutomationRuntime::ClockReader &clock;

  [[nodiscard]] std::optional<BudgetExhausted> check() const {
    if (clock() >= deadline)
      return BudgetExhausted{
          {"$runtime", "automation execution exceeded 2 ms"}};
    return std::nullopt;
  }
  [[nodiscard]] std::optional<BudgetExhausted> consume_node() {
    if (auto exhausted = check())
      return exhausted;
    if (node_evaluations == max_automation_node_evaluations)
      return BudgetExhausted{
          {"$runtime", "root event exceeded 8192 node evaluations"}};
    ++node_evaluations;
    return std::nullopt;
  }
};

struct AutomationRuntime::Impl {
  AutomationProfile profile;
  // Group semantics come only from the shared topology (F-A11): the runtime
  // derives intra-batch events from the same sets the engine applies.
  CooldownGroupTopology groups;
  AutomationRuntimeSnapshot state;
  AutomationSessionEdges edges;
  std::set<std::string, std::less<>> faulted;
  bool persistent_dirty{};
  bool trace_enabled{};
  std::uint64_t trace_sequence{};
  std::vector<AutomationTraceEntry> trace_entries;
  ClockReader clock;

  Impl(AutomationProfile source, AutomationPersistentValues persistent,
       std::span<const CooldownGroup> group_definitions,
       ClockReader clock_reader);

  void make_trace_room();
  [[nodiscard]] std::optional<AutomationTraceEntry>
  begin_trace(const AutomationEvent &event);
  void add_event_trace(const AutomationEvent &event, std::string phase);
  void add_trace(const AutomationEvent &event, std::string automation,
                 std::string rule, std::string phase, std::string detail);

  [[nodiscard]] ExpressionResult eval(const AutomationExpression &expression,
                                      const AutomationEvent &event,
                                      const AutomationRuntimeSnapshot &snapshot,
                                      EvaluationBudget &budget) const;
  bool apply_internal(const AutomationEffect &effect, TimePoint now,
                      AutomationRuntimeSnapshot &target, AutomationError &error,
                      std::vector<AutomationEvent> &derived) const;

  struct EvaluationStage {
    AutomationRuntimeSnapshot root_snapshot;
    AutomationRuntimeSnapshot staged;
    std::vector<AutomationEvent> queue;
    std::set<std::uint64_t> fired;
    std::map<std::string, AutomationEffect, std::less<>> winners;
    EvaluationBudget &budget;
    AutomationEvaluationResult &result;
  };

  [[nodiscard]] AttemptOutcome evaluate_event(const AutomationEvent &root,
                                              const AutomationEvent &event,
                                              EvaluationStage &stage);
  [[nodiscard]] AttemptOutcome commit_batch(EvaluationStage &stage,
                                            const EffectValidator &validator,
                                            const EffectCommitter &committer);
  [[nodiscard]] AttemptOutcome evaluate(const AutomationEvent &root,
                                        const EffectValidator &validator,
                                        const EffectCommitter &committer,
                                        EvaluationBudget &budget,
                                        AutomationEvaluationResult &result);
};

} // namespace layerc::core
