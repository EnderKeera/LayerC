import importlib.util
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "linux_test_report.py"
SPEC = importlib.util.spec_from_file_location("linux_test_report", MODULE_PATH)
assert SPEC and SPEC.loader
linux_test_report = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their module through sys.modules.
sys.modules[SPEC.name] = linux_test_report
SPEC.loader.exec_module(linux_test_report)
Gate = linux_test_report.Gate

def registration(*tests: tuple[str, list[str]]) -> str:
    return json.dumps({"tests": [
        {"name": name, "properties": [{"name": "LABELS", "value": labels}]}
        for name, labels in tests
    ]})

def junit(*cases: str) -> str:
    return "<testsuite>" + "".join(cases) + "</testsuite>"

class LinuxTestReportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, name: str, content: str) -> Path:
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        return path

    def check(self, registered: str, report: str, gate: Gate) -> dict[str, object]:
        return linux_test_report.validate(
            self.write("registered.json", registered), self.write("ctest.xml", report), gate
        )

    def test_accepts_nonempty_complete_report(self) -> None:
        gate = Gate("g", frozenset({"x11", "capture"}), label="x11")
        report = self.check(
            registration(("x11", ["x11"]), ("capture", ["x11"])),
            junit('<testcase name="x11"/>', '<testcase name="capture"/>'),
            gate,
        )
        self.assertEqual("pass", report["result"], report["problems"])
        self.assertEqual(2, report["tests"])

    def test_rejects_empty_or_failed_report(self) -> None:
        gate = Gate("g", frozenset({"x11"}))
        registered = registration(("x11", ["x11"]))
        report = self.check(registered, "<testsuite/>", gate)
        self.assertEqual("fail", report["result"])
        self.assertEqual(0, report["tests"])
        self.assertEqual(["x11"], report["missingExecuted"])
        report = self.check(registered, junit('<testcase name="x11"><failure/></testcase>'), gate)
        self.assertEqual("fail", report["result"])
        self.assertEqual(1, report["failures"])

    def test_rejects_missing_and_skipped_required_coverage(self) -> None:
        gate = Gate("g", frozenset({"x11", "capture"}))
        report = self.check(
            registration(("x11", ["x11"])),
            junit('<testcase name="x11"><skipped/></testcase>'),
            gate,
        )
        self.assertEqual("fail", report["result"])
        self.assertEqual(["capture"], report["missingRequired"])
        self.assertEqual(["capture"], report["missingExecuted"])
        self.assertEqual(1, report["skipped"])

    def test_reports_why_a_suite_skipped(self) -> None:
        report = self.check(
            registration(("x11", ["x11"])),
            junit('<testcase name="x11"><skipped/><system-out>'
                  "PASS: probe\nskip: x11: cannot open display\nSKIP: tracker"
                  "</system-out></testcase>"),
            Gate("g", frozenset({"x11"})),
        )
        self.assertIn(
            "x11 skip reason: skip: x11: cannot open display | SKIP: tracker",
            report["problems"],
        )

    def test_unregistered_suite_fails_even_when_others_pass(self) -> None:
        # B08-01: a reduced but nonempty labelled registration used to define
        # its own expectations and pass.
        gate = Gate("g", frozenset({"core", "input"}), label="headless")
        report = self.check(
            registration(("core", ["required", "headless"])),
            junit('<testcase name="core"/>'),
            gate,
        )
        self.assertEqual("fail", report["result"])
        self.assertIn("required tests are not registered: input", report["problems"])

    def test_suite_that_loses_its_gate_label_fails(self) -> None:
        gate = Gate("g", frozenset({"core", "input"}), label="headless")
        report = self.check(
            registration(("core", ["headless"]), ("input", ["required"])),
            junit('<testcase name="core"/>'),
            gate,
        )
        self.assertEqual("fail", report["result"])
        self.assertIn("gate suites lack the headless label: input", report["problems"])
        self.assertEqual(["input"], report["missingExecuted"])

    def test_labelled_suite_missing_from_policy_fails(self) -> None:
        gate = Gate("headless-gate", frozenset({"core"}), label="headless")
        report = self.check(
            registration(("core", ["headless"]), ("new", ["headless"])),
            junit('<testcase name="core"/>', '<testcase name="new"/>'),
            gate,
        )
        self.assertEqual("fail", report["result"])
        self.assertIn(
            "registered headless not in the headless-gate gate policy: new",
            report["problems"],
        )

    def test_optional_suite_runs_only_where_registered(self) -> None:
        gate = Gate("g", frozenset({"core"}), label="headless",
                    optional=frozenset({"tooling"}))
        absent = self.check(
            registration(("core", ["headless"])), junit('<testcase name="core"/>'), gate)
        self.assertEqual("pass", absent["result"], absent["problems"])
        present = registration(("core", ["headless"]), ("tooling", ["headless"]))
        ran = self.check(present, junit('<testcase name="core"/>', '<testcase name="tooling"/>'), gate)
        self.assertEqual("pass", ran["result"], ran["problems"])
        self.assertEqual(["tooling"], ran["optionalRun"])
        dropped = self.check(present, junit('<testcase name="core"/>'), gate)
        self.assertEqual(["tooling"], dropped["missingExecuted"])

    def test_only_named_suites_may_skip(self) -> None:
        registered = registration(("core", ["required"]), ("overlay", ["required"]))
        report_xml = junit('<testcase name="core"/>',
                           '<testcase name="overlay"><skipped/></testcase>')
        allowed = Gate("g", frozenset({"core", "overlay"}), label="required",
                       allow_skip=frozenset({"overlay"}))
        report = self.check(registered, report_xml, allowed)
        self.assertEqual("pass", report["result"], report["problems"])
        self.assertEqual(["overlay"], report["allowedSkips"])
        strict = Gate("g", frozenset({"core", "overlay"}), label="required")
        report = self.check(registered, report_xml, strict)
        self.assertIn("unexpected skipped suites: overlay", report["problems"])

    def test_rejects_extra_unregistered_and_duplicate_cases(self) -> None:
        report = self.check(
            registration(("core", ["headless"]), ("x11", ["x11"])),
            junit('<testcase name="core"/>', '<testcase name="core"/>',
                  '<testcase name="x11"/>', '<testcase name="unknown"/>'),
            Gate("g", frozenset({"core"}), label="headless"),
        )
        self.assertEqual("fail", report["result"])
        self.assertEqual(["unknown", "x11"], report["unexpected"])
        self.assertIn("duplicate testcase names in CTest report", report["problems"])
        self.assertIn("unregistered suites in CTest report: unknown", report["problems"])

    def test_rejects_notrun(self) -> None:
        report = self.check(
            registration(("a", ["required"])),
            junit('<testcase name="a" status="notrun"/>'),
            Gate("g", frozenset({"a"})),
        )
        self.assertEqual("fail", report["result"])
        self.assertIn("tests did not run: a", report["problems"])

    def test_policy_rejects_inconsistent_gates(self) -> None:
        policy = self.write("gates.json", json.dumps({"gates": {
            "empty": {"required": []},
            "both": {"required": ["a"], "optional": {"a": "why"}},
            "skip": {"required": ["a"], "allowSkip": {"b": "why"}},
        }}))
        for name in ("empty", "both", "skip", "unknown"):
            with self.subTest(name), self.assertRaises(ValueError):
                linux_test_report.load_gate(policy, name)

class CheckedInPolicyTests(unittest.TestCase):
    """The policy names exactly the suites CMake can register."""

    repo = Path(__file__).parents[2]

    def test_policy_matches_cmake_registration(self) -> None:
        cmake = "".join(
            (self.repo / name).read_text(encoding="utf-8")
            for name in ("CMakeLists.txt", "cmake/LayerCTests.cmake")
        )
        declared = set(re.findall(r"layerc_add_test\((\w+)", cmake))
        declared |= set(re.findall(r"add_test\(NAME (\w+)", cmake))
        declared.discard("${target}")
        # Registered only with LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS, which
        # no automated gate enables.
        declared.discard("layerc_capture_integration_tests")
        gated: set[str] = set()
        policy = linux_test_report.DEFAULT_POLICY
        for name in json.loads(policy.read_text(encoding="utf-8"))["gates"]:
            gate = linux_test_report.load_gate(policy, name)
            gated |= gate.required | gate.optional
        self.assertEqual(set(), gated - declared, "policy names undeclared suites")
        self.assertEqual(set(), declared - gated, "CMake suites outside every gate")

if __name__ == "__main__":
    unittest.main()
