import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = Path(__file__).parents[1] / "desktop_evidence.py"
SPEC = importlib.util.spec_from_file_location("desktop_evidence", MODULE_PATH)
assert SPEC and SPEC.loader
subject = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = subject
SPEC.loader.exec_module(subject)
FIXTURES = Path(__file__).with_name("fixtures") / "desktop_evidence"
REPOSITORY = "Example/Project"
COMMENT_URL = "https://github.com/Example/Project/pull/7#issuecomment-99"
PARENTS = ["e" * 40, "a" * 40]


def fixture_report(name="valid-complete.md"):
    return subject.parse_report(subject.extract_report((FIXTURES / name).read_text(encoding="utf-8")))


class DesktopEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.config = subject.load_config(FIXTURES / "config.json")
        self.report = fixture_report()

    def validate(self, report=None, allowed=None, built_parents=PARENTS):
        return subject.validate_report(self.report if report is None else report,
                                       self.config, REPOSITORY, allowed, built_parents)

    def mutate(self, path, value):
        report = copy.deepcopy(self.report)
        target = report
        for part in path[:-1]:
            target = target[part]
        target[path[-1]] = value
        return report

    def test_accepts_valid_report_and_ascii_case_insensitive_repository(self):
        self.assertEqual([], self.validate(allowed={"a" * 40}))
        report = self.mutate(("project",), "eXAMPLE/pROJECT")
        self.assertEqual([], self.validate(report))
        report["environment"]["os"] = "Linux café 日本語 Ω + 👩 e\u0301"
        report["environment"].pop("gpu")
        self.assertEqual([], self.validate(report))

    def test_unknown_and_missing_keys_at_every_level(self):
        for path in ((), ("harness",), ("build",), ("environment",), ("results", 0)):
            for action in ("unknown", "missing"):
                with self.subTest(path=path, action=action):
                    report = copy.deepcopy(self.report)
                    target = report
                    for part in path:
                        target = target[part]
                    if action == "unknown":
                        target["extra"] = "not accepted"
                    else:
                        del target[next(iter(target))]
                    self.assertTrue(self.validate(report))
        local = self.mutate(("build",), {"source": "local-build", "builtCommit": "a" * 40, "dirty": False})
        self.assertEqual([], self.validate(local))
        local["build"]["ciRunId"] = 1
        self.assertTrue(self.validate(local))
        del local["build"]["ciRunId"]
        del local["build"]["dirty"]
        self.assertTrue(self.validate(local))

    def test_exact_types_and_required_objects(self):
        for path in (("schema",), ("project",), ("commit",), ("recordedAt",),
                     ("harness",), ("build",), ("environment",), ("results",),
                     ("harness", "id"), ("harness", "version"),
                     ("environment", "session"), ("environment", "machine"),
                     ("environment", "os"), ("environment", "gpu"),
                     ("build", "source"), ("build", "builtCommit"), ("build", "identitySha256"),
                     ("results", 0), ("results", 0, "id"), ("results", 0, "status"),
                     ("results", 0, "detail")):
            for value in (None, True, 1, [], {}):
                with self.subTest(path=path, value=value):
                    self.assertTrue(self.validate(self.mutate(path, value)))
        self.assertTrue(self.validate([]))

    def test_ci_run_id_integer_bounds_and_local_dirty_boolean(self):
        for value in (True, False, 0, -1, 2**53, 1.0, "1", None, [], {}):
            with self.subTest(value=value):
                self.assertTrue(self.validate(self.mutate(("build", "ciRunId"), value)))
        for value in (1, 2**53 - 1):
            self.assertEqual([], self.validate(self.mutate(("build", "ciRunId"), value)))
        for value in (True, False, 0, 1, "false", None):
            build = {"source": "local-build", "builtCommit": "a" * 40, "dirty": value}
            self.assertEqual(type(value) is not bool, bool(self.validate(self.mutate(("build",), build))))

    def test_build_commit_attribution_and_verified_merge_suffix(self):
        problem = ("build.builtCommit must equal commit or be its pull request merge commit "
                   "(two parents, the second being commit)")
        self.assertEqual([problem + " ; pass --built-commit-parents to verify a merge build"],
                         self.validate(built_parents=None))
        for parents in ([], ["a" * 40], ["a" * 40, "e" * 40], ["e" * 40, "b" * 40],
                        ["bad", "a" * 40]):
            with self.subTest(parents=parents):
                self.assertEqual([problem], self.validate(built_parents=parents))
        self.assertEqual([], self.validate(built_parents=PARENTS))
        payload = subject.check_run_payload(self.report, self.config, REPOSITORY, "99", COMMENT_URL,
                                            "tester", "OWNER", PARENTS)
        self.assertIn(" (pull request merge of `" + "a" * 40 + "` into `" + "e" * 40 +
                      "`; parents read from GitHub)", payload["output"]["summary"])
        equal = self.mutate(("build", "builtCommit"), "a" * 40)
        self.assertEqual([], self.validate(equal, built_parents=None))
        payload = subject.check_run_payload(equal, self.config, REPOSITORY, "99", COMMENT_URL,
                                            "tester", "OWNER", PARENTS)
        self.assertNotIn("parents read", payload["output"]["summary"])
        local = self.mutate(("build",), {"source": "local-build", "builtCommit": "c" * 40, "dirty": False})
        self.assertEqual(["build.builtCommit must equal commit for a local build"], self.validate(local))

    def test_render_comment_enforces_pretty_json_and_comment_limits(self):
        report = self.mutate(("results",), [{"id": "test-" + str(index), "status": "pass", "detail": "x" * 190}
                                           for index in range(64)])
        self.assertLessEqual(len(json.dumps(report, separators=(",", ":")).encode("utf-8")), subject.MAX_REPORT_BYTES)
        with self.assertRaisesRegex(subject.ReportError, "report exceeds 16384 UTF-8 bytes"):
            subject.render_comment(report, self.config)
        with mock.patch.object(subject, "MAX_COMMENT_CHARS", 100):
            with self.assertRaisesRegex(subject.ReportError, "comment exceeds 65536 characters"):
                subject.render_comment(self.report, self.config)

    def test_patterns_and_known_harness_members(self):
        cases = [(("schema",), "other/1"), (("project",), "Other/Project"),
                 (("project",), "Example/Project\n"), (("project",), "é/Project"),
                 (("project",), "a" * 40 + "/repo"), (("project",), "owner/" + "a" * 101),
                 (("commit",), "A" * 40), (("commit",), "a" * 39),
                 (("commit",), "g" * 40), (("commit",), "a" * 40 + "\n"),
                 (("harness", "id"), "unknown"), (("harness", "id"), "Demo"),
                 (("harness", "version"), "v_1"), (("harness", "version"), "v" * 33),
                 (("harness", "version"), ".1"), (("harness", "version"), ""),
                 (("environment", "session"), "unknown"), (("environment", "session"), "UPPER"),
                 (("environment", "machine"), "container"), (("build", "source"), "unknown"),
                 (("build", "builtCommit"), "C" * 40), (("build", "identitySha256"), "D" * 64),
                 (("build", "identitySha256"), "d" * 63), (("results", 0, "id"), "unknown"),
                 (("results", 0, "id"), "-capture"), (("results", 0, "id"), "x" * 65),
                 (("results", 0, "status"), "passed")]
        for path, value in cases:
            with self.subTest(path=path, value=value):
                self.assertTrue(self.validate(self.mutate(path, value)))
        self.assertEqual([], self.validate(self.mutate(("harness", "version"), "v1.0+local-2")))
        self.config["harnesses"]["demo-desktop"]["builds"] = ["local-build"]
        self.assertTrue(self.validate())
        self.config["harnesses"]["demo-desktop"]["machines"] = ["vm"]
        self.assertTrue(self.validate())

    def test_allowed_commit_and_real_dates(self):
        self.assertTrue(self.validate(allowed={"b" * 40}))
        for date in ("2026-02-30T12:34:56Z", "2026-13-01T12:34:56Z", "2026-01-01T24:00:00Z",
                     "2026-01-01T00:00:60Z", "2026-1-01T00:00:00Z", "2026-01-01T00:00:00+00:00",
                     "2026-01-01T00:00:00Z\n", "0000-01-01T00:00:00Z", "2026-01-01"):
            with self.subTest(date=date):
                self.assertTrue(self.validate(self.mutate(("recordedAt",), date)))
        self.assertEqual([], self.validate(self.mutate(("recordedAt",), "2024-02-29T23:59:59Z")))

    def test_text_limits_and_injection_characters(self):
        for path, maximum in ((("environment", "os"), 80), (("environment", "gpu"), 80),
                              (("results", 0, "detail"), 200)):
            self.assertEqual([], self.validate(self.mutate(path, "x" * maximum)))
            for value in ("", "x" * (maximum + 1), " leading", "trailing ", " "):
                with self.subTest(path=path, value=value):
                    self.assertTrue(self.validate(self.mutate(path, value)))
            for char in ("`", "|", "<", ">", "\n", "\r", "\t", "\x00", "\x1b", "\u202e", "\u200b",
                         "\ud800", "\ue000", "\u0378", "\u00a0", "\u2028", "\u2029", "\u2003"):
                with self.subTest(path=path, char=repr(char)):
                    self.assertTrue(self.validate(self.mutate(path, "before" + char + "after")))
        self.assertEqual([], self.validate(self.mutate(("environment", "os"), "two  spaces")))
        self.assertTrue(self.validate(fixture_report("injection.md")))

    def test_result_list_bounds_duplicates_and_statuses(self):
        for results in ([], self.report["results"] * 22, [self.report["results"][0]] * 2):
            self.assertTrue(self.validate(self.mutate(("results",), results)))
        harness = self.config["harnesses"]["demo-desktop"]
        harness["required"] = ["test-" + str(index) for index in range(64)]
        results = [{"id": test_id, "status": "pass"} for test_id in harness["required"]]
        self.assertEqual([], self.validate(self.mutate(("results",), results)))
        results.append({"id": "extra", "status": "pass"})
        self.assertTrue(self.validate(self.mutate(("results",), results)))
        harness["required"] = ["capture", "overlay"]
        for status in subject.STATUSES:
            self.assertEqual([], self.validate(self.mutate(("results", 0, "status"), status)))

    def test_evaluation_failure_precedence_missing_required_and_allowed_unavailability(self):
        harness = self.config["harnesses"]["demo-desktop"]
        evaluation, counts, notes = subject.evaluate(self.report, harness)
        self.assertEqual(("complete", {"passed": 2, "failed": 0, "skipped": 1, "unavailable": 0}, []),
                         (evaluation, counts, notes))
        report = self.mutate(("results",), [self.report["results"][0]])
        self.assertEqual(("incomplete", ["required `overlay` is missing"]),
                         (subject.evaluate(report, harness)[0], subject.evaluate(report, harness)[2]))
        for status in ("skip", "unavailable"):
            report = self.mutate(("results", 0, "status"), status)
            self.assertEqual("incomplete", subject.evaluate(report, harness)[0])
            self.assertIn(f"required `capture` is `{status}`", subject.evaluate(report, harness)[2])
            report = self.mutate(("results", 1, "status"), status)
            self.assertEqual("complete", subject.evaluate(report, harness)[0])
            self.assertEqual([f"`overlay` is `{status}`: Overlay capture is not supported on this session."],
                             subject.evaluate(report, harness)[2])
            report["results"][0]["status"] = "fail"
            self.assertEqual("failed", subject.evaluate(report, harness)[0])
        report = self.mutate(("results", 2, "status"), "fail")
        self.assertEqual("failed", subject.evaluate(report, harness)[0])
        report["results"].pop(0)
        self.assertEqual("failed", subject.evaluate(report, harness)[0])

    def test_associations(self):
        for association in ("OWNER", "MEMBER", "COLLABORATOR"):
            self.assertEqual("maintainer-attested", subject.trust_tier(association))
        for association in ("CONTRIBUTOR", "FIRST_TIME_CONTRIBUTOR", "FIRST_TIMER", "MANNEQUIN", "NONE"):
            self.assertEqual("unverified", subject.trust_tier(association))
        for association in ("owner", "BOT", "", None):
            with self.subTest(association=association), self.assertRaises(subject.ReportError):
                subject.trust_tier(association)

    def test_goldens(self):
        for fixture, association, golden in (("valid-complete.md", "OWNER", "owner-pass.json"),
                                             ("valid-failed.md", "OWNER", "owner-fail.json"),
                                             ("valid-complete.md", "CONTRIBUTOR", "contributor-neutral.json")):
            with self.subTest(golden=golden):
                payload = subject.check_run_payload(fixture_report(fixture), self.config, REPOSITORY,
                                                    "99", COMMENT_URL, "tester", association, PARENTS)
                expected = json.loads((FIXTURES / "expected" / golden).read_text(encoding="utf-8"))
                self.assertEqual(expected, payload)

    def test_local_build_payload_and_notes(self):
        report = self.mutate(("build",), {"source": "local-build", "builtCommit": "a" * 40, "dirty": True})
        report["results"][0]["status"] = "skip"
        report["results"].pop(1)
        del report["environment"]["gpu"]
        payload = subject.check_run_payload(report, self.config, REPOSITORY, "99", COMMENT_URL, "tester", "MEMBER")
        self.assertEqual("failure", payload["conclusion"])
        self.assertIn("dirty tree (reported by the submitter, not verified)", payload["output"]["summary"])
        self.assertIn("  - required `overlay` is missing", payload["output"]["summary"])
        self.assertIn("  - required `capture` is `skip`", payload["output"]["summary"])
        self.assertNotIn("differs", payload["output"]["summary"])
        self.assertNotIn("GPU", payload["output"]["summary"])

    def test_render_comment_round_trip_and_canonical_digest(self):
        self.report["environment"]["os"] = "Linux 日本語"
        comment = subject.render_comment(self.report, self.config)
        parsed = subject.parse_report(subject.extract_report(comment))
        self.assertEqual(self.report, parsed)
        self.assertEqual([], self.validate(parsed))
        self.assertNotIn("LayerC", comment)
        canonical = json.dumps(self.report, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        self.assertEqual(hashlib.sha256(canonical.encode("utf-8")).hexdigest(), subject.canonical_digest(parsed))
        self.assertEqual(subject.canonical_digest(self.report), subject.canonical_digest(dict(reversed(list(self.report.items())))))

    def test_extract_normalizes_newlines_and_ignores_free_text(self):
        body = (FIXTURES / "valid-complete.md").read_text(encoding="utf-8")
        for newline in ("\n", "\r\n", "\r"):
            self.assertEqual(self.report, subject.parse_report(subject.extract_report(body.replace("\n", newline))))
        self.assertEqual(self.report, subject.parse_report(subject.extract_report(body + subject.MARKER + "\nfree text\n```\n")))

    def test_extract_rejects_marker_fences_and_limits(self):
        body = (FIXTURES / "valid-complete.md").read_text(encoding="utf-8")
        cases = [body.replace(subject.MARKER, "", 1), "prefix\n" + body,
                 body.replace(subject.MARKER, subject.MARKER + " extra", 1),
                 body.replace("```json", "```JSON"), body + "```json\n{}\n```\n",
                 body.rsplit("```", 1)[0], body + "x" * subject.MAX_COMMENT_CHARS,
                 subject.MARKER + '\n```json\n' + ' ' * (subject.MAX_REPORT_BYTES + 1) + '\n```',
                 subject.MARKER + '\n```json\n' + 'é' * (subject.MAX_REPORT_BYTES // 2 + 1) + '\n```',
                 subject.MARKER + '\n```json\n"\ud800"\n```']
        for index, case in enumerate(cases):
            with self.subTest(index=index), self.assertRaises(subject.ReportError):
                subject.extract_report(case)

    def test_json_rejects_duplicates_nonfinite_nonobjects_and_deep_nesting(self):
        for text in ('{"x": 1, "x": 2}', '{"x": {"y": 1, "y": 2}}', '{"x": NaN}',
                     '{"x": Infinity}', '{"x": -Infinity}', '[]', 'true', 'null', '"text"',
                     '{"x":' + '[' * 10000 + '0' + ']' * 10000 + '}'):
            with self.subTest(text=text[:60]), self.assertRaises(subject.ReportError):
                subject.parse_report(text)


class ConfigTests(unittest.TestCase):
    def test_real_configuration_loads(self):
        config = subject.load_config(Path(__file__).parents[2] / ".github/desktop-evidence.json")
        self.assertEqual(3, len(config["harnesses"]))

    def test_invalid_configuration(self):
        original = subject.load_config(FIXTURES / "config.json")
        cases = [[], {"schema": "wrong", "harnesses": {}},
                 {**original, "extra": 1}, {"schema": subject.CONFIG_SCHEMA, "harnesses": {}},
                 {"schema": subject.CONFIG_SCHEMA, "harnesses": {"INVALID": {}}}]
        for field, value in (("extra", 1), ("description", "`unsafe`"), ("description", "x" * 201),
                             ("description", ""), ("description", " padded "),
                             ("sessions", []), ("sessions", ["x", "x"]), ("sessions", ["UPPER"]),
                             ("sessions", "x"), ("sessions", [None]),
                             ("machines", []), ("machines", ["container"]), ("machines", ["vm", "vm"]),
                             ("builds", []), ("builds", ["other"]), ("required", ["x", "x"]),
                             ("required", ["x" * 65]), ("required", ["latency"]),
                             ("optional", ["capture"]), ("optional", ["x", "x"]),
                             ("allowUnavailable", {"latency": "optional"}),
                             ("allowUnavailable", {"overlay": "x" * 201}),
                             ("allowUnavailable", {"overlay": "bad\nreason"}), ("allowUnavailable", [])):
            config = copy.deepcopy(original)
            config["harnesses"]["demo-desktop"][field] = value
            cases.append(config)
        missing = copy.deepcopy(original)
        del missing["harnesses"]["demo-desktop"]["optional"]
        cases.append(missing)
        empty = copy.deepcopy(original)
        empty["harnesses"]["demo-desktop"].update(required=[], optional=[], allowUnavailable={})
        cases.append(empty)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for index, config in enumerate(cases):
                with self.subTest(index=index), self.assertRaises(subject.ConfigError):
                    path.write_text(json.dumps(config), encoding="utf-8")
                    subject.load_config(path)
            for content in ('{', '{"schema": 1, "schema": 2}', '{"value": NaN}'):
                path.write_text(content, encoding="utf-8")
                with self.assertRaises(subject.ConfigError):
                    subject.load_config(path)


class DesktopEvidenceCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.out = self.root / "out.json"
        self.summary = self.root / "summary.md"
        self.environment = {**os.environ, "GITHUB_STEP_SUMMARY": str(self.summary)}

    def tearDown(self):
        self.directory.cleanup()

    def run_cli(self, command, *arguments, environment=None, parents=True):
        options = [sys.executable, str(MODULE_PATH), command]
        if command != "built-commit":
            options += ["--config", str(FIXTURES / "config.json"), "--repository", REPOSITORY]
            if parents:
                options += ["--built-commit-parents", str(FIXTURES / "built-commit-parents.txt")]
        return subprocess.run([*options, *map(str, arguments)],
                              env=self.environment if environment is None else environment,
                              capture_output=True, text=True, encoding="utf-8")

    def publish(self, fixture="valid-complete.md", *arguments, environment=None, parents=True):
        return self.run_cli("publish", "--comment-file", FIXTURES / fixture,
                            "--comment-id", "99", "--comment-url", COMMENT_URL,
                            "--author", "tester", "--association", "OWNER",
                            "--allowed-commits", FIXTURES / "allowed-commits.txt",
                            "--out", self.out, *arguments, environment=environment, parents=parents)

    def test_publish_writes_exact_payload_and_appends_summary(self):
        self.summary.write_text("Existing summary\n", encoding="utf-8")
        result = self.publish()
        self.assertEqual(0, result.returncode, result.stderr)
        expected = json.loads((FIXTURES / "expected/owner-pass.json").read_text(encoding="utf-8"))
        self.assertEqual(expected, json.loads(self.out.read_text(encoding="utf-8")))
        summary = self.summary.read_text(encoding="utf-8")
        self.assertTrue(summary.startswith("Existing summary\n"))
        self.assertIn(expected["output"]["title"], summary)
        self.assertIn(expected["name"], summary)
        self.assertTrue(self.out.read_bytes().endswith(b"\n"))

    def test_rejected_fixtures_write_no_payload_and_summary_has_reasons(self):
        for name in ("malformed.md", "wrong-commit.md", "unknown-field.md", "injection.md"):
            with self.subTest(name=name):
                result = self.publish(name)
                self.assertEqual(2, result.returncode, result.stderr)
                self.assertFalse(self.out.exists())
                self.assertIn("rejected:", result.stderr)
        self.assertIn("### Desktop evidence report rejected", self.summary.read_text(encoding="utf-8"))
        hostile = self.root / "hostile.md"
        hostile.write_text(subject.MARKER + '\n```json\n{"`<script>|\\u202e":1,"`<script>|\\u202e":2}\n```', encoding="utf-8")
        result = self.publish(str(hostile))
        self.assertEqual(2, result.returncode)
        self.assertNotIn("<script>", self.summary.read_text(encoding="utf-8"))

    def test_publish_reads_environment_and_missing_variable_is_usage_error(self):
        body = (FIXTURES / "valid-complete.md").read_text(encoding="utf-8")
        arguments = ("--comment-env", "DESKTOP_TEST_COMMENT", "--comment-id", "99", "--comment-url", COMMENT_URL,
                     "--author", "tester", "--association", "CONTRIBUTOR", "--allowed-commits",
                     FIXTURES / "allowed-commits.txt", "--out", "-")
        result = self.run_cli("publish", *arguments, environment={**self.environment, "DESKTOP_TEST_COMMENT": body})
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("neutral", json.loads(result.stdout)["conclusion"])
        environment = dict(self.environment)
        environment.pop("DESKTOP_TEST_COMMENT", None)
        self.assertEqual(1, self.run_cli("publish", *arguments, environment=environment).returncode)

    def test_publish_rejects_bad_metadata(self):
        for key, value in (("comment-id", "0"), ("comment-id", "1" * 21), ("comment-id", "-1"),
                           ("author", "bad_login"), ("author", "-user"), ("author", "x" * 40),
                           ("association", "BOT"), ("comment-url", "https://evil.example/pull/7"),
                           ("comment-url", "https://github.com/Other/Project/pull/7"),
                           ("comment-url", COMMENT_URL + "?query=1"),
                           ("comment-url", COMMENT_URL.replace("/pull/", "/PULL/")),
                           ("comment-url", "https://github.com/Example/Project/pull/7\n")):
            with self.subTest(key=key, value=value):
                result = self.publish("valid-complete.md", "--" + key + "=" + value)
                self.assertEqual(2, result.returncode, result.stderr)
                self.assertFalse(self.out.exists())
        result = self.publish("valid-complete.md", "--comment-url", COMMENT_URL.replace("Example/Project", "example/project"))
        self.assertEqual(0, result.returncode, result.stderr)

    def test_allowed_commit_file_errors_and_normalization(self):
        allowed = self.root / "allowed.txt"
        allowed.write_text("\n " + "A" * 40 + " \n\n", encoding="utf-8")
        result = self.publish("valid-complete.md", "--allowed-commits", allowed)
        self.assertEqual(0, result.returncode, result.stderr)
        for value in ("bad", "g" * 40, "a" * 39):
            allowed.write_text(value, encoding="utf-8")
            self.assertEqual(1, self.publish("valid-complete.md", "--allowed-commits", allowed).returncode)

    def test_publish_requires_merge_parents_and_parent_file_validation(self):
        result = self.publish(parents=False)
        self.assertEqual(2, result.returncode, result.stderr)
        self.assertIn("pass --built-commit-parents", result.stderr)
        self.assertFalse(self.out.exists())
        path = self.root / "parents.txt"
        for content in ("e" * 40 + "\n" + "a" * 40 + "\n" + "b" * 40, "bad\n", "g" * 40):
            path.write_text(content, encoding="utf-8")
            for command in ("validate", "comment", "publish"):
                with self.subTest(content=content, command=command):
                    if command == "publish":
                        result = self.publish("valid-complete.md", "--built-commit-parents", path)
                    else:
                        report = self.root / "report.json"
                        report.write_text(json.dumps(fixture_report()), encoding="utf-8")
                        result = self.run_cli(command, "--report", report, "--built-commit-parents", path)
                    self.assertEqual(1, result.returncode, result.stderr)
        path.write_text("\n", encoding="utf-8")
        self.assertEqual([], subject.load_built_commit_parents(path))
        self.assertEqual(2, self.publish("valid-complete.md", "--built-commit-parents", path).returncode)
        path.write_text(" E" + "E" * 39 + " \n\n " + "A" * 40 + " \n", encoding="utf-8")
        self.assertEqual(PARENTS, subject.load_built_commit_parents(path))
        self.assertEqual(0, self.publish("valid-complete.md", "--built-commit-parents", path).returncode)

    def test_built_commit_prints_only_a_valid_differing_sha(self):
        def run(body):
            return self.run_cli("built-commit", "--comment-env", "DESKTOP_BUILT_COMMENT",
                                environment={**self.environment, "DESKTOP_BUILT_COMMENT": body})

        result = run((FIXTURES / "valid-complete.md").read_text(encoding="utf-8"))
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("c" * 40 + "\n", result.stdout)
        reports = [{"commit": "a" * 40, "build": {"builtCommit": "a" * 40}},
                   {"commit": "bad", "build": {"builtCommit": "c" * 40}},
                   {"commit": "a" * 40, "build": {"builtCommit": "C" * 40}},
                   {"commit": "a" * 40, "build": {"builtCommit": "c" * 40 + "`|<script>"}},
                   {"commit": "a" * 40 + "\n", "build": {"builtCommit": "c" * 40}},
                   {"commit": "a" * 40, "build": []}, {}, []]
        hostile = [(FIXTURES / name).read_text(encoding="utf-8") for name in ("malformed.md", "injection.md")]
        hostile += ["missing marker", "<script>`|\u202e"]
        hostile += [subject.MARKER + "\n```json\n" + json.dumps(report) + "\n```" for report in reports]
        hostile += [subject.MARKER + '\n```json\n{"deep":' + '[' * 7000 + '0' + ']' * 7000 + '}\n```']
        for body in hostile:
            result = run(body)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual("", result.stdout)
            self.assertEqual("", result.stderr)
        environment = dict(self.environment)
        environment.pop("DESKTOP_BUILT_COMMENT", None)
        result = self.run_cli("built-commit", "--comment-env", "DESKTOP_BUILT_COMMENT", environment=environment)
        self.assertEqual(1, result.returncode)
        self.assertEqual("", result.stdout)

    def test_report_files_enforce_raw_byte_limit_before_decoding(self):
        report = self.root / "large.json"
        for content in (b" " * 16385, b"\xff" * 20000, ("é" * 8193).encode("utf-8")):
            report.write_bytes(content)
            for command in ("validate", "comment"):
                arguments = ("--out", self.out) if command == "comment" else ()
                result = self.run_cli(command, "--report", report, *arguments)
                self.assertEqual(2, result.returncode, result.stderr)
                self.assertIn("report exceeds 16384 UTF-8 bytes", result.stderr)
                self.assertFalse(self.out.exists())

    def test_comment_rejects_compact_report_whose_pretty_json_is_too_large(self):
        config = subject.load_config(FIXTURES / "config.json")
        harness = config["harnesses"]["demo-desktop"]
        harness.update(required=["test-" + str(index) for index in range(64)], optional=[], allowUnavailable={})
        config_path = self.root / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        report = fixture_report()
        report["results"] = [{"id": test_id, "status": "pass", "detail": "x" * 190} for test_id in harness["required"]]
        text = json.dumps(report, separators=(",", ":"), ensure_ascii=False)
        self.assertLessEqual(len(text.encode("utf-8")), subject.MAX_REPORT_BYTES)
        self.assertGreater(len(json.dumps(report, indent=2).encode("utf-8")), subject.MAX_REPORT_BYTES)
        path = self.root / "compact.json"
        path.write_text(text, encoding="utf-8")
        result = self.run_cli("validate", "--config", config_path, "--report", path)
        self.assertEqual(0, result.returncode, result.stderr)
        result = self.run_cli("comment", "--config", config_path, "--report", path, "--out", self.out)
        self.assertEqual(2, result.returncode, result.stderr)
        self.assertIn("report exceeds 16384 UTF-8 bytes", result.stderr)
        self.assertFalse(self.out.exists())

    def test_validate_comment_and_report_and_render(self):
        report = self.root / "report.json"
        report.write_text(json.dumps(fixture_report()), encoding="utf-8")
        for option, path in (("--report", report), ("--comment-file", FIXTURES / "valid-complete.md")):
            result = self.run_cli("validate", option, path)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual("valid: " + subject.canonical_digest(fixture_report()), result.stdout.strip())
        result = self.run_cli("comment", "--report", report, "--out", self.out)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(fixture_report(), subject.parse_report(subject.extract_report(self.out.read_text(encoding="utf-8"))))
        self.assertEqual(1, self.run_cli("validate").returncode)
        self.assertEqual(1, self.run_cli("validate", "--repository", "not-a-repository", "--report", report).returncode)
        self.assertEqual(1, self.run_cli("validate", "--config", self.root / "missing", "--report", report).returncode)
        report.write_text('{"nested":' + '[' * 10000 + '0' + ']' * 10000 + '}', encoding="utf-8")
        self.assertEqual(2, self.run_cli("validate", "--report", report).returncode)


if __name__ == "__main__":
    unittest.main()
