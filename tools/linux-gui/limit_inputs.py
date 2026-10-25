#!/usr/bin/env python3
"""Freeze validator-derived resource inputs once; select them without regeneration.

This is test-only. Core verdicts describe the file format. The editor may refuse
an otherwise valid operation under its explicitly reported allocation policy.
Neither a parser verdict nor this preparation script is GUI/resource evidence.
"""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path, PurePosixPath

from limit_fixtures import base_profile, wire, write_fixtures
from module_limit_fixtures import write_module_fixtures
from trace_limit_fixtures import write_trace_fixtures

# Required family inventory is independent of the generators' returned rows.
# Removing a workload must fail qualification, not shrink its own denominator.
REQUIRED_GUI_CASES = frozenset({
    "profiles:baseline", "profiles:opaque-string-byte-limit",
    "profiles:opaque-array-byte-limit", "profiles:opaque-object-byte-limit",
    "profiles:opaque-depth-byte-limit", "profiles:cooldowns-byte-limit",
    "profiles:panels-byte-limit", "profiles:bindings-byte-limit",
    "modules:automation-rules-variables-limit", "modules:automation-actions-limit",
    "modules:automation-depth-limit", "modules:automation-nodes-limit",
    "modules:automation-collection-string-limit", "modules:automation-byte-limit",
    "modules:visual-observation-reference-limit", "traces:trace-entries-limit",
    "traces:trace-fields-byte-limit", "traces:trace-opaque-array-byte-limit",
    "traces:trace-opaque-string-byte-limit",
})


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def command(argv, output):
    result = subprocess.run([str(arg) for arg in argv], capture_output=True,
                            timeout=90, check=False)
    output.with_suffix(".stdout").write_bytes(result.stdout)
    output.with_suffix(".stderr").write_bytes(result.stderr)
    output.with_suffix(".command.json").write_text(json.dumps([str(arg) for arg in argv]) + "\n")
    output.with_suffix(".exit").write_text(str(result.returncode) + "\n")
    try:
        value = json.loads(result.stdout)
    except (ValueError, UnicodeError) as error:
        raise ValueError("validator emitted invalid JSON: " + output.name) from error
    if not isinstance(value, dict) or result.stderr:
        raise ValueError("validator emitted malformed evidence or stderr: " + output.name)
    return result.returncode, value


def create(build, output):
    build, output = Path(build).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    probe = build / "layerc_editor_resource_probe"
    helper_sha = digest(probe)
    code, limits = command([probe, "--limits"], output / "compiled-limits")
    if code != 0 or not limits or any(type(value) is not int or value <= 0 for value in limits.values()):
        raise ValueError("compiled limits are missing or invalid")
    if limits.get("traceImportJsonValues") != 131072:
        raise ValueError("compiled editor trace-import policy is not the reviewed policy")
    profiles = write_fixtures(output / "profiles", limits)
    modules = write_module_fixtures(output / "modules", limits)
    traces = write_trace_fixtures(output / "traces", limits)
    records = []
    for group, manifest in (("profiles", profiles), ("modules", modules), ("traces", traces)):
        for original in manifest["cases"]:
            record = dict(original)
            record["group"] = group
            record["caseId"] = group + ":" + record["id"]
            record["path"] = group + "/" + record["file"]
            source = output / record["path"]
            if source.stat().st_size != record["bytes"] or digest(source) != record["sha256"]:
                raise ValueError("generated input does not match its manifest")
            if group == "profiles":
                argv = [probe, "--profile", source]
                record["kind"] = "profile"
            elif group == "modules":
                argv = [probe, "--" + record["kind"], output / "modules/base.json", source]
            else:
                argv = [probe, "--trace", source]
            code, verdict = command(argv, output / (group + "-" + record["id"]))
            accepted = record["expectedAccepted"]
            if type(accepted) is not bool or verdict.get("accepted") is not accepted \
                    or code != (0 if accepted else 2):
                raise ValueError("compiled verdict disagrees with candidate: " + record["caseId"])
            record["validator"] = verdict
            record["editorOperationAccepted"] = accepted and record["id"] != "trace-opaque-array-byte-limit"
            records.append(record)
    required = [row["caseId"] for row in records if row["expectedAccepted"]]
    if len(required) != len(REQUIRED_GUI_CASES) or set(required) != REQUIRED_GUI_CASES:
        raise ValueError("required boundary-family inventory was omitted, duplicated or changed")
    if digest(probe) != helper_sha:
        raise ValueError("validator executable changed while deriving inputs")
    sources = (Path(__file__), Path(__file__).with_name("limit_fixtures.py"),
               Path(__file__).with_name("module_limit_fixtures.py"),
               Path(__file__).with_name("trace_limit_fixtures.py"))
    manifest = {"schema": 1, "passed": True, "limits": limits, "validatorSha256": helper_sha,
                "sources": {path.name: digest(path) for path in sources}, "cases": records,
                "requiredGuiCases": required,
                "scope": "Core format boundaries only. GUI selection, observed peaks, operation refusal, preservation and recovery must be measured separately. No existing budget is changed."}
    write_json(output / "manifest.json", manifest)
    return manifest


def validate_case_inventory(manifest):
    """A declared inventory cannot substitute for actual compiled verdict rows."""
    rows = manifest.get("cases")
    if not isinstance(rows, list) or not rows or any(not isinstance(row, dict) for row in rows):
        raise ValueError("missing compiled boundary verdict rows")
    ids = [row.get("caseId") for row in rows]
    if any(not isinstance(value, str) for value in ids) or len(ids) != len(set(ids)):
        raise ValueError("duplicate or malformed compiled boundary case identity")
    if any(type(row.get("expectedAccepted")) is not bool
           or not isinstance(row.get("validator"), dict)
           or row["validator"].get("accepted") is not row["expectedAccepted"] for row in rows):
        raise ValueError("compiled boundary verdict differs from its expected format outcome")
    accepted = {row["caseId"] for row in rows if row["expectedAccepted"]}
    if accepted != REQUIRED_GUI_CASES:
        raise ValueError("compiled verdict rows are missing a required boundary family")


def load_case(inputs, case_id):
    inputs = Path(inputs).resolve()
    try:
        manifest = json.loads((inputs / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError, UnicodeError) as error:
        raise ValueError("cannot read the frozen input manifest") from error
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or manifest.get("passed") is not True:
        raise ValueError("input set has no successful compiled qualification")
    required = manifest.get("requiredGuiCases")
    if not isinstance(required, list) or len(required) != len(REQUIRED_GUI_CASES) \
            or set(required) != REQUIRED_GUI_CASES:
        raise ValueError("frozen input set is missing a required boundary family")
    validate_case_inventory(manifest)
    matches = [row for row in manifest["cases"] if row["caseId"] == case_id]
    if len(matches) != 1 or case_id not in required:
        raise ValueError("missing, duplicate or non-GUI boundary case: " + case_id)
    record = matches[0]
    relative = PurePosixPath(record["path"])
    if relative.is_absolute() or ".." in relative.parts or len(relative.parts) != 2:
        raise ValueError("unsafe frozen input path")
    source = inputs.joinpath(*relative.parts)
    if source.is_symlink() or not source.is_file() or source.stat().st_size != record["bytes"] \
            or digest(source) != record["sha256"]:
        raise ValueError("frozen input bytes changed")
    return manifest, record, source


def prepare(inputs, case_id, profiles, evidence):
    manifest, record, source = load_case(inputs, case_id)
    profiles, evidence = Path(profiles), Path(evidence)
    if any(profiles.iterdir()):
        raise ValueError("limit case requires an empty disposable profile store")
    profile = base_profile()
    if record["kind"] == "profile":
        profile_bytes = source.read_bytes()
    else:
        if record["kind"] in ("automation", "visual"):
            kind = record["kind"]
            profile["modules"] = [{"kind": kind, "schemaVersion": 1 if kind == "automation" else 2}]
            sidecars = Path(os.environ["XDG_DATA_HOME"]) / "layerc/profiles/fixture"
            sidecars.mkdir(parents=True, exist_ok=True)
            (sidecars / (kind + ".json")).write_bytes(source.read_bytes())
        profile_bytes = wire(profile)
    (profiles / "fixture.json").write_bytes(profile_bytes)
    selection = {"schema": 1, "case": record, "compiledLimits": manifest["limits"],
                 "inputsManifestSha256": digest(Path(inputs) / "manifest.json"),
                 "inputPath": str(source.resolve()), "profileSha256": digest(profiles / "fixture.json"),
                 "profileId": "limit", "profileName": "Limit document",
                 "scope": "One cold-start selected boundary document; external harness/validator memory is not editor USS."}
    # Keep the exact inputs with the bundle so offline verification never needs
    # the original host paths or a rerun of the derivation.
    selection["inputArtifact"] = "limit-input" + source.suffix
    with (evidence / selection["inputArtifact"]).open("xb") as stream:
        stream.write(source.read_bytes())
    with (evidence / "limit-input-manifest.json").open("xb") as stream:
        stream.write((Path(inputs) / "manifest.json").read_bytes())
    write_json(evidence / "limit-case.json", selection)
    return selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("create")
    make.add_argument("--build", type=Path, required=True)
    make.add_argument("--out", type=Path, required=True)
    select = commands.add_parser("prepare")
    select.add_argument("--inputs", type=Path, required=True)
    select.add_argument("--case", required=True)
    select.add_argument("--profiles", type=Path, required=True)
    select.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "create":
        value = create(args.build, args.out)
        print(json.dumps({"passed": value["passed"], "coreCases": len(value["cases"]),
                          "requiredGuiCases": value["requiredGuiCases"]}))
    else:
        value = prepare(args.inputs, args.case, args.profiles, args.evidence)
        print(json.dumps({"caseId": value["case"]["caseId"], "profileSha256": value["profileSha256"]}))


if __name__ == "__main__":
    main()
