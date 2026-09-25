#!/usr/bin/env bash
# Run the complete automated Linux gate in a digest-pinned distro container.
set -Eeuo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <image> [evidence-directory]" >&2
  exit 64
fi
image=$1
repo=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
evidence=${2:-$repo/out/linux-ci/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$evidence"
evidence=$(cd "$evidence" && pwd)

# The image digest is the reproducibility pin. Package versions are recorded in
# the evidence bundle so distro updates remain visible without stale apt pins.
docker run --rm \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$repo:/work:ro" -v "$evidence:/evidence" \
  "$image" \
  bash -Eeuo pipefail -c '
    cleanup() { chown -R "$HOST_UID:$HOST_GID" /evidence 2>/dev/null || true; }
    trap cleanup EXIT
    apt-get update
    mapfile -t packages < /work/tools/linux-deps.txt
    apt-get install -y --no-install-recommends "${packages[@]}"
    bash /work/tools/run-linux-tests.sh \
      --source-dir /work --build-dir /tmp/build-linux \
      --evidence-dir /evidence --flatpak off
  '
