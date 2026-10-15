"""Policy/recovery tests for the backend gate runner, without a live desktop."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
POLICY = json.loads((REPO / "tools/test-gates.json").read_text())["gates"]


@unittest.skipUnless(shutil.which("bash") and os.name == "posix",
                     "backend gate shell fixtures run on Linux")
class BackendGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()
        self.registration = self.evidence / "ctest-registration.json"
        stub = self.bin / "ctest"
        stub.write_text("#!" + sys.executable + "\n" + """
import json, os, pathlib, sys
import xml.etree.ElementTree as ET
args = sys.argv[1:]
label = args[args.index("-L") + 1].strip("^$")
assert not any(name in os.environ for name in (
    "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS",
    "LAYERC_DISPLAY_BACKEND", "LAYERC_INPUT"))
tests = json.load(open(os.environ["FIXTURE_REGISTRATION"]))["tests"]
root = ET.Element("testsuite")
for test in tests:
    if label not in test["properties"][0]["value"]:
        continue
    case = ET.SubElement(root, "testcase", name=test["name"])
    if os.environ.get("FIXTURE_SKIP") == test["name"]:
        ET.SubElement(case, "skipped")
path = pathlib.Path(args[args.index("--output-junit") + 1])
ET.ElementTree(root).write(path)
sys.exit(int(os.environ.get("FIXTURE_CTEST_RC", "0")))
""")
        stub.chmod(0o755)

    def register(self, label="dbus", *, omit=(), unlabel=()):
        gate = POLICY["linux-" + label]
        names = set(gate["required"]) | set(gate.get("optional", {}))
        self.registration.write_text(json.dumps({"tests": [
            {"name": name, "properties": [
                {"name": "LABELS", "value": [] if name in unlabel else [label]}]}
            for name in sorted(names - set(omit))
        ]}))

    def run_gate(self, policy, harness="", **extra):
        env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                   FIXTURE_REGISTRATION=str(self.registration),
                   DISPLAY="never-connect", WAYLAND_DISPLAY="never-connect",
                   WAYLAND_SOCKET="123", DBUS_SESSION_BUS_ADDRESS="never-connect",
                   LAYERC_DISPLAY_BACKEND="wayland", LAYERC_INPUT="evdev", **extra)
        script = """
set -euo pipefail
source_dir=$1 evidence_dir=$2 build_dir=$3 wayland_policy=$4
source "$source_dir/tools/linux-backend-gates.sh"
record_stage() { printf '%s\\n%s\\n' "$2" "${3:-}" > "$evidence_dir/$1.stage"; }
run_logged() { shift; "$@"; }
if [[ -n $5 ]]; then run_backend_gate dbus "$5"; else run_backend_gate dbus; fi
"""
        return subprocess.run(
            ["bash", "-c", script, "_", str(REPO), str(self.evidence),
             str(self.root / "build"), policy, harness],
            env=env, text=True, capture_output=True, timeout=20)

    def stage(self):
        return (self.evidence / "dbus.stage").read_text().splitlines()[0]

    def verify_base(self, label, policy):
        registered = json.loads(self.registration.read_text())["tests"]
        xml = ET.Element("testsuite")
        for test in registered:
            ET.SubElement(xml, "testcase", name=test["name"])
        ET.ElementTree(xml).write(self.evidence / f"ctest-{label}.xml")
        script = """
set -euo pipefail
source_dir=$1 evidence_dir=$2 wayland_policy=$3
source "$source_dir/tools/linux-backend-gates.sh"
run_logged() { shift; "$@"; }
verify_base_gate "$4"
"""
        return subprocess.run(
            ["bash", "-c", script, "_", str(REPO), str(self.evidence), policy, label],
            text=True, capture_output=True, timeout=20)

    def test_required_base_gates_reject_each_missing_wayland_fixture(self):
        for label in ("headless", "x11"):
            optional = POLICY["linux-" + label]["optional"]
            fixtures = [name for name in optional if name.startswith("layerc_linux_wayland_")]
            self.assertTrue(fixtures)
            for name in fixtures:
                with self.subTest(label=label, suite=name):
                    self.register(label, omit=[name, "layerc_benchmark_result_tests"])
                    auto = self.verify_base(label, "auto")
                    self.assertEqual(0, auto.returncode, auto.stderr)
                    strict = self.verify_base(label, "required")
                    self.assertNotEqual(0, strict.returncode)
                    self.assertIn(name, strict.stderr)

    def test_required_base_gates_keep_powershell_optional(self):
        for label in ("headless", "x11"):
            with self.subTest(label=label):
                self.register(label, omit=["layerc_benchmark_result_tests"])
                strict = self.verify_base(label, "required")
                self.assertEqual(0, strict.returncode, strict.stderr)

    def test_off_does_not_read_registration(self):
        self.assertEqual(0, self.run_gate("off").returncode)
        self.assertEqual("not-run", self.stage())

    def test_absent_lane_is_unavailable_or_fails_when_required(self):
        self.registration.write_text('{"tests":[]}')
        self.assertEqual(0, self.run_gate("auto").returncode)
        self.assertEqual("unavailable", self.stage())
        self.assertNotEqual(0, self.run_gate("required").returncode)
        self.assertEqual("fail", self.stage())

    def test_missing_harness_is_never_a_pass(self):
        self.register()
        self.assertEqual(0, self.run_gate("auto", "layerc_nonexistent_fixture_tool").returncode)
        self.assertEqual("unavailable", self.stage())
        self.assertNotEqual(0, self.run_gate("required", "layerc_nonexistent_fixture_tool").returncode)

    def test_required_gate_and_environment_isolation(self):
        self.register()
        result = self.run_gate("required")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("pass", self.stage())

    def test_required_gate_rejects_missing_pipewire_and_lost_labels(self):
        name = "layerc_linux_pipewire_capture_tests"
        self.register(omit=[name])
        self.assertNotEqual(0, self.run_gate("required").returncode)
        self.register(unlabel=[name])
        self.assertNotEqual(0, self.run_gate("required").returncode)

    def test_allowed_skip_is_unavailable_in_auto_and_fails_required(self):
        self.register()
        extra = {"FIXTURE_SKIP": "layerc_linux_dbus_service_tests"}
        result = self.run_gate("auto", **extra)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("unavailable", self.stage())
        self.assertNotEqual(0, self.run_gate("required", **extra).returncode)

    def test_flatpak_only_cannot_claim_required_backend_coverage(self):
        result = subprocess.run(
            ["bash", str(REPO / "tools/run-linux-tests.sh"),
             "--source-dir", str(REPO), "--evidence-dir", str(self.evidence),
             "--flatpak-only", "--flatpak", "required", "--wayland", "required"],
            text=True, capture_output=True, timeout=20)
        self.assertEqual(64, result.returncode, result.stderr)
        self.assertIn("no --wayland required", result.stderr)
        self.assertEqual([], list(self.evidence.iterdir()))

    def test_ctest_failure_is_not_swallowed(self):
        self.register()
        self.assertEqual(12, self.run_gate("required", FIXTURE_CTEST_RC="12").returncode)


if __name__ == "__main__":
    unittest.main()
