"""Boundary-evidence gate tests. All parser/GUI verdicts here are MOCK fixtures."""
import copy
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import test_active_resources as raw_fixtures

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
import report as gui_report  # noqa: E402
from limit_inputs import REQUIRED_GUI_CASES  # noqa: E402
from limit_resources import evaluate  # noqa: E402
from limits_walkthrough import REQUIRED_WINDOWS, contains_authored, verify_automation_preserved  # noqa: E402


class LimitResourceTests(unittest.TestCase):
    def setUp(self):
        self.raw = raw_fixtures.ActiveResourceTests()
        self.raw.setUp()
        self.addCleanup(self.raw.tearDown)
        self.out = self.raw.out
        source = b"{}"
        source_sha = hashlib.sha256(source).hexdigest()
        records = [{"caseId": name, "kind": "profile", "bytes": len(source),
                    "sha256": source_sha, "expectedAccepted": True,
                    "validator": {"accepted": True}, "editorOperationAccepted": True}
                   for name in sorted(REQUIRED_GUI_CASES)]
        self.record = next(row for row in records if row["caseId"] == "profiles:baseline")
        self.manifest = {"schema": 1, "passed": True, "limits": {"historyCapacity": 50},
                         "requiredGuiCases": sorted(REQUIRED_GUI_CASES), "cases": records}
        self.selection = {"schema": 1, "case": self.record, "compiledLimits": self.manifest["limits"],
                          "inputArtifact": "limit-input.json", "profileSha256": source_sha}
        (self.out / "limit-input.json").write_bytes(source)
        (self.out / "profile-before.json").write_bytes(source)
        window = self.raw.activity["windows"]["simulator-play"]
        self.activity = {"passed": True,
                         "windows": {name: copy.deepcopy(window) for name in REQUIRED_WINDOWS},
                         "operation": {"accepted": True, "outcome": "Save acknowledged"}}
        self.save()

    def save(self):
        self.raw.write("limit-input-manifest.json", self.manifest)
        digest = hashlib.sha256((self.out / "limit-input-manifest.json").read_bytes()).hexdigest()
        self.selection["inputsManifestSha256"] = digest
        self.activity["fixture"] = {"boundaryCase": self.record["caseId"], "inputsManifestSha256": digest}
        self.raw.write("limit-case.json", self.selection)
        self.raw.write("limit-windows.json", self.activity)

    def test_same_bound_raw_samples_and_selected_input_are_required(self):
        result = evaluate(self.out)
        self.assertEqual(result["caseId"], "profiles:baseline")
        self.assertEqual(result["targetPeakUssBytes"], 48 * 1024**2)
        self.assertEqual(set(result["windows"]), set(REQUIRED_WINDOWS))

    def test_manifest_rows_cannot_be_replaced_by_a_declared_denominator(self):
        self.manifest["cases"] = [self.record]
        self.save()
        with self.assertRaisesRegex(ValueError, "missing a required boundary family"):
            evaluate(self.out)

    def test_duplicate_case_and_disagreeing_compiled_verdict_fail(self):
        original = copy.deepcopy(self.manifest)
        for mutate in (lambda m: m["cases"].append(copy.deepcopy(m["cases"][0])),
                       lambda m: m["cases"][0]["validator"].update(accepted=False)):
            self.manifest = copy.deepcopy(original)
            mutate(self.manifest)
            self.save()
            with self.assertRaises(ValueError):
                evaluate(self.out)

    def test_input_profile_and_manifest_tampering_fail(self):
        for name in ("limit-input.json", "profile-before.json", "limit-input-manifest.json"):
            original = (self.out / name).read_bytes()
            (self.out / name).write_bytes(original + b" ")
            with self.assertRaises(ValueError):
                evaluate(self.out)
            (self.out / name).write_bytes(original)

    def test_missing_semantic_operation_and_recovery_fail(self):
        self.activity["windows"].pop("boundary-recovery")
        self.save()
        with self.assertRaises(ValueError):
            evaluate(self.out)

    def test_baseline_refusal_and_unexplained_refusal_fail(self):
        self.activity["operation"] = {"accepted": False, "outcome": "resource limit"}
        self.save()
        with self.assertRaisesRegex(ValueError, "baseline edit/save was refused"):
            evaluate(self.out)
        self.record = next(row for row in self.manifest["cases"]
                           if row["caseId"] == "profiles:opaque-string-byte-limit")
        self.selection["case"] = self.record
        self.activity["operation"]["outcome"] = "Unexplained I/O failure"
        self.save()
        with self.assertRaisesRegex(ValueError, "no visible resource-limit reason"):
            evaluate(self.out)

    def test_format_valid_trace_can_be_explicitly_refused_by_editor_policy(self):
        self.record = next(row for row in self.manifest["cases"]
                           if row["caseId"] == "traces:trace-opaque-array-byte-limit")
        self.record.update(kind="trace", editorOperationAccepted=False)
        self.selection["case"] = self.record
        self.activity["operation"] = {"accepted": False, "sourceSha256": self.record["sha256"],
            "outcome": "Trace import resource limit exceeded (131072 JSON values)"}
        self.save()
        self.assertFalse(evaluate(self.out)["operation"]["accepted"])
        self.activity["operation"].update(accepted=True, outcome="Trace loaded")
        self.save()
        with self.assertRaisesRegex(ValueError, "reviewed allocation policy"):
            evaluate(self.out)

    def test_peak_spike_is_not_hidden_by_small_startup_or_idle(self):
        self.raw.samples[15]["ussBytes"] = 49 * 1024**2
        self.raw.save_samples()
        self.raw.save()
        with self.assertRaisesRegex(ValueError, "USS target exceeded"):
            evaluate(self.out)

    def test_original_members_array_order_and_numeric_values_are_preserved(self):
        self.assertTrue(contains_authored({"future": [18446744073709551615, True]},
                                         {"future": [18446744073709551615, True], "default": 1}))
        self.assertFalse(contains_authored({"future": [1, 2]}, {"future": [2, 1]}))
        self.assertFalse(contains_authored({"future": 18446744073709551615},
                                          {"future": 18446744073709551616}))
        self.assertFalse(contains_authored({"future": True}, {"future": 1}))
        self.assertFalse(contains_authored({"future": 1}, {}))

    def test_json_numbers_compare_by_exact_value_not_spelling(self):
        # The canonical writer saves an unchanged 0.0 coordinate as 0.
        self.assertTrue(contains_authored({"region": {"x": 0.0, "width": 0.5}},
                                          {"region": {"x": 0, "width": 0.5}}))
        self.assertTrue(contains_authored({"count": 2}, {"count": 2.0}))
        self.assertFalse(contains_authored({"x": 0.1}, {"x": 0.1000000000000001}))
        self.assertFalse(contains_authored({"x": 9007199254740993}, {"x": 9007199254740992.0}))
        self.assertFalse(contains_authored({"x": 0}, {"x": False}))
        self.assertFalse(contains_authored({"x": False}, {"x": 0.0}))
        self.assertFalse(contains_authored({"x": 0}, {"x": "0"}))
        self.assertFalse(contains_authored({"x": 0.0}, {"x": None}))


class AutomationPreservationTests(unittest.TestCase):
    """MOCK command transport; real typed-codec checks run against the probe."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.out = Path(temporary.name)
        self.probe = self.out / "mock-probe"
        self.probe.write_bytes(b"MOCK comparator, never executed")
        for name in ("profile-before.json", "module-before.json", "module-after.json"):
            (self.out / name).write_text("{}")

    def test_production_comparator_is_retained_and_its_receipt_is_bound(self):
        with mock.patch("limits_walkthrough.command", return_value=(0, {"accepted": True, "equivalent": True})) as command:
            receipt = verify_automation_preserved(self.out, self.probe)
        argv, output = command.call_args.args
        self.assertEqual(argv, [self.out / "module-preservation-probe", "--compare-automation",
                               self.out / "profile-before.json", self.out / "module-before.json",
                               self.out / "module-after.json"])
        self.assertEqual(output, self.out / "module-preservation")
        self.assertEqual(receipt["probeSha256"], hashlib.sha256(self.probe.read_bytes()).hexdigest())
        self.assertEqual(receipt["beforeSha256"], hashlib.sha256(b"{}").hexdigest())
        self.assertEqual(receipt["afterSha256"], hashlib.sha256(b"{}").hexdigest())
        self.assertTrue((self.out / "module-preservation.json").is_file())

    def test_mismatch_and_nonzero_exit_cannot_be_accepted(self):
        for code, verdict in ((0, {"accepted": True, "equivalent": False}),
                              (2, {"accepted": True, "equivalent": True}),
                              (0, {"accepted": False, "equivalent": True})):
            with self.subTest(code=code, verdict=verdict):
                retained = self.out / "module-preservation-probe"
                if retained.exists():
                    retained.unlink()
                with mock.patch("limits_walkthrough.command", return_value=(code, verdict)):
                    with self.assertRaisesRegex(Exception, "changed authored automation values"):
                        verify_automation_preserved(self.out, self.probe)

    def test_comparator_change_during_execution_fails_closed(self):
        def changed(*_args):
            (self.out / "module-preservation-probe").write_bytes(b"changed")
            return 0, {"accepted": True, "equivalent": True}
        with mock.patch("limits_walkthrough.command", side_effect=changed):
            with self.assertRaisesRegex(Exception, "changed during comparison"):
                verify_automation_preserved(self.out, self.probe)


class ResourceTargetScopeTests(unittest.TestCase):
    def test_empty_and_representative_ceilings_are_unchanged(self):
        mib = 1024**2
        for mode in ("agent", "walkthrough", "wayland"):
            self.assertEqual(gui_report.resource_targets(mode),
                {"idleUssBytes": 20 * mib, "startupPeakUssBytes": 20 * mib, "idleCpuPercent": 0.2})
        for mode in ("resources", "peaks"):
            self.assertEqual(gui_report.resource_targets(mode),
                {"idleUssBytes": 32 * mib, "startupPeakUssBytes": 32 * mib, "idleCpuPercent": 0.2})

    def test_selected_boundary_document_uses_its_unchanged_lifetime_peak_bound(self):
        mib = 1024**2
        self.assertEqual(gui_report.resource_targets("limits"),
            {"idleUssBytes": 48 * mib, "startupPeakUssBytes": 48 * mib, "idleCpuPercent": 0.2})
        self.assertNotEqual(gui_report.resource_targets("limits"), gui_report.TARGETS)


if __name__ == "__main__":
    unittest.main()
