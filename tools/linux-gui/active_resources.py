"""Continuous resource evidence for the sampler-owned editor, not idle evidence.

This recorder belongs to start_process.py, the editor's sole parent. It never
signals or reaps anything. A discovered child fails closed: an unaccounted
editor helper must never silently disappear from a reported aggregate.
"""
import hashlib
import json
import math
import time
from pathlib import Path, PurePosixPath

MIB = 1024 * 1024
PEAK_LIMIT = 48 * MIB
MAX_SAMPLE_GAP = 0.1
MEMORY_FIELDS = ("ussBytes", "pssBytes", "rssBytes", "swapBytes")
REQUIRED_WINDOWS = ("simulator-play", "trace-recording", "x11-capture",
                    "template-processing", "template-matching")


def memory(proc):
    values = {}
    wanted = {"Private_Clean", "Private_Dirty", "Private_Hugetlb", "Pss", "Rss", "Swap"}
    for line in (proc / "smaps_rollup").read_text().splitlines():
        key, _, rest = line.partition(":")
        if key in wanted:
            parts = rest.split()
            if len(parts) != 2 or parts[1] != "kB" or not parts[0].isdigit():
                raise RuntimeError("invalid active smaps_rollup field: " + key)
            values[key] = int(parts[0]) * 1024
    if set(values) != wanted:
        raise RuntimeError("incomplete active smaps_rollup sample")
    return {"ussBytes": values["Private_Clean"] + values["Private_Dirty"]
                       + values["Private_Hugetlb"],
            "pssBytes": values["Pss"], "rssBytes": values["Rss"], "swapBytes": values["Swap"]}


class LifetimeRecorder:
    def __init__(self, output, pid, start_time, nonce, executable):
        self.output = Path(output)
        self.raw = self.output.with_suffix(".jsonl")
        # Never overwrite an earlier measurement or let a symlink redirect it.
        self.stream = self.raw.open("x", encoding="utf-8", buffering=65536)
        self.report = {"passed": False, "pid": pid, "startTime": start_time,
                       "nonce": nonce, "executable": str(executable),
                       "rawFile": self.raw.name, "sampleIntervalSeconds": 0.005,
                       "sampleCount": 0, "observedEditorChildren": [],
                       "peak": {key: 0 for key in MEMORY_FIELDS},
                       "note": "Continuous sampled peak; sub-interval maxima are not proved. "
                               "USS includes Private_Hugetlb. Unaccounted helpers fail closed."}
        self.first = self.last = None
        self.max_gap = 0.0
        self.closed = False

    def sample(self):
        proc = Path("/proc") / str(self.report["pid"])
        start = (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
        if start != self.report["startTime"] or (proc / "exe").resolve() != Path(self.report["executable"]):
            raise RuntimeError("active sampler editor identity changed")
        children = set()
        for task in (proc / "task").iterdir():
            try:
                children.update(int(value) for value in (task / "children").read_text().split())
            except FileNotFoundError:
                continue  # A finished thread, not an omitted live helper.
        if children:
            self.report["observedEditorChildren"] = sorted(children)
            raise RuntimeError("editor-owned helper requires aggregate resource accounting")
        values = memory(proc)
        now = time.monotonic()
        sample = {"monotonic": now, **values}
        self.stream.write(json.dumps(sample, separators=(",", ":")) + "\n")
        for key, value in values.items():
            self.report["peak"][key] = max(self.report["peak"][key], value)
        self.report["sampleCount"] += 1
        if self.last is not None:
            self.max_gap = max(self.max_gap, now - self.last)
        self.first = now if self.first is None else self.first
        self.last = now

    def finish(self, orderly, error=None):
        if self.closed:
            return
        self.closed = True
        self.stream.close()
        self.report.update(firstMonotonic=self.first, lastMonotonic=self.last,
                           maxSampleGapSeconds=self.max_gap, stoppedByHarness=bool(orderly))
        self.report["passed"] = bool(orderly and not error and self.first is not None)
        if error:
            self.report["error"] = str(error)
        digest = hashlib.sha256()
        with self.raw.open("rb") as stream:
            for block in iter(lambda: stream.read(65536), b""):
                digest.update(block)
        self.report["rawSha256"] = digest.hexdigest()
        temporary = self.output.with_name(self.output.name + ".tmp")
        temporary.write_text(json.dumps(self.report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.output)


def finite(value):
    try:
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value) and 0 <= value < 2**63)
    except OverflowError:
        return False


def evaluate(out, *, activity_file="active-windows.json", fixture=None,
             required_windows=REQUIRED_WINDOWS):
    """Recompute peaks and operation coverage from raw, same-identity evidence.

    Boundary-document runs supply their independently selected input identity and
    operation inventory. Both workloads retain the same 48 MiB sampled peak gate.
    """
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def load(name):
        return json.loads((out / name).read_text(encoding="utf-8"),
                          parse_constant=lambda name: None)

    require(isinstance(activity_file, str) and PurePosixPath(activity_file).name == activity_file
            and "\\" not in activity_file, "invalid operation evidence path")
    require(isinstance(required_windows, (tuple, list)) and required_windows
            and all(isinstance(name, str) and name for name in required_windows)
            and len(set(required_windows)) == len(required_windows),
            "invalid required-operation inventory")
    fixture = {"profiles": 20, "cooldownsEach": 10, "panelsEach": 2} if fixture is None else fixture
    require(isinstance(fixture, dict) and fixture, "missing expected workload identity")
    report = load("active-resources.json")
    startup = load("startup-resources.json")
    identity = load("editor-identity.json")
    activity = load(activity_file)
    require(all(isinstance(value, dict) for value in (report, startup, identity, activity)),
            "active resource evidence must contain objects")
    require(report.get("passed") is True and report.get("stoppedByHarness") is True,
            "continuous active sampler did not finish cleanly")
    require(type(identity.get("pid")) is int and identity["pid"] > 0
            and isinstance(identity.get("startTime"), str) and identity["startTime"].isdigit()
            and isinstance(identity.get("nonce"), str) and identity["nonce"],
            "invalid active editor identity")
    for key, startup_key in (("pid", "applicationPid"), ("startTime", "applicationStartTime"),
                             ("nonce", "nonce")):
        require(report.get(key) == identity[key] == startup.get(startup_key),
                "active samples do not name this owned editor: " + key)
    require(type(report.get("pid")) is int and type(startup.get("applicationPid")) is int
            and startup.get("passed") is True and startup.get("stoppedByHarness") is True
            and isinstance(report.get("executable"), str)
            and report["executable"] == startup.get("executable")
            and PurePosixPath(report["executable"]).is_absolute()
            and PurePosixPath(report["executable"]).name == "layere"
            and all(finite(report.get(key)) for key in
                    ("firstMonotonic", "lastMonotonic", "maxSampleGapSeconds"))
            and report.get("sampleIntervalSeconds") == 0.005,
            "invalid continuous sampler identity or interval")
    require(report.get("observedEditorChildren") == [], "unaccounted editor-owned helper")
    require(report.get("rawFile") == "active-resources.jsonl", "unexpected active sample path")
    require(activity.get("passed") is True, "active operations did not pass semantic checks")
    require(activity.get("fixture") == fixture,
            "active workload does not match its independently selected fixture")
    windows = activity.get("windows")
    require(isinstance(windows, dict) and set(windows) == set(required_windows),
            "missing or unexpected active-operation windows")
    observations = {}
    for name, window in windows.items():
        require(isinstance(window, dict) and finite(window.get("start")) and finite(window.get("end"))
                and window["end"] > window["start"] and window.get("verified") is True
                and isinstance(window.get("screenshots"), list) and len(window["screenshots"]) >= 2,
                "invalid or unverified active operation: " + name)
        for shot in window["screenshots"]:
            require(isinstance(shot, str) and shot.startswith("shots/")
                    and ".." not in Path(shot).parts and (out / shot).is_file(),
                    "missing operation screenshot: " + name)
        observations[name] = {"sampleCount": 0, "peak": {key: 0 for key in MEMORY_FIELDS}}
    peaks, count, first, previous, max_gap = {key: 0 for key in MEMORY_FIELDS}, 0, None, None, 0.0
    digest = hashlib.sha256()
    with (out / report["rawFile"]).open("rb") as stream:
        for raw in stream:
            require(len(raw) <= 4096, "oversized active memory sample")
            digest.update(raw)
            sample = json.loads(raw, parse_constant=lambda name: None)
            require(isinstance(sample, dict) and finite(sample.get("monotonic"))
                    and all(type(sample.get(key)) is int and 0 <= sample[key] < 2**63
                            for key in MEMORY_FIELDS), "malformed active memory sample")
            now = sample["monotonic"]
            if previous is not None:
                require(now > previous, "active sample timestamps are not increasing")
                max_gap = max(max_gap, now - previous)
            first = now if first is None else first
            previous, count = now, count + 1
            for key in MEMORY_FIELDS:
                peaks[key] = max(peaks[key], sample[key])
            for name, window in windows.items():
                if window["start"] <= now <= window["end"]:
                    observed = observations[name]
                    observed["sampleCount"] += 1
                    for key in MEMORY_FIELDS:
                        observed["peak"][key] = max(observed["peak"][key], sample[key])
    require(count >= 3 and type(report.get("sampleCount")) is int and count == report["sampleCount"],
            "active sample count does not match raw data")
    require(digest.hexdigest() == report.get("rawSha256"), "active sample checksum mismatch")
    require(first == report.get("firstMonotonic") and previous == report.get("lastMonotonic")
            and max_gap == report.get("maxSampleGapSeconds"), "active sampling interval mismatch")
    require(max_gap <= MAX_SAMPLE_GAP, "unobserved gap in continuous active memory sampling")
    require(peaks == report.get("peak"), "active memory peaks do not match raw data")
    for name, window in windows.items():
        require(first <= window["start"] < window["end"] <= previous
                and observations[name]["sampleCount"] >= 3,
                "continuous samples do not cover active operation: " + name)
    require(peaks["ussBytes"] <= PEAK_LIMIT, "active advanced-view USS target exceeded")
    return {"targetPeakUssBytes": PEAK_LIMIT, "peak": peaks, "windows": observations,
            "sampleCount": count, "maxSampleGapSeconds": max_gap,
            "note": report["note"]}
