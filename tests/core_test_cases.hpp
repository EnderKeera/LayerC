#pragma once

namespace layerc::test {

void test_cooldown_charges();
void test_non_stacking_cooldown();
void test_restart_policy();
void test_restart_with_empty_stacked_charges();
void test_cooldown_control_operations();
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
void test_runtime_instance_tracking();
void test_json_unicode_and_errors();
void test_json_nesting_limit();
void test_json_malformed_corpus();
void test_sha256_helpers();
void test_live_profile_reconciliation();
void test_live_protocol_framing();
void test_live_placement_math();
void test_visual_profile_parser();
void test_pixel_rule_evaluation();
void test_template_matching();
void test_visual_trigger_state_machine();
void test_visual_repeat_when_ready();
void test_calibration_protocol();

} // namespace layerc::test
