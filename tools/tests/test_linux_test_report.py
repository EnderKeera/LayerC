import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "linux_test_report.py"
SPEC = importlib.util.spec_from_file_location("linux_test_report", MODULE_PATH)
assert SPEC and SPEC.loader
linux_test_report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linux_test_report)


class LinuxTestReportTests(unittest.TestCase):
    def write(self, root: Path, name: str, content: str) -> Path:
        path = root / name
        path.write_text(content, encoding="utf-8")
        return path

    def test_accepts_nonempty_complete_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(
                root, "registered.json",
                json.dumps({"tests": [{"name": "x11"}, {"name": "capture"}]}),
            )
            junit = self.write(
                root, "ctest.xml",
                '<testsuite><testcase name="x11"/><testcase name="capture"/></testsuite>',
            )
            report = linux_test_report.validate(registration, junit, ["x11", "capture"])
            self.assertEqual("pass", report["result"])
            self.assertEqual(2, report["tests"])

    def test_rejects_empty_or_failed_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(
                root, "registered.json", json.dumps({"tests": [{"name": "x11"}]})
            )
            empty = self.write(root, "empty.xml", "<testsuite/>")
            report = linux_test_report.validate(registration, empty, ["x11"])
            self.assertEqual("fail", report["result"])
            self.assertEqual(0, report["tests"])
            self.assertEqual(["x11"], report["missingExecuted"])
            failed = self.write(
                root, "failed.xml",
                '<testsuite><testcase name="x11"><failure/></testcase></testsuite>',
            )
            report = linux_test_report.validate(registration, failed, ["x11"])
            self.assertEqual("fail", report["result"])
            self.assertEqual(1, report["failures"])

    def test_rejects_missing_and_skipped_required_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(
                root, "registered.json", json.dumps({"tests": [{"name": "x11"}]})
            )
            junit = self.write(
                root, "ctest.xml",
                '<testsuite><testcase name="x11"><skipped/></testcase></testsuite>',
            )
            report = linux_test_report.validate(registration, junit, ["x11", "capture"])
            self.assertEqual("fail", report["result"])
            self.assertEqual(["capture"], report["missingRequired"])
            self.assertEqual(["capture"], report["missingExecuted"])
            self.assertEqual(1, report["skipped"])

    def test_label_selects_registered_suites_and_allows_named_skip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(root, "registered.json", json.dumps({"tests": [
                {"name": "core", "properties": [{"name": "LABELS", "value": ["headless"]}]},
                {"name": "overlay", "properties": [{"name": "LABELS", "value": ["headless"]}]},
                {"name": "x11", "properties": [{"name": "LABELS", "value": ["x11"]}]},
            ]}))
            junit = self.write(root, "ctest.xml", '<testsuite><testcase name="core"/><testcase name="overlay"><skipped/></testcase></testsuite>')
            report = linux_test_report.validate(
                registration, junit, [], labels=("headless",), allow_skip=("overlay",)
            )
            self.assertEqual("pass", report["result"])
            self.assertEqual(["core", "overlay"], report["required"])
            without_allowance = linux_test_report.validate(registration, junit, [], labels=("headless",))
            self.assertEqual("fail", without_allowance["result"])
            self.assertIn("unexpected skipped suites: overlay", without_allowance["problems"])
            missing = linux_test_report.validate(registration, junit, [], labels=("x11",))
            self.assertEqual(["x11"], missing["missingExecuted"])

    def test_rejects_extra_unregistered_and_duplicate_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(root, "registered.json", json.dumps({"tests": [
                {"name": "core", "properties": [
                    {"name": "LABELS", "value": ["headless"]}]},
                {"name": "x11", "properties": [
                    {"name": "LABELS", "value": ["x11"]}]},
            ]}))
            junit = self.write(root, "ctest.xml", '<testsuite>'
                '<testcase name="core"/><testcase name="core"/>'
                '<testcase name="x11"/><testcase name="unknown"/></testsuite>')
            report = linux_test_report.validate(
                registration, junit, [], labels=("headless",))
            self.assertEqual("fail", report["result"])
            self.assertEqual(["unknown", "x11"], report["unexpected"])
            self.assertIn("duplicate testcase names in CTest report", report["problems"])
            self.assertIn("unregistered suites in CTest report: unknown", report["problems"])

    def test_rejects_no_selected_suite_and_notrun(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            registration = self.write(root, "registered.json", '{"tests": [{"name": "a"}]}')
            junit = self.write(root, "ctest.xml", '<testsuite><testcase name="a" status="notrun"/></testsuite>')
            report = linux_test_report.validate(registration, junit, [])
            self.assertEqual("fail", report["result"])
            self.assertIn("no required tests were selected", report["problems"])
            self.assertIn("tests did not run: a", report["problems"])


if __name__ == "__main__":
    unittest.main()
