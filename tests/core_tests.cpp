#include "core_test_cases.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  using namespace layerc::test;

  test_cooldown_charges();
  test_non_stacking_cooldown();
  test_restart_policy();
  test_restart_with_empty_stacked_charges();
  test_cooldown_control_operations();
  test_snapshot_reuse();
  test_input_chord_edges();
  test_input_matcher_state_management();
  test_profile_parser();
  test_profile_input_code_validation();
  test_profile_game_matcher_utf8_validation();
  test_profile_identity_utf8_validation();
  test_profile_serialization();
  test_profile_enabled_and_independent_bar_placement();
  test_runtime_instance_tracking();
  test_json_unicode_and_errors();
  test_json_nesting_limit();
  test_json_malformed_corpus();
  test_live_profile_reconciliation();
  test_live_protocol_framing();
  test_live_placement_math();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All LayerC core tests passed\n";
  return EXIT_SUCCESS;
}
