"""Trace parser-bound candidates; the compiled validator must check every verdict.

No memory acceptance is inferred here. In particular, the opaque-array family
exercises syntactically valid data the trace reader ignores, rather than only
padding a small root event up to the file-size limit.
"""
import copy
import hashlib
import json
from pathlib import Path


def wire(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def write_trace_fixtures(output, limits):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    cap = limits["traceBytes"]
    entries_cap = limits["traceEntries"]
    fields_cap = limits["collectionEntries"]
    string_cap = limits["stringBytes"]
    records = []

    def document(entries, **extra):
        return {"schemaVersion": 1, "kind": "layerctrace", "entries": entries, **extra}

    def event(index=0):
        return {"phase": "root", "event": "inputPressed", "timeMs": index,
                "source": "boundary", "origin": index, "fields": {}}

    def save(name, data, accepted=True, **dimensions):
        path = output / (name + ".layerctrace")
        path.write_bytes(data)
        records.append({"id": name, "kind": "trace", "file": path.name,
                        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                        "expectedAccepted": accepted, **dimensions})

    def pad(data):
        if len(data) > cap:
            raise RuntimeError("derived candidate exceeds compiled trace byte bound")
        return data + b" " * (cap - len(data))

    roots = [event(i) for i in range(entries_cap)]
    save("trace-entries-limit", wire(document(roots)), roots=entries_cap)
    save("trace-entries-over-limit", wire(document(roots + [event(entries_cap)])),
         accepted=False, entries=entries_cap + 1, errorPathPrefix="$.entries")

    dense = event()
    dense["source"] = "s" * string_cap
    dense["fields"] = {(f"f{i:03d}" + "k" * string_cap)[:string_cap]:
                       {"type": "string", "value": "v" * string_cap}
                       for i in range(fields_cap)}
    dense_roots = []
    while len(dense_roots) < entries_cap:
        candidate = dense_roots + [dense]
        if len(wire(document(candidate))) > cap:
            break
        dense_roots = candidate
    if not dense_roots:
        raise RuntimeError("one maximum-width root exceeds the trace byte bound")
    dense_bytes = pad(wire(document(dense_roots)))
    save("trace-fields-byte-limit", dense_bytes, roots=len(dense_roots),
         fieldsPerRoot=fields_cap, fieldKeyBytes=string_cap,
         fieldStringBytes=string_cap, sourceBytes=string_cap, byteBound=cap)
    save("trace-one-byte-over-limit", dense_bytes + b" ", accepted=False,
         byteBound=cap + 1, errorPathPrefix="$")

    over = copy.deepcopy(dense)
    over["fields"]["extra"] = {"type": "bool", "value": True}
    save("trace-fields-over-limit", wire(document([over])), accepted=False,
         fieldsPerRoot=fields_cap + 1, errorPathPrefix="$.entries[0]")
    over = event()
    over["fields"] = {"f": {"type": "string", "value": "v" * (string_cap + 1)}}
    save("trace-string-over-limit", wire(document([over])), accepted=False,
         fieldStringBytes=string_cap + 1, errorPathPrefix="$.entries[0].fields.f")

    # Binary search the largest flat, opaque numeric array fitting a valid
    # document; unlike whitespace padding, this saturates parser node storage.
    low, high = 0, cap // 2 + 1
    while low + 1 < high:
        middle = (low + high) // 2
        if len(wire(document([], opaque=[0] * middle))) <= cap:
            low = middle
        else:
            high = middle
    opaque = wire(document([], opaque=[0] * low))
    if len(wire(document([], opaque=[0] * (low + 1)))) <= cap:
        raise RuntimeError("opaque array did not reach the compiled byte frontier")
    save("trace-opaque-array-byte-limit", pad(opaque), roots=0,
         opaqueArrayEntries=low, byteBound=cap)
    save("trace-opaque-string-byte-limit", pad(wire(document([], opaque="x" *
         (cap - len(wire(document([], opaque=""))))))), roots=0, byteBound=cap)

    manifest = {"schema": 1, "limits": limits, "cases": records,
                "scope": "Validator-derived trace dimensions, not GUI/resource acceptance; no thresholds changed"}
    (output / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest
