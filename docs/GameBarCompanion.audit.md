# Game Bar Companion Audit

## Current status

The source tree now contains a typed companion contract, strict bounded snapshot
parser, stale-data bridge, daemon-owned publication transport, sidecar validation
and preservation, focused tests, and an unsigned manifest/package skeleton.

## Remaining release gates

- **No end-to-end activation path yet (release blocker).** `layerc-gamebar.exe`
  is a named-pipe one-way consumer only: it implements no Windows AppService
  activation factory, no Game Bar widget registration/rendering, and the daemon
  never launches it - a valid sidecar produces a pipe that nobody reads. The
  host's `AppServiceConnection` is a package-readiness stub.
- The compiled C++/WinRT host and AppService package now build; the Xbox Game Bar SDK-specific widget registration, real AppService client behavior, and the launch path remain release-gate work.
- CMake now assembles an unsigned MSIX; CI must install it and run Game Bar/Explorer restart,
  reconnect, eight-slot, controller, click-through, DPI, and fullscreen matrix tests.
- Official release packaging still requires an externally supplied signing secret.

Until those gates pass, V2.4 remains work in progress and must not be described as
a complete shipping Game Bar companion.
