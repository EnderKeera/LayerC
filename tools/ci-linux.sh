#!/usr/bin/env bash
# Run the complete automated Linux gate in a digest-pinned distro container.
# Options after the evidence directory go to tools/run-linux-tests.sh (for
# example --editor required --package required); --flatpak stays off here.
set -Eeuo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <image> [evidence-directory] [run-linux-tests options...]" >&2
  exit 64
fi
image=$1
repo=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
evidence=${2:-$repo/out/linux-ci/$(date -u +%Y%m%dT%H%M%SZ)}
shift $(($# < 2 ? $# : 2))
mkdir -p "$evidence"
evidence=$(cd "$evidence" && pwd)

# The image digest is the reproducibility pin. Package versions are recorded in
# the evidence bundle so distro updates remain visible without stale apt pins.
# The container is the disposable root the package stage installs into; only
# this launcher declares it so (LAYERC_DISPOSABLE_ROOT).
docker run --rm \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e LAYERC_DISPOSABLE_ROOT=1 \
  -v "$repo:/work:ro" -v "$evidence:/evidence" \
  "$image" \
  bash -Eeuo pipefail -c '
    cleanup() { chown -R "$HOST_UID:$HOST_GID" /evidence 2>/dev/null || true; }
    trap cleanup EXIT
    bash /work/tools/linux-gate-in-root.sh /work /tmp/build-linux /evidence \
      --flatpak off "$@"
  ' _ "$@"
