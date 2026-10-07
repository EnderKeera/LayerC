#pragma once

namespace layerc::test::daemon_owner {
void input_owner_dispatches_without_a_daemon_app();
void dormant_owners_do_not_recreate_resources();
void automation_owner_controls_runtime_and_trace();
void live_owner_owns_control_resources_and_faults();
} // namespace layerc::test::daemon_owner
