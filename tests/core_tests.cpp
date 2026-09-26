#include "core_test_cases.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  using namespace layerc::test;
  return run({
      {"test_cooldown_charges", test_cooldown_charges},
      {"test_non_stacking_cooldown", test_non_stacking_cooldown},
      {"test_restart_policy", test_restart_policy},
      {"test_restart_with_empty_stacked_charges", test_restart_with_empty_stacked_charges},
      {"test_cooldown_control_operations", test_cooldown_control_operations},
      {"test_cooldown_recovery_modes_and_groups", test_cooldown_recovery_modes_and_groups},
      {"test_snapshot_reuse", test_snapshot_reuse},
      {"test_input_chord_edges", test_input_chord_edges},
      {"test_input_matcher_state_management", test_input_matcher_state_management},
      {"test_hid_boolean_evaluation", test_hid_boolean_evaluation},
      {"test_profile_parser", test_profile_parser},
      {"test_profile_input_code_validation", test_profile_input_code_validation},
      {"test_profile_game_matcher_utf8_validation", test_profile_game_matcher_utf8_validation},
      {"test_profile_identity_utf8_validation", test_profile_identity_utf8_validation},
      {"test_profile_serialization", test_profile_serialization},
      {"test_profile_enabled_and_independent_bar_placement", test_profile_enabled_and_independent_bar_placement},
      {"test_profile_schema3_hid", test_profile_schema3_hid},
      {"test_panel_geometry", test_panel_geometry},
      {"test_runtime_instance_tracking", test_runtime_instance_tracking},
      {"test_json_unicode_and_errors", test_json_unicode_and_errors},
      {"test_json_field_reader", test_json_field_reader},
      {"test_json_nesting_limit", test_json_nesting_limit},
      {"test_json_malformed_corpus", test_json_malformed_corpus},
      {"test_sha256_helpers", test_sha256_helpers},
      {"test_utf8_codec_and_casefold", test_utf8_codec_and_casefold},
      {"test_game_match_resolution_policy", test_game_match_resolution_policy},
      {"test_live_profile_reconciliation", test_live_profile_reconciliation},
      {"test_live_protocol_framing", test_live_protocol_framing},
      {"test_live_placement_math", test_live_placement_math},
      {"test_visual_profile_parser", test_visual_profile_parser},
      {"test_pixel_rule_evaluation", test_pixel_rule_evaluation},
      {"test_template_matching", test_template_matching},
      {"test_visual_trigger_state_machine", test_visual_trigger_state_machine},
      {"test_visual_repeat_when_ready", test_visual_repeat_when_ready},
      {"test_calibration_protocol", test_calibration_protocol},
      {"test_automation_profile_round_trip", test_automation_profile_round_trip},
      {"test_automation_session_trigger_policy", test_automation_session_trigger_policy},
      {"test_automation_session_origin_and_persistence", test_automation_session_origin_and_persistence},
      {"test_automation_input_source_ids", test_automation_input_source_ids},
      {"test_automation_runtime_transaction_and_replay", test_automation_runtime_transaction_and_replay},
      {"test_automation_priority_fault_isolation_and_reconfigure", test_automation_priority_fault_isolation_and_reconfigure},
      {"test_automation_value_round_trip_and_bounds", test_automation_value_round_trip_and_bounds},
      {"test_automation_expression_and_action_contract", test_automation_expression_and_action_contract},
      {"test_automation_limits_and_fault_rollback", test_automation_limits_and_fault_rollback},
      {"test_automation_persistent_sync_and_cascade_bounds", test_automation_persistent_sync_and_cascade_bounds},
  });
}
