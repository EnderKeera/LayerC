#!/usr/bin/env python3
"""Validate desktop reports and turn attested comments into advisory checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import unicodedata
from datetime import datetime
from pathlib import Path

MARKER = "<!-- desktop-evidence/1 -->"
SCHEMA = "desktop-evidence/1"
CONFIG_SCHEMA = "desktop-evidence-config/1"
MAX_COMMENT_CHARS = 65536
MAX_REPORT_BYTES = 16384
MAX_RESULTS = 64

ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,31}")
COMMIT = re.compile(r"[0-9a-f]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")
PROJECT = re.compile(r"[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}")
RECORDED_AT = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z")
STATUSES = ("pass", "fail", "skip", "unavailable")


class ConfigError(ValueError):
    """The trusted harness configuration is invalid."""


class ReportError(ValueError):
    """An untrusted report or its comment metadata was rejected."""


class UsageError(ValueError):
    """Invalid command-line arguments."""


def text_character(character: str) -> bool:
    return (character not in "`|<>" and
            (character == " " or unicodedata.category(character)[0] in "LMNPS"))


def valid_text(value: object, maximum: int = 80) -> bool:
    return (type(value) is str and 1 <= len(value) <= maximum and
            value == value.strip(" ") and all(text_character(char) for char in value))


def matches(pattern: re.Pattern, value: object) -> bool:
    return type(value) is str and pattern.fullmatch(value) is not None


def _object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _constant(value: str) -> None:
    raise ValueError("non-finite JSON number")


def parse_report(text: str) -> dict:
    try:
        report = json.loads(text, object_pairs_hook=_object, parse_constant=_constant)
    except (ValueError, RecursionError) as error:
        raise ReportError(f"invalid JSON: {error}") from error
    if type(report) is not dict:
        raise ReportError("report must be a JSON object")
    return report


def _keys(value: object, required: set, optional: frozenset = frozenset()) -> bool:
    return type(value) is dict and required <= value.keys() <= required | optional


def load_config(path: Path) -> dict:
    try:
        config = parse_report(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ReportError) as error:
        raise ConfigError(f"could not load configuration: {error}") from error
    if not _keys(config, {"schema", "harnesses"}) or config["schema"] != CONFIG_SCHEMA:
        raise ConfigError("configuration must have schema desktop-evidence-config/1 and harnesses")
    harnesses = config["harnesses"]
    if type(harnesses) is not dict or not harnesses:
        raise ConfigError("configuration harnesses must be a non-empty object")
    fields = {"description", "sessions", "machines", "builds", "required", "optional",
              "allowUnavailable"}
    for name, harness in harnesses.items():
        if not matches(ID, name) or not _keys(harness, fields):
            raise ConfigError("each harness must have a valid id and exactly the configured fields")
        if not valid_text(harness["description"], 200):
            raise ConfigError(f"harness {name}: invalid description")
        for field in ("sessions", "machines", "builds", "required", "optional"):
            values = harness[field]
            if (type(values) is not list or not all(matches(ID, item) for item in values)
                    or len(values) != len(set(values))):
                raise ConfigError(f"harness {name}: {field} must be a unique id list")
            if field in ("sessions", "machines", "builds") and not values:
                raise ConfigError(f"harness {name}: {field} must not be empty")
        if not set(harness["machines"]) <= {"physical", "vm"}:
            raise ConfigError(f"harness {name}: unknown machine type")
        if not set(harness["builds"]) <= {"ci-artifact", "local-build"}:
            raise ConfigError(f"harness {name}: unknown build source")
        required, optional = set(harness["required"]), set(harness["optional"])
        if required & optional or not required | optional:
            raise ConfigError(f"harness {name}: tests must be non-empty and disjoint")
        allowed = harness["allowUnavailable"]
        if (type(allowed) is not dict or not allowed.keys() <= required or
                not all(valid_text(reason, 200) for reason in allowed.values())):
            raise ConfigError(f"harness {name}: invalid allowUnavailable entries")
    return config


def extract_report(body: str) -> str:
    body = body.replace("\r\n", "\n").replace("\r", "\n")
    if len(body) > MAX_COMMENT_CHARS:
        raise ReportError("comment exceeds 65536 characters")
    lines = body.split("\n")
    if lines[0] != MARKER:
        raise ReportError("comment must start with the exact desktop-evidence marker line")
    openers = [index for index, line in enumerate(lines) if line == "```json"]
    if len(openers) != 1:
        raise ReportError("comment must contain exactly one JSON fence")
    start = openers[0] + 1
    end = next((index for index in range(start, len(lines)) if lines[index] == "```"), None)
    if end is None:
        raise ReportError("JSON fence is not closed")
    text = "\n".join(lines[start:end])
    try:
        size = len(text.encode("utf-8"))
    except UnicodeError as error:
        raise ReportError("report is not valid UTF-8 text") from error
    if size > MAX_REPORT_BYTES:
        raise ReportError("report exceeds 16384 UTF-8 bytes")
    return text


def validate_report(report: dict, config: dict, repository: str,
                    allowed_commits: set | None = None) -> list[str]:
    problems = []

    def keys(value: object, required: set, path: str,
             optional: frozenset = frozenset()) -> bool:
        if not _keys(value, required, optional):
            problems.append(f"{path} has missing or unknown keys, or is not an object")
        return type(value) is dict

    def pattern(value: object, expression: re.Pattern, path: str) -> None:
        if not matches(expression, value):
            problems.append(f"{path} has an invalid type or format")

    def text(value: object, path: str, maximum: int = 80) -> None:
        if not valid_text(value, maximum):
            problems.append(f"{path} must be safe text of 1..{maximum} code points")

    if not keys(report, {"schema", "project", "commit", "recordedAt", "harness", "build",
                         "environment", "results"}, "report"):
        return problems
    if report.get("schema") != SCHEMA:
        problems.append("schema must be desktop-evidence/1")
    project = report.get("project")
    pattern(project, PROJECT, "project")
    if not matches(PROJECT, repository) or not matches(PROJECT, project) or project.lower() != repository.lower():
        problems.append("project does not match the receiving repository")
    commit = report.get("commit")
    pattern(commit, COMMIT, "commit")
    if allowed_commits is not None and (type(commit) is not str or commit not in allowed_commits):
        problems.append("commit is not in the allowed pull request commits")
    recorded_at = report.get("recordedAt")
    if matches(RECORDED_AT, recorded_at):
        try:
            datetime.strptime(recorded_at, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError:
            problems.append("recordedAt is not a valid UTC date and time")
    else:
        problems.append("recordedAt must be UTC YYYY-MM-DDTHH:MM:SSZ")
    harness = None
    harness_info = report.get("harness")
    if keys(harness_info, {"id", "version"}, "harness"):
        harness_id = harness_info.get("id")
        pattern(harness_id, ID, "harness.id")
        pattern(harness_info.get("version"), VERSION, "harness.version")
        if type(harness_id) is str:
            harness = config["harnesses"].get(harness_id)
        if harness is None:
            problems.append("harness.id is not configured")
    build = report.get("build")
    if type(build) is not dict:
        problems.append("build must be an object")
    else:
        source = build.get("source")
        if source == "ci-artifact":
            keys(build, {"source", "ciRunId", "builtCommit", "identitySha256"}, "build")
            run = build.get("ciRunId")
            if type(run) is not int or not 1 <= run <= 2**53 - 1:
                problems.append("build.ciRunId must be an integer in 1..2^53-1")
            pattern(build.get("identitySha256"), SHA256, "build.identitySha256")
        elif source == "local-build":
            keys(build, {"source", "builtCommit", "dirty"}, "build")
            if type(build.get("dirty")) is not bool:
                problems.append("build.dirty must be a boolean")
        else:
            problems.append("build.source is unknown")
        pattern(build.get("builtCommit"), COMMIT, "build.builtCommit")
        if harness is not None and source not in harness["builds"]:
            problems.append("build.source is not allowed by the harness")
    environment = report.get("environment")
    if keys(environment, {"os", "session", "machine"}, "environment", {"gpu"}):
        text(environment.get("os"), "environment.os")
        if "gpu" in environment:
            text(environment["gpu"], "environment.gpu")
        pattern(environment.get("session"), ID, "environment.session")
        if environment.get("machine") not in ("physical", "vm"):
            problems.append("environment.machine must be physical or vm")
        if harness is not None:
            if environment.get("session") not in harness["sessions"]:
                problems.append("environment.session is not allowed by the harness")
            if environment.get("machine") not in harness["machines"]:
                problems.append("environment.machine is not allowed by the harness")
    results = report.get("results")
    if type(results) is not list or not 1 <= len(results) <= MAX_RESULTS:
        problems.append("results must be a list of 1..64 results")
    if type(results) is list:
        seen = set()
        for index, result in enumerate(results):
            path = f"results[{index}]"
            if not keys(result, {"id", "status"}, path, {"detail"}):
                continue
            test_id = result.get("id")
            pattern(test_id, ID, path + ".id")
            if type(test_id) is str:
                if test_id in seen:
                    problems.append("results contain duplicate test ids")
                seen.add(test_id)
                if harness is not None and test_id not in harness["required"] + harness["optional"]:
                    problems.append(path + ".id is not configured for the harness")
            if result.get("status") not in STATUSES:
                problems.append(path + ".status is unknown")
            if "detail" in result:
                text(result["detail"], path + ".detail", 200)
    return problems


def trust_tier(association: str) -> str:
    if association in ("OWNER", "MEMBER", "COLLABORATOR"):
        return "maintainer-attested"
    if association in ("CONTRIBUTOR", "FIRST_TIME_CONTRIBUTOR", "FIRST_TIMER", "MANNEQUIN", "NONE"):
        return "unverified"
    raise ReportError("unknown author association")


def evaluate(report: dict, harness: dict) -> tuple[str, dict, list[str]]:
    results = {result["id"]: result["status"] for result in report["results"]}
    counts = {name: sum(status == value for status in results.values())
              for name, value in zip(("passed", "failed", "skipped", "unavailable"), STATUSES)}
    notes = []
    incomplete = False
    for test_id in harness["required"]:
        status = results.get(test_id)
        if status is None:
            notes.append(f"required `{test_id}` is missing")
            incomplete = True
        elif status in ("skip", "unavailable"):
            if test_id in harness["allowUnavailable"]:
                notes.append(f"`{test_id}` is `{status}`: {harness['allowUnavailable'][test_id]}")
            else:
                notes.append(f"required `{test_id}` is `{status}`")
                incomplete = True
    evaluation = "failed" if counts["failed"] else "incomplete" if incomplete else "complete"
    return evaluation, counts, notes


def canonical_digest(report: dict) -> str:
    return hashlib.sha256(json.dumps(report, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=False).encode("utf-8")).hexdigest()


def check_run_payload(report: dict, config: dict, repository: str, comment_id: str,
                      comment_url: str, author: str, association: str) -> dict:
    maintainer = trust_tier(association) == "maintainer-attested"
    harness_id, version = report["harness"]["id"], report["harness"]["version"]
    harness = config["harnesses"][harness_id]
    environment, build = report["environment"], report["build"]
    session = environment["session"]
    evaluation, counts, notes = evaluate(report, harness)
    p, f, s, u = (counts[key] for key in ("passed", "failed", "skipped", "unavailable"))
    title = (f"{'Maintainer-attested' if maintainer else 'Unverified'}: {evaluation}, "
             f"{p} passed, {f} failed, {s} skipped, {u} unavailable")
    if maintainer:
        statement = (f"Reported by @{author} ({association}). Maintainer-attested: GitHub did not run "
                     "these tests; this check records the maintainer's report.")
    else:
        statement = (f"Reported by @{author} ({association}). **Unverified**: GitHub did not run "
                     "these tests and cannot confirm them. Weigh this report by its submitter and "
                     "reproduce important claims independently.")
    if build["source"] == "ci-artifact":
        run = build["ciRunId"]
        build_line = (f"- Build: CI run [`{run}`](https://github.com/{repository}/actions/runs/{run}) "
                      f"built `{build['builtCommit']}`, identity SHA-256 `{build['identitySha256']}` "
                      "(reported by the submitter, not verified)")
    else:
        build_line = (f"- Build: local build of `{build['builtCommit']}`, "
                      f"{'dirty' if build['dirty'] else 'clean'} tree "
                      "(reported by the submitter, not verified)")
    if build["builtCommit"] != report["commit"]:
        build_line += " (built commit differs from the reported commit)"
    environment_line = (f"- Environment: `{environment['os']}`, session `{session}`, "
                        f"`{environment['machine']}` machine")
    if environment.get("gpu"):
        environment_line += f", GPU `{environment['gpu']}`"
    lines = [statement, "", f"- Harness: `{harness_id}` version `{version}`: {harness['description']}",
             f"- Commit: `{report['commit']}`", build_line, environment_line,
             f"- Recorded at: `{report['recordedAt']}`", f"- Evaluation: `{evaluation}`"]
    lines.extend("  - " + note for note in notes)
    lines.extend([f"- Report SHA-256: `{canonical_digest(report)}`",
                  f"- Source comment: {comment_url}", "", "| Test | Status | Detail |",
                  "| --- | --- | --- |"])
    for result in report["results"]:
        detail = f"`{result['detail']}`" if "detail" in result else ""
        lines.append(f"| `{result['id']}` | `{result['status']}` | {detail} |")
    return {
        "name": f"Desktop evidence: {harness_id} ({session}) by @{author}",
        "head_sha": report["commit"],
        "status": "completed",
        "conclusion": ("success" if evaluation == "complete" else "failure") if maintainer else "neutral",
        "external_id": f"comment-{comment_id}",
        "details_url": comment_url,
        "output": {"title": title, "summary": "\n".join(lines)},
    }


def render_comment(report: dict, config: dict) -> str:
    lines = [MARKER, f"### Desktop evidence: `{report['harness']['id']}` on `{report['commit'][:12]}`", ""]
    lines.extend(f"- `{result['id']}`: `{result['status']}`" for result in report["results"])
    lines.extend(["", "See the repository's desktop-evidence documentation.", "", "```json",
                  json.dumps(report, indent=2, ensure_ascii=False), "```", ""])
    return "\n".join(lines)


def load_allowed_commits(path: Path) -> set[str]:
    commits = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip().lower()
        if line:
            if not matches(COMMIT, line):
                raise ConfigError("allowed-commits file contains an invalid commit")
            commits.add(line)
    return commits


def write_output(destination: str, text: str) -> None:
    if destination == "-":
        sys.stdout.write(text)
    else:
        with Path(destination).open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)


def step_summary(lines: list[str]) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with Path(path).open("a", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(lines) + "\n")


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise UsageError(message)


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("validate", "comment", "publish"):
        sub = commands.add_parser(command)
        sub.add_argument("--config", type=Path, required=True)
        sub.add_argument("--repository", required=True)
        if command == "validate":
            source = sub.add_mutually_exclusive_group(required=True)
            source.add_argument("--report", type=Path)
            source.add_argument("--comment-file", type=Path)
            sub.add_argument("--allowed-commits", type=Path)
        elif command == "comment":
            sub.add_argument("--report", type=Path, required=True)
            sub.add_argument("--out", default="-")
        else:
            source = sub.add_mutually_exclusive_group(required=True)
            source.add_argument("--comment-env")
            source.add_argument("--comment-file", type=Path)
            for option in ("comment-id", "comment-url", "author", "association", "out"):
                sub.add_argument("--" + option, required=True)
            sub.add_argument("--allowed-commits", type=Path, required=True)
    args = None
    try:
        args = parser.parse_args(argv)
        if not matches(PROJECT, args.repository):
            raise UsageError("repository must be OWNER/REPO")
        config = load_config(args.config)
        allowed = load_allowed_commits(args.allowed_commits) if getattr(args, "allowed_commits", None) else None
        if args.command == "publish":
            if not re.fullmatch(r"[1-9][0-9]{0,19}", args.comment_id):
                raise ReportError("invalid comment id")
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}", args.author):
                raise ReportError("invalid author login")
            if (not re.fullmatch(r"https://github\.com/[A-Za-z0-9._/#-]+", args.comment_url)
                    or not re.match(r"https://github\.com/(?i:" + re.escape(args.repository) + r")/pull/",
                                    args.comment_url)):
                raise ReportError("comment URL does not name a pull request in the receiving repository")
            trust_tier(args.association)
        if getattr(args, "comment_env", None) is not None:
            if args.comment_env not in os.environ:
                raise UsageError("comment environment variable is not set")
            report = parse_report(extract_report(os.environ[args.comment_env]))
        elif getattr(args, "comment_file", None) is not None:
            report = parse_report(extract_report(args.comment_file.read_text(encoding="utf-8")))
        else:
            report = parse_report(args.report.read_text(encoding="utf-8"))
        problems = validate_report(report, config, args.repository, allowed)
        if problems:
            raise ReportError("\n".join(problems))
        if args.command == "validate":
            print(f"valid: {canonical_digest(report)}")
        elif args.command == "comment":
            write_output(args.out, render_comment(report, config))
        else:
            payload = check_run_payload(report, config, args.repository, args.comment_id,
                                        args.comment_url, args.author, args.association)
            write_output(args.out, json.dumps(payload, indent=2, ensure_ascii=False) + "\n")
            step_summary(["### " + payload["output"]["title"], "", payload["name"], ""])
        return 0
    except ReportError as error:
        reasons = str(error).splitlines()
        for reason in reasons:
            print("rejected: " + reason, file=sys.stderr)
        if args is not None and args.command == "publish":
            try:
                step_summary(["### Desktop evidence report rejected", ""] + [
                    "- `" + json.dumps(reason, ensure_ascii=True).replace("`", "'") + "`"
                    for reason in reasons
                ])
            except OSError as summary_error:
                print(f"error writing step summary: {summary_error}", file=sys.stderr)
                return 1
        return 2
    except (OSError, UnicodeError, ValueError, RecursionError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
