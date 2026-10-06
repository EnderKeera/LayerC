---
name: layerc-profile-store-roundtrip
description: Verify profile and sidecar persistence, recovery, and bundle round trips.
---

# LayerC profile store roundtrip

Use disposable stores for mutation and fault injection. A damaged user store is read-only evidence unless repair is requested.

## Map the document and writer

Read [profile locations and format](../../../README.md#profile-locations-and-format) and [profile_store.hpp](../../../src/platform/include/layerc/platform/profile_store.hpp). Determine which files belong to the affected document: base profile, visual/automation/companion sidecars, assets, and persistent automation state. Derive roots and suffixes from current code rather than reconstructing them from memory.

Identify the entry point and follow it to the shared transaction boundary:

| Entry point | Inspect |
| --- | --- |
| Editor save/autosave or accepted calibration | [editor_profiles.cpp](../../../src/editor/editor_profiles.cpp), [editor_live_edit.cpp](../../../src/editor/editor_live_edit.cpp), [editor_visual.cpp](../../../src/editor/editor_visual.cpp) |
| Linux profile/bundle command | [layerctl.cpp](../../../src/ctl/layerctl.cpp) |
| Bundle validation and asset mapping | [profile_pack.cpp](../../../src/bundle/profile_pack.cpp), [pack_detail.cpp](../../../src/bundle/pack_detail.cpp) |
| Windows commit/recovery | [profile_transaction.cpp](../../../src/platform/profile_transaction.cpp), [profile_file_guard.cpp](../../../src/platform/win32/profile_file_guard.cpp) |
| Linux commit/recovery | [linux/profile_store.cpp](../../../src/platform/linux/profile_store.cpp), [linux/profile_file_guard.cpp](../../../src/platform/linux/profile_file_guard.cpp) |

## Choose the relevant experiment

- **Save or upgrade:** snapshot the disposable document, make one change, reload it, and compare all affected modules. Exercise an invalid sidecar or rejected upgrade only when that boundary is involved. Invalid modules must not silently become removals.
- **Interrupted transaction:** reuse the existing test seam in [profile_transaction_testing.cpp](../../../src/platform/profile_transaction_testing.cpp) or [profile_transaction_test.hpp](../../../src/platform/linux/profile_transaction_test.hpp). Select publish/commit interruption points from the implementation. Verify recovery yields the complete prior or committed document as appropriate, and that a second recovery is stable.
- **Writer contention:** use existing lock/ownership scenarios; check failure is visible and leaves the last consistent document usable. Do not add a production fault-injection switch.
- **Import/export:** round-trip a fixture with the sidecars/assets relevant to the change. Check conflict policy, validation before publication, deterministic export where promised, and hash-based deduplication. Include malformed archive/path/size cases only for the touched boundary.
- **Cross-platform transfer:** transfer validated JSON or `.layercprofile` bundles. Use golden parity tests; do not copy an entire Windows store into XDG roots or vice versa.
- **Recovery assets:** check references from valid recovery copies before treating an asset as orphaned. Preserve malformed evidence until the recovery policy has classified it.

Keep fixture roots explicit for every process. Confirm the current Windows profile-root override or Linux XDG roots before launch; an existing daemon may still own a different store.

## Focused verification

Use the affected scenarios in [platform_live_scenarios.cpp](../../../tests/platform_live_scenarios.cpp), [linux_profile_store_tests.cpp](../../../tests/linux_profile_store_tests.cpp), [profile_pack_tests.cpp](../../../tests/profile_pack_tests.cpp), or [linux_profile_pack_tests.cpp](../../../tests/linux_profile_pack_tests.cpp). Their executable targets are `layerc_platform_tests`, `layerc_linux_profile_store_tests`, `layerc_bundle_tests`, and `layerc_linux_profile_pack_tests`; build only the relevant platform/target. Schema/parser changes may additionally need `layerc_core_tests`.

For visible editor save/reload behavior, continue with [draft-to-daemon sync](../layere-draft-to-daemon-sync/SKILL.md). For authenticated CLI routing, use [Linux programs-per-environment verification](../layerc-linux-programs-per-environment/SKILL.md).

## Completion

Verify the affected document across write, reload, and the nearest failure or recovery case. Report fixture roots, mutation/interruption point, module or hash comparison, recovery outcome, and subsequent load/write result without embedding private profile content.
