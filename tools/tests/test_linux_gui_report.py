import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

TOOLS = Path(__file__).parents[1] / "linux-gui"
REPORT = TOOLS / "report.py"
sys.path.insert(0, str(TOOLS))
from walkthrough import EXPECTED  # noqa: E402
from editor_resources import ADVANCED_VIEWS  # noqa: E402

MIB = 1024 * 1024
ROLES = ("wm", "fixture", "daemon", "sampler", "bridge", "outer-xvfb")


class LinuxGuiReportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.out = Path(self.directory.name)
        (self.out / "ownership").mkdir()
        for name in ROLES:
            self.write(f"ownership/{name}.status", {"state": "stopped", "code": 0})
        self.write("ownership/outer-session.status", {"state": "exited", "code": 0})
        self.write("resources.json", {"peakIdleUssBytes": 7 * MIB,
                                      "idleCpuPercent": 0.0, "idleSeconds": 60.0})
        self.write("startup-resources.json", {"peakUssKiB": 7 * 1024, "passed": True,
                                              "stoppedByHarness": True, "applicationPid": 1234,
                                              "applicationStartTime": "4242", "nonce": "n0nce"})
        for view in ADVANCED_VIEWS:
            self.write(f"automation-{view}-resources.json", self.advanced_resources())
        self.cases([(name, "pass") for name in EXPECTED])

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, name: str, value: object) -> None:
        (self.out / name).write_text(json.dumps(value), encoding="utf-8")

    def cases(self, cases: list[tuple[str, str]], required: object = True) -> None:
        self.write("cases.json", {"deferred": {}, "cases": [
            {"id": name, "required": required, "status": status} for name, status in cases]})

    def run_report(self, mode: str = "walkthrough", session_exit: int = 0,
                   joined: str = "1", sandbox_removed: str = "1", leftovers: str = "",
                   lock_left: str = "0", stale: str = "") -> dict[str, Any]:
        completed = subprocess.run(
            [sys.executable, str(REPORT), str(self.out), mode, str(session_exit),
             joined, sandbox_removed, leftovers, lock_left, stale],
            capture_output=True, text=True, check=False)
        result = json.loads((self.out / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(completed.returncode, 0 if result["passed"] else 1, completed.stderr)
        return result

    def assert_fails(self, result: dict[str, Any], fragment: str) -> None:
        self.assertFalse(result["passed"])
        self.assertTrue(any(fragment in failure for failure in result["failures"]),
                        result["failures"])

    def test_complete_evidence_passes(self) -> None:
        result = self.run_report()
        self.assertTrue(result["passed"], result["failures"])
        cleanup = json.loads((self.out / "cleanup.json").read_text(encoding="utf-8"))
        self.assertTrue(cleanup["ownedProcessesJoined"] and cleanup["sandboxRemoved"])

    def test_blocked_or_failed_required_case_fails(self) -> None:
        names = list(EXPECTED)
        self.cases([(names[0], "fail"), (names[1], "blocked")] +
                   [(name, "pass") for name in names[2:]])
        self.assert_fails(self.run_report(session_exit=1), f"{names[0]}, {names[1]}")

    def test_missing_empty_duplicate_or_optional_cases_fail(self) -> None:
        (self.out / "cases.json").unlink()
        self.assert_fails(self.run_report(), "no case report")
        self.cases([])
        self.assert_fails(self.run_report(), "required cases missing")
        names = list(EXPECTED)
        self.cases([(name, "pass") for name in names[1:]])
        self.assert_fails(self.run_report(), f"required cases missing: {names[0]}")
        self.cases([(name, "pass") for name in names] + [(names[0], "pass")])
        self.assert_fails(self.run_report(), "duplicate case record")
        self.cases([(name, "pass") for name in names] + [("invented", "pass")])
        self.assert_fails(self.run_report(), "unexpected case records")
        self.cases([(name, "pass") for name in names], required="yes")
        self.assert_fails(self.run_report(), "required cases not passed")

    def test_malformed_evidence_fails_instead_of_crashing(self) -> None:
        self.write("cases.json", {"deferred": {}, "cases": [{"id": [], "required": True}]})
        self.assert_fails(self.run_report(), "malformed case record")
        self.cases([(name, "pass") for name in EXPECTED])
        self.write("ownership/wm.status", {"state": "stopped", "code": False})
        self.assert_fails(self.run_report(), "owned process wm")
        self.write("ownership/wm.status", {"state": "stopped", "code": 0})
        self.write("startup-resources.json", [1, 2])
        self.assert_fails(self.run_report(), "resource evidence is missing")
        (self.out / "startup-resources.json").write_text(
            '{"peakUssKiB": 1' + "0" * 400 + ', "passed": true, "stoppedByHarness": true}')
        self.assert_fails(self.run_report(), "peakUssKiB is missing or not a finite")
        (self.out / "cases.json").write_text("[]")
        self.assert_fails(self.run_report(), "no case report")

    def test_agent_session_needs_no_scripted_cases(self) -> None:
        (self.out / "cases.json").unlink()
        result = self.run_report(mode="session")
        self.assertTrue(result["passed"], result["failures"])
        self.assertNotIn("cases", result)

    def test_cleanup_residue_fails(self) -> None:
        self.assert_fails(self.run_report(leftovers="4242 4343"), "cleanup")
        self.assert_fails(self.run_report(lock_left="1"), "cleanup")
        self.assert_fails(self.run_report(joined="0"), "cleanup")
        self.assert_fails(self.run_report(sandbox_removed="0"), "cleanup")
        result = self.run_report(stale="1234")  # A recorded stale own lock is not residue.
        self.assertTrue(result["passed"], result["failures"])
        cleanup = json.loads((self.out / "cleanup.json").read_text(encoding="utf-8"))
        self.assertEqual(cleanup["staleOwnLocksRemoved"], ["1234"])

    def test_abnormal_or_missing_owned_process_fails(self) -> None:
        self.write("ownership/daemon.status", {"state": "failed", "code": 70})
        self.assert_fails(self.run_report(), "owned process daemon")
        for role in ROLES:
            with self.subTest(role=role):
                self.tearDown()
                self.setUp()
                (self.out / "ownership" / f"{role}.status").unlink()
                self.assert_fails(self.run_report(), f"owned process record missing: {role}")

    def advanced_resources(self, moments: Any = None) -> dict[str, Any]:
        moments = moments if moments is not None else [100.0 + 0.25 * i for i in range(241)]
        return {"process": "layere", "pid": 1234, "startTime": "4242", "nonce": "n0nce",
                "peakIdleUssBytes": 8 * MIB,
                "peakIdlePssBytes": 9 * MIB, "peakIdleRssBytes": 12 * MIB,
                "idleCpuPercent": 0.0, "idleSeconds": 60.0,
                "samples": [{"monotonic": moment, "ussBytes": 8 * MIB,
                             "pssBytes": 9 * MIB, "rssBytes": 12 * MIB}
                            for moment in moments]}

    def test_advanced_resource_evidence_is_required_and_bounded(self) -> None:
        for view in ADVANCED_VIEWS:
            with self.subTest(view=view):
                path = self.out / f"automation-{view}-resources.json"
                path.unlink()
                self.assert_fails(self.run_report(), f"automation {view}: resource evidence is missing")
                self.write(path.name, self.advanced_resources())
        for key, value, fragment in (
                ("peakIdleUssBytes", 49 * MIB, "resource target exceeded"),
                ("idleCpuPercent", 0.3, "resource target exceeded"),
                ("idleSeconds", 1.0, "idle window shorter"),
                ("peakIdlePssBytes", True, "not a finite"),
                ("peakIdleRssBytes", float("nan"), "not a finite"),
                ("pid", 1235, "owned editor"),
                ("pid", True, "owned editor"),
                ("startTime", "4243", "owned editor"),
                ("nonce", "other", "owned editor"),
                ("idleSeconds", 65.0, "do not cover the reported idle window"),
                ("samples", [], "memory samples are missing"),
                ("samples", ["x"] * 241, "memory samples are missing"),
                ("samples", [{"monotonic": 100 + i} for i in range(150)], "too few"),
                ("samples", [{"monotonic": 100.0 + 0.25 * i} for i in range(241)], "not a finite")):
            with self.subTest(key=key, value=value):
                report = self.advanced_resources()
                report[key] = value
                self.write("automation-json-resources.json", report)
                self.assert_fails(self.run_report(), fragment)
        report = self.advanced_resources()
        report["samples"][1]["monotonic"] = 99
        self.write("automation-json-resources.json", report)
        self.assert_fails(self.run_report(), "timestamps are not increasing")
        report = self.advanced_resources()
        report["samples"][1]["ussBytes"] += MIB
        self.write("automation-json-resources.json", report)
        self.assert_fails(self.run_report(), "does not match its samples")
        # Two quick readings cannot stand in for a reported sixty-second window.
        self.write("automation-json-resources.json",
                   self.advanced_resources([100.0 + 0.04 * i for i in range(241)]))
        self.assert_fails(self.run_report(), "do not cover the reported idle window")
        self.write("automation-json-resources.json",
                   self.advanced_resources([100.0 + 0.25 * i for i in range(241) if not 100 <= i < 108]))
        self.assert_fails(self.run_report(), "unobserved gap")
        self.write("automation-json-resources.json", self.advanced_resources())
        startup = json.loads((self.out / "startup-resources.json").read_text())
        startup["applicationPid"] = True
        self.write("startup-resources.json", startup)
        self.assert_fails(self.run_report(), "names no owned editor")

    def test_resource_targets_and_malformed_samples_fail(self) -> None:
        self.write("resources.json", {"peakIdleUssBytes": 21 * MIB,
                                      "idleCpuPercent": 0.3, "idleSeconds": 60.0})
        result = self.run_report()
        self.assert_fails(result, "idleUssBytes")
        self.assert_fails(result, "idleCpuPercent")
        (self.out / "resources.json").write_text(
            '{"peakIdleUssBytes": NaN, "idleCpuPercent": 0, "idleSeconds": 60}')
        self.assert_fails(self.run_report(), "peakIdleUssBytes is missing or not a finite")
        self.write("resources.json", {"peakIdleUssBytes": MIB, "idleCpuPercent": 0,
                                      "idleSeconds": 1.0})
        self.assert_fails(self.run_report(), "idle window shorter")
        self.write("resources.json", {"peakIdleUssBytes": MIB, "idleCpuPercent": 0,
                                      "idleSeconds": 60.0})
        self.write("startup-resources.json", {"passed": True, "stoppedByHarness": True})
        self.assert_fails(self.run_report(), "peakUssKiB is missing")
        self.write("startup-resources.json", {"peakUssKiB": 1, "passed": 1,
                                              "stoppedByHarness": True})
        self.assert_fails(self.run_report(), "startup sampler did not complete")
        (self.out / "resources.json").unlink()
        self.assert_fails(self.run_report(), "resource evidence is missing")


if __name__ == "__main__":
    unittest.main()
