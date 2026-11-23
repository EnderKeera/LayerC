#pragma once

namespace layerc::test::daemon_owner {
void input_owner_dispatches_without_a_daemon_app();
void dormant_owners_do_not_recreate_resources();
void automation_owner_controls_runtime_and_trace();
void live_owner_owns_control_resources_and_faults();
void live_owner_confirms_direct_drag_off();
void companion_owner_timer_lifecycle();
} // namespace layerc::test::daemon_owner
