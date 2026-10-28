#!/usr/bin/env python3
"""Process resource samples for the Linux GUI runner (harness-only).

  sample.py idle PID SECONDS OUTPUT   editor-only USS/PSS/RSS series + CPU share
  sample.py roles OUTPUT ROLE=PID...  one snapshot per harness/daemon process

USS = Private_Clean + Private_Dirty + Private_Hugetlb from smaps_rollup. CPU
is utime+stime over the window, as a percentage of one logical CPU.
"""
import json
import os
import sys
import time
from pathlib import Path


def clock_ticks():
    # Resolved per call, not at import: the Windows tooling-policy tests import
    # the report chain that reaches this module but never sample /proc.
    return os.sysconf("SC_CLK_TCK")


def memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        key, _, rest = line.partition(":")
        if key in ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Private_Hugetlb", "Swap"):
            values[key] = int(rest.split()[0]) * 1024
    uss = sum(values.get(k, 0) for k in ("Private_Clean", "Private_Dirty", "Private_Hugetlb"))
    return {"ussBytes": uss, "pssBytes": values.get("Pss", 0),
            "rssBytes": values.get("Rss", 0), "swapBytes": values.get("Swap", 0)}


def cpu_snapshot(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return {"startTime": fields[19], "userTicks": int(fields[11]),
            "systemTicks": int(fields[12])}


def cpu_seconds(pid):
    sample = cpu_snapshot(pid)
    return (sample["userTicks"] + sample["systemTicks"]) / clock_ticks()


def write(path, data):
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def idle(pid, seconds, output):
    initial, samples = cpu_snapshot(pid), []
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        samples.append({"monotonic": round(time.monotonic(), 6), **memory(pid)})
        time.sleep(0.25)
    ended = time.monotonic()
    final = cpu_snapshot(pid)
    if final["startTime"] != initial["startTime"]:
        raise RuntimeError("editor identity changed during idle sampling")
    initial_ticks = initial["userTicks"] + initial["systemTicks"]
    final_ticks = final["userTicks"] + final["systemTicks"]
    if final_ticks < initial_ticks:
        raise RuntimeError("editor CPU ticks decreased during idle sampling")
    elapsed, ticks = ended - start, clock_ticks()
    write(output, {"process": "layere", "pid": pid, "idleSeconds": round(elapsed, 3),
                   "idleCpuPercent": round(100 * (final_ticks - initial_ticks) / ticks / elapsed, 4),
                   "clockTicksPerSecond": ticks,
                   "idleStartedMonotonic": start, "idleEndedMonotonic": ended,
                   "cpuTicksStart": initial, "cpuTicksEnd": final,
                   "peakIdleUssBytes": max(s["ussBytes"] for s in samples),
                   "peakIdlePssBytes": max(s["pssBytes"] for s in samples),
                   "peakIdleRssBytes": max(s["rssBytes"] for s in samples),
                   "samples": samples})


def roles(output, pairs):
    report = {}
    for pair in pairs:
        role, _, pid = pair.partition("=")
        try:
            report[role] = {"pid": int(pid), **memory(pid), "cpuSeconds": cpu_seconds(pid)}
        except (OSError, ValueError, IndexError) as error:
            report[role] = {"pid": pid, "error": str(error)}
    write(output, {"note": "Harness/daemon processes, measured separately from LayerE",
                   "processes": report})


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "idle":
        idle(int(sys.argv[2]), float(sys.argv[3]), Path(sys.argv[4]))
    elif len(sys.argv) >= 4 and sys.argv[1] == "roles":
        roles(Path(sys.argv[2]), sys.argv[3:])
    else:
        sys.exit("usage: sample.py idle PID SECONDS OUTPUT | roles OUTPUT ROLE=PID...")
