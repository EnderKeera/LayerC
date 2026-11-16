# DiagnosticsDialog

Tools > Runtime diagnostics: a resizable tool window with a primary Close.

- **Intro** in `subtext`: "LayerD’s health and a one-time snapshot. Nothing is kept after this window closes."
- **Summary** as label / value rows, labels in `secondary` in a 120 px column: LayerD, Active profile, Visual worker, Last error. LayerD and Visual worker lead with a StatusText mark and word: ✓ Connected, ! Quarantined or Degraded, ✕ Not connected or Stopped. Not connected adds what to do: "Start LayerC to run LayerD, then refresh." A value that doesn't exist yet reads "Unavailable" in `disabled`.
- **Details:** a semibold heading with Summary / Raw JSON radio buttons, disabled until a snapshot arrives, over a read-only box in mono `verbose` (Services, Sessions, Automation faults, overlay item diagnostics and, on Linux, X11, capabilities and display). Without a snapshot the box holds an EmptyState: "No snapshot yet." and "LayerD reports one while it runs."
- **Buttons:** Refresh snapshot, Retry visual (only while a helper is quarantined) and Copy raw JSON (disabled without a snapshot) at the left; Close at the right.
- **Source:** `runtime_diagnostics_view` returns each summary row with its `StatusKind`, so the marks never come from parsing the text.

The consumer supplies the connection state and the snapshot. The values shown are sample data.
