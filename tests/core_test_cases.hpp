#pragma once

namespace layerc::test {

void test_cooldown_charges();
void test_non_stacking_cooldown();
void test_restart_policy();
void test_restart_with_empty_stacked_charges();
void test_cooldown_control_operations();
void test_cooldown_recovery_modes_and_groups();
void test_snapshot_reuse();
void test_input_chord_edges();
void test_input_matcher_state_management();
void test_hid_boolean_evaluation();
void test_profile_parser();
void test_profile_input_code_validation();
void test_profile_game_matcher_utf8_validation();
void test_profile_identity_utf8_validation();
void test_profile_serialization();
void test_profile_enabled_and_independent_bar_placement();
void test_profile_schema3_hid();
void test_panel_geometry();
void test_runtime_instance_tracking();
void test_json_unicode_and_errors();
void test_json_field_reader();
void test_json_nesting_limit();
void test_json_malformed_corpus();
void test_sha256_helpers();
void test_utf8_codec_and_casefold();
void test_update_manifest_round_trip();
void test_update_manifest_rejects_unsafe_input();
void test_update_manifest_plan();
void test_game_match_resolution_policy();
void test_live_profile_reconciliation();
void test_live_protocol_framing();
void test_live_placement_math();
void test_visual_profile_parser();
void test_pixel_rule_evaluation();
void test_template_matching();
void test_visual_trigger_state_machine();
void test_visual_repeat_when_ready();
void test_calibration_protocol();
void test_automation_profile_round_trip();
void test_automation_session_trigger_policy();
void test_automation_session_origin_and_persistence();
void test_automation_input_source_ids();
void test_automation_session_continuous_input();
void test_automation_runtime_transaction_and_replay();
void test_automation_priority_fault_isolation_and_reconfigure();
void test_automation_value_round_trip_and_bounds();
void test_automation_expression_and_action_contract();
void test_automation_limits_and_fault_rollback();
void test_automation_persistent_sync_and_cascade_bounds();

} // namespace layerc::test
