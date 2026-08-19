# Game Bar Companion Audit

## Current status

The source tree now contains a typed companion contract, strict bounded snapshot
parser, stale-data bridge, daemon-owned publication transport, sidecar validation
and preservation, focused tests, and an unsigned manifest/package skeleton.

## Remaining release gates

- The compiled C++/WinRT host and AppService package now build; the Xbox Game Bar SDK-specific widget registration and real AppService client behavior remain release-gate work.
- CMake now assembles an unsigned MSIX; CI must install it and run Game Bar/Explorer restart,
  reconnect, eight-slot, controller, click-through, DPI, and fullscreen matrix tests.
- Official release packaging still requires an externally supplied signing secret.

Until those gates pass, V2.4 remains work in progress and must not be described as
a complete shipping Game Bar companion.
