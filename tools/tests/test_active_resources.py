"""Active resource evidence is fail-closed and independently recomputed."""
import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[1] / "linux-gui"))
from active_resources import MEMORY_FIELDS, REQUIRED_WINDOWS, evaluate, memory  # noqa: E402


class ActiveResourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.out = Path(self.temporary.name)
        (self.out / "shots").mkdir()
        for name in ("before.png", "after.png"):
            (self.out / "shots" / name).write_bytes(b"fixture screenshot")
        self.samples = [{"monotonic": 100.0 + i * 0.005, "ussBytes": 10 * 1024**2,
                         "pssBytes": 12 * 1024**2, "rssBytes": 18 * 1024**2, "swapBytes": 0}
                        for i in range(41)]
        self.report = {"passed": True, "stoppedByHarness": True, "pid": 1234,
                       "startTime": "42", "nonce": "owned", "executable": "/private/layere",
                       "rawFile": "active-resources.jsonl", "sampleIntervalSeconds": 0.005,
                       "observedEditorChildren": [], "note": "fixture"}
        self.activity = {"passed": True,
                         "fixture": {"profiles": 20, "cooldownsEach": 10, "panelsEach": 2},
                         "windows": {name: {"start": 100.01, "end": 100.18, "verified": True,
                                             "screenshots": ["shots/before.png", "shots/after.png"]}
                                     for name in REQUIRED_WINDOWS}}
        self.write("editor-identity.json", {"pid": 1234, "startTime": "42", "nonce": "owned"})
        self.write("startup-resources.json", {"applicationPid": 1234, "applicationStartTime": "42",
                    "nonce": "owned", "executable": "/private/layere", "passed": True,
                    "stoppedByHarness": True})
        self.save_samples()
        self.save()

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, value):
        (self.out / name).write_text(json.dumps(value), encoding="utf-8")

    def save(self):
        self.write("active-resources.json", self.report)
        self.write("active-windows.json", self.activity)

    def save_samples(self):
        raw = b"".join((json.dumps(row) + "\n").encode("utf-8") for row in self.samples)
        (self.out / "active-resources.jsonl").write_bytes(raw)
        self.report.update(rawSha256=hashlib.sha256(raw).hexdigest(), sampleCount=len(self.samples),
            firstMonotonic=self.samples[0]["monotonic"], lastMonotonic=self.samples[-1]["monotonic"],
            maxSampleGapSeconds=max(b["monotonic"] - a["monotonic"]
                                    for a, b in zip(self.samples, self.samples[1:])),
            peak={key: max(row[key] for row in self.samples) for key in MEMORY_FIELDS})

    def test_raw_peak_and_every_running_operation_are_accepted(self):
        result = evaluate(self.out)
        self.assertEqual(result["peak"]["ussBytes"], 10 * 1024**2)
        self.assertEqual(set(result["windows"]), set(REQUIRED_WINDOWS))
        self.assertTrue(all(w["sampleCount"] >= 3 for w in result["windows"].values()))

    def test_boundary_workload_requires_its_selected_identity_and_operation(self):
        fixture = {"boundaryCase": "profiles:opaque-depth-byte-limit",
                   "inputsManifestSha256": "a" * 64}
        self.activity["fixture"] = fixture
        self.activity["windows"] = {"boundary-save": self.activity["windows"]["simulator-play"]}
        self.write("limit-windows.json", self.activity)
        options = dict(activity_file="limit-windows.json", fixture=fixture,
                       required_windows=("boundary-save",))
        result = evaluate(self.out, **options)
        self.assertEqual(result["targetPeakUssBytes"], 48 * 1024**2)
        self.assertEqual(set(result["windows"]), {"boundary-save"})
        with self.assertRaises(ValueError):
            evaluate(self.out, **dict(options, fixture={"boundaryCase": "other"}))
        with self.assertRaises(ValueError):
            evaluate(self.out, **dict(options, required_windows=("missing",)))
        with self.assertRaises(ValueError):
            evaluate(self.out, **dict(options, required_windows=()))
        with self.assertRaises(ValueError):
            evaluate(self.out, **dict(options, required_windows=("boundary-save", "boundary-save")))
        with self.assertRaises(ValueError):
            evaluate(self.out, **dict(options, activity_file="../limit-windows.json"))
        self.samples[15]["ussBytes"] = 49 * 1024**2
        self.save_samples()
        self.save()
        with self.assertRaisesRegex(ValueError, "USS target exceeded"):
            evaluate(self.out, **options)

    def test_spike_during_operation_fails_even_with_small_idle_and_startup(self):
        self.samples[15]["ussBytes"] = 49 * 1024**2
        self.save_samples()
        self.save()
        with self.assertRaisesRegex(ValueError, "USS target exceeded"):
            evaluate(self.out)

    def test_missing_window_stopped_unverified_or_uncovered_fails(self):
        original = copy.deepcopy(self.activity)
        for mutation in (lambda a: a["windows"].pop("simulator-play"),
                         lambda a: a.update(passed=False),
                         lambda a: a["windows"]["trace-recording"].update(verified=False),
                         lambda a: a["windows"]["x11-capture"].update(start=99.0),
                         lambda a: a["windows"]["template-matching"].update(end=101.0),
                         lambda a: a["fixture"].update(profiles=1)):
            with self.subTest(mutation=mutation):
                self.activity = copy.deepcopy(original)
                mutation(self.activity)
                self.save()
                with self.assertRaises(ValueError):
                    evaluate(self.out)

    def test_mismatched_identity_helpers_false_success_and_forged_peak_fail(self):
        original = copy.deepcopy(self.report)
        for key, value in (("pid", True), ("pid", 1235), ("startTime", "43"),
                           ("nonce", "other"), ("executable", "/other/layere"),
                           ("observedEditorChildren", [999]), ("passed", 1),
                           ("stoppedByHarness", False), ("sampleCount", 1),
                           ("rawSha256", "0" * 64), ("maxSampleGapSeconds", 0.0),
                           ("firstMonotonic", False), ("rawFile", "../other.jsonl"),
                           ("peak", {key: 0 for key in MEMORY_FIELDS})):
            with self.subTest(key=key, value=value):
                self.report = copy.deepcopy(original)
                self.report[key] = value
                self.save()
                with self.assertRaises(ValueError):
                    evaluate(self.out)

    def test_missing_corrupt_and_gapped_raw_samples_fail(self):
        raw = self.out / "active-resources.jsonl"
        original = raw.read_bytes()
        for contents in (b"", b"not json\n", original[:-1] + b"broken\n"):
            raw.write_bytes(contents)
            with self.assertRaises((ValueError, KeyError)):
                evaluate(self.out)
        self.samples = self.samples[:5] + self.samples[35:]
        self.save_samples()
        self.save()
        with self.assertRaisesRegex(ValueError, "unobserved gap"):
            evaluate(self.out)

    def test_required_smaps_fields_include_hugetlb_and_do_not_default_to_zero(self):
        valid = "Private_Clean: 1 kB\nPrivate_Dirty: 2 kB\nPrivate_Hugetlb: 3 kB\nPss: 7 kB\nRss: 8 kB\nSwap: 0 kB\n"
        with mock.patch.object(Path, "read_text", return_value=valid):
            self.assertEqual(memory(Path("/fixture"))["ussBytes"], 6 * 1024)
        for text in (valid.replace("Private_Dirty: 2 kB\n", ""),
                     valid.replace("Pss: 7 kB", "Pss: -1 kB"),
                     valid.replace("Rss: 8 kB", "Rss: 8 bytes")):
            with mock.patch.object(Path, "read_text", return_value=text):
                with self.assertRaises(RuntimeError):
                    memory(Path("/fixture"))


class ActiveFixtureSelectionTests(unittest.TestCase):
    def make_walk(self, ids, rows=(19,)):
        from active_walkthrough import ActiveWalk
        walk = ActiveWalk.__new__(ActiveWalk)
        walk.profile = mock.Mock(return_value=[{"json": {"id": "fixture", "name": "Fixture"}}])
        walk.text = mock.Mock(side_effect=ids)
        walk.profile_rows = mock.Mock(return_value=list(rows))
        walk.find = mock.Mock(return_value={"role": "list", "name": "Profiles"})
        walk.click = mock.Mock()
        walk.key = mock.Mock()
        return walk

    def test_selected_fixture_receives_no_reselection_input(self):
        walk = self.make_walk(["fixture"])
        walk.select_fixture("visual-fixture")
        walk.profile_rows.assert_not_called()
        walk.find.assert_not_called()
        walk.click.assert_not_called()
        walk.key.assert_not_called()

    def test_offscreen_fixture_uses_bounded_keyboard_and_checks_identity(self):
        walk = self.make_walk(["other", "fixture"])
        walk.select_fixture("fixture")
        walk.key.assert_has_calls([mock.call("Home", "fixture-list-home")] +
            [mock.call("Down", f"fixture-list-down-{index}") for index in range(19)])
        self.assertEqual(walk.key.call_count, 20)
        self.assertEqual(walk.text.call_count, 2)

    def test_wrong_final_identity_is_not_accepted(self):
        from walkthrough import Failure
        walk = self.make_walk(["other", "still-other"], rows=(0,))
        with self.assertRaisesRegex(Failure, "keyboard selection did not open"):
            walk.select_fixture("fixture")

    def test_ambiguous_profile_rows_receive_no_list_input(self):
        from walkthrough import Failure
        walk = self.make_walk(["other"], rows=(0, 1))
        with self.assertRaisesRegex(Failure, "missing or ambiguous"):
            walk.select_fixture("fixture")
        walk.find.assert_not_called()
        walk.click.assert_not_called()
        walk.key.assert_not_called()

if __name__ == "__main__":
    unittest.main()
