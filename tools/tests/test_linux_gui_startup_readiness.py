"""Readiness transport controls: no actual processes, display, bus or production data."""
import errno
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "linux-gui"))
from startup_readiness import announced, verify_report


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "startup.json"
        self.identity = (12345, "67890", "a" * 32)
        self.good = dict(passed=True, applicationPid=self.identity[0],
                         applicationStartTime=self.identity[1], nonce=self.identity[2])

    def publish(self, report):
        self.path.write_text(json.dumps(report), encoding="utf-8")

    def test_missing_log_is_not_ready(self):
        self.assertFalse(announced(self.path, {}))

    def test_exact_ready_line(self):
        for text, expected in (("", False), ("already ready\n", False),
                               ("unready\n", False), ("start\nready\n", True)):
            with self.subTest(text=text):
                self.path.write_text(text, encoding="utf-8")
                self.assertIs(announced(self.path, {}), expected)

    def test_no_data_then_ready_retains_retry(self):
        log = Mock()
        log.read_text.side_effect = [OSError(errno.ENODATA, "No data available"), "ready\n"]
        report = {"sampleCount": 35, "peakUssKiB": 28064}
        self.assertFalse(announced(log, report))
        self.assertNotIn("passed", report)
        self.assertTrue(announced(log, report))
        self.assertEqual(report, {"sampleCount": 35, "peakUssKiB": 28064,
                                  "readinessLogNoDataRetries": 1})

    def test_repeated_no_data_never_announces_readiness(self):
        log = Mock()
        log.read_text.side_effect = OSError(errno.ENODATA, "No data available")
        report = {}
        for _ in range(3):
            self.assertFalse(announced(log, report))
        self.assertEqual(report, {"readinessLogNoDataRetries": 3})

    def test_other_io_errors_are_not_retried(self):
        for code in (errno.EIO, errno.EACCES):
            with self.subTest(code=code):
                log = Mock()
                log.read_text.side_effect = OSError(code, "not transient ENODATA")
                report = {}
                with self.assertRaises(OSError):
                    announced(log, report)
                self.assertEqual(report, {})

    def test_good_report(self):
        self.publish(self.good)
        verify_report(self.path, *self.identity)

    def test_failed_report_is_not_a_ready_file(self):
        self.publish(dict(self.good, passed=False, error="read failed"))
        with self.assertRaisesRegex(RuntimeError, "read failed"):
            verify_report(self.path, *self.identity)

    def test_true_is_required_not_a_truthy_value(self):
        for value in (1, "true", None):
            with self.subTest(value=value):
                self.publish(dict(self.good, passed=value))
                with self.assertRaises(RuntimeError):
                    verify_report(self.path, *self.identity)

    def test_each_identity_component_is_required(self):
        for key, value in (("applicationPid", 54321), ("applicationStartTime", "67891"),
                           ("nonce", "b" * 32)):
            with self.subTest(key=key):
                self.publish(dict(self.good, **{key: value}))
                with self.assertRaisesRegex(ValueError, "identity mismatch"):
                    verify_report(self.path, *self.identity)
                missing = dict(self.good)
                del missing[key]
                self.publish(missing)
                with self.assertRaises(ValueError):
                    verify_report(self.path, *self.identity)

    def test_missing_report_fails_closed(self):
        with self.assertRaises(FileNotFoundError):
            verify_report(self.path, *self.identity)

    def test_malformed_report_fails_closed(self):
        self.path.write_text("{", encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            verify_report(self.path, *self.identity)


if __name__ == "__main__":
    unittest.main()
