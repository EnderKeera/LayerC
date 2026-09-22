#!/usr/bin/env bash
# Configure, build, and test LayerC in a digest-pinned Linux container.
#
# Usage: ci-linux.sh <image>
#
# The caller (CI or a local shell with docker) passes the container image;
# tags stay as display labels, digests in ci.yml are the real pins. The repo
# is mounted read-only and the build lives in a container-only path, so the
# checkout never collects build output. Runnable locally:
#   ./tools/ci-linux.sh ubuntu:24.04
set -Eeuo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <image>" >&2
  exit 64
fi
image=$1

repo=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}

# Intentionally unpinned distro packages: the base image digest is the
# reproducibility pin; tool/library versions drift with the distro and any
# skew surfaces here, with the installed versions logged below for evidence.
docker run --rm \
  -v "$repo:/work:ro" \
  "$image" \
  bash -eu -c '
    apt-get update
    apt-get install -y --no-install-recommends \
      g++ cmake ninja-build xvfb xauth \
      libx11-dev libxi-dev libxext-dev libxrandr-dev \
      libxcomposite-dev libxdamage-dev libxfixes-dev libxtst-dev
    cmake --version
    g++ --version
    cmake -S /work -B /tmp/build-linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug
    cmake --build /tmp/build-linux --parallel
    xvfb-run -a ctest --test-dir /tmp/build-linux \
      --output-on-failure --no-tests=error \
      --output-junit /tmp/layerc-ctest.xml
    total=$(grep -o "<testcase" /tmp/layerc-ctest.xml | wc -l || true)
    skipped=$(grep -o "<skipped" /tmp/layerc-ctest.xml | wc -l || true)
    echo "ctest cases=$total skipped=$skipped"
    if [[ $total -eq 0 ]]; then
      echo "error: no test cases reported; refusing a vacuous pass" >&2
      exit 1
    fi
    if [[ $skipped -ne 0 ]]; then
      echo "error: $skipped test(s) skipped; X11 suites must run, not skip" >&2
      exit 1
    fi
  '
