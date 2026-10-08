#!/usr/bin/env python3
"""An attached test client; its memory/CPU is harness overhead, not LayerE."""
import gi
import pyatspi

gi.require_version("Atspi", "2.0")
pyatspi.Registry.registerEventListener(
    lambda event: None, "object:state-changed", "object:text-changed")
pyatspi.Registry.getDesktop(0)
print("client-ready", flush=True)
pyatspi.Registry.start()
