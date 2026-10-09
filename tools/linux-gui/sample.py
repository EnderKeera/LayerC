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

TICKS = os.sysconf("SC_CLK_TCK")


def memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        key, _, rest = line.partition(":")
        if key in ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Private_Hugetlb", "Swap"):
            values[key] = int(rest.split()[0]) * 1024
    uss = sum(values.get(k, 0) for k in ("Private_Clean", "Private_Dirty", "Private_Hugetlb"))
    return {"ussBytes": uss, "pssBytes": values.get("Pss", 0),
            "rssBytes": values.get("Rss", 0), "swapBytes": values.get("Swap", 0)}


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / TICKS


def write(path, data):
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def idle(pid, seconds, output):
    start, initial, samples = time.monotonic(), cpu_seconds(pid), []
    while time.monotonic() - start < seconds:
        samples.append({"monotonic": round(time.monotonic(), 6), **memory(pid)})
        time.sleep(0.25)
    elapsed = time.monotonic() - start
    write(output, {"process": "layere", "pid": pid, "idleSeconds": round(elapsed, 3),
                   "idleCpuPercent": round(100 * (cpu_seconds(pid) - initial) / elapsed, 4),
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
