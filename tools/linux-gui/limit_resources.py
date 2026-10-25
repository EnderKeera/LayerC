"""Offline qualification of one frozen validator-derived boundary workload."""
import hashlib
import json
from pathlib import PurePosixPath
from typing import Any

from active_resources import evaluate as evaluate_active
from limit_inputs import REQUIRED_GUI_CASES, validate_case_inventory
from limits_walkthrough import REQUIRED_WINDOWS


def evaluate(out):
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def load(name):
        return json.loads((out / name).read_text(encoding="utf-8"),
                          parse_constant=lambda name: None)

    def object_value(value, message) -> dict[str, Any]:
        if not isinstance(value, dict):
            raise ValueError(message)
        return value

    selection = object_value(load("limit-case.json"), "missing selected boundary case")
    manifest = object_value(load("limit-input-manifest.json"), "missing frozen input manifest")
    require(isinstance(selection, dict) and selection.get("schema") == 1,
            "missing selected boundary case")
    require(isinstance(manifest, dict) and manifest.get("schema") == 1
            and manifest.get("passed") is True, "boundary format qualification did not pass")
    manifest_sha = hashlib.sha256((out / "limit-input-manifest.json").read_bytes()).hexdigest()
    require(selection.get("inputsManifestSha256") == manifest_sha,
            "selected boundary input manifest changed")
    required = manifest.get("requiredGuiCases")
    require(isinstance(required, list) and len(required) == len(REQUIRED_GUI_CASES)
            and set(required) == REQUIRED_GUI_CASES, "missing or duplicate boundary family")
    validate_case_inventory(manifest)
    record = object_value(selection.get("case"), "missing boundary case identity")
    require(record.get("caseId") in REQUIRED_GUI_CASES,
            "unknown selected boundary case")
    candidates = manifest.get("cases")
    if not isinstance(candidates, list):
        raise ValueError("missing compiled format verdicts")
    matches = [candidate for candidate in candidates if isinstance(candidate, dict)
               and candidate.get("caseId") == record["caseId"]]
    require(matches == [record] and record.get("expectedAccepted") is True
            and isinstance(record.get("validator"), dict)
            and record["validator"].get("accepted") is True,
            "selected input lacks its unique successful compiled format verdict")
    require(isinstance(manifest.get("limits"), dict)
            and selection.get("compiledLimits") == manifest["limits"],
            "selected input limits differ from the compiled format limits")
    artifact = selection.get("inputArtifact")
    if not isinstance(artifact, str):
        raise ValueError("missing frozen input artifact path")
    require(PurePosixPath(artifact).name == artifact
            and "\\" not in artifact and artifact.startswith("limit-input."),
            "unsafe frozen input artifact path")
    path = out / artifact
    require(path.is_file() and not path.is_symlink()
            and path.stat().st_size == record.get("bytes")
            and hashlib.sha256(path.read_bytes()).hexdigest() == record.get("sha256"),
            "frozen boundary input bytes changed")
    require(hashlib.sha256((out / "profile-before.json").read_bytes()).hexdigest()
            == selection.get("profileSha256"), "selected durable profile changed before launch")
    activity = load("limit-windows.json")
    operation = object_value(activity.get("operation") if isinstance(activity, dict) else None,
                             "missing boundary operation outcome")
    require(type(operation.get("accepted")) is bool
            and isinstance(operation.get("outcome"), str) and operation["outcome"],
            "boundary operation has no explicit accepted/refused outcome")
    if record.get("kind") == "trace":
        require(operation["accepted"] is record.get("editorOperationAccepted")
                and operation.get("sourceSha256") == record["sha256"],
                "trace import did not follow the reviewed allocation policy")
    elif record["caseId"] == "profiles:baseline":
        require(operation["accepted"], "baseline edit/save was refused")
    if not operation["accepted"]:
        require("resource limit" in operation["outcome"].lower(),
                "boundary refusal has no visible resource-limit reason")
    result = evaluate_active(out, activity_file="limit-windows.json",
        fixture={"boundaryCase": record["caseId"], "inputsManifestSha256": manifest_sha},
        required_windows=REQUIRED_WINDOWS)
    return dict(result, caseId=record["caseId"], formatVerdict=record["validator"],
                inputSha256=record["sha256"], inputsManifestSha256=manifest_sha,
                operation=operation,
                scope="One selected valid boundary document, full retained history or trace import, "
                      "visible refusal without durable data loss, then valid-operation recovery. "
                      "External validator and harness memory are excluded; the 48 MiB sampled "
                      "editor peak bound is unchanged.")
