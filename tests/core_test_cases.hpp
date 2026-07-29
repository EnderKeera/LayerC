#pragma once

namespace layerc::test {

void test_cooldown_charges();
void test_non_stacking_cooldown();
void test_restart_policy();
void test_snapshot_reuse();
void test_input_chord_edges();
void test_profile_parser();
void test_profile_input_code_validation();
void test_profile_game_matcher_utf8_validation();
void test_profile_identity_utf8_validation();
void test_profile_serialization();
void test_profile_enabled_and_independent_bar_placement();
void test_runtime_instance_tracking();
void test_json_unicode_and_errors();
void test_json_nesting_limit();

} // namespace layerc::test