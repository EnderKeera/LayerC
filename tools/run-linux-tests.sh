#!/usr/bin/env bash
set -Eeuo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/linux-lib.sh"
export PYTHONDONTWRITEBYTECODE=1

usage() {
  cat <<'EOF'
Usage: run-linux-tests.sh [options]

Options:
  --source-dir PATH       Repository root (default: auto-detected)
  --build-dir PATH        Native build directory (default: build/linux-automation)
  --evidence-dir PATH     Durable artifact directory (default: out/linux-automation/<run>)
  --flatpak POLICY        off, auto, or required (default: auto; required for release mode)
  --flatpak-only          Build only the X11 smoke fixture natively and run the
                          Flatpak stages; needs --flatpak required. For CI jobs
                          whose native coverage comes from another job.

Environment:
  LAYERC_RELEASE_EVIDENCE=1  Makes absent Flatpak coverage a failure.
EOF
}

source_dir=
build_dir=
evidence_dir=
flatpak_policy=
flatpak_only=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-dir) source_dir=${2:?}; shift 2 ;;
    --build-dir) build_dir=${2:?}; shift 2 ;;
    --evidence-dir) evidence_dir=${2:?}; shift 2 ;;
    --flatpak) flatpak_policy=${2:?}; shift 2 ;;
    --flatpak-only) flatpak_only=true; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 64 ;;
  esac
done

source_dir=${source_dir:-$(git rev-parse --show-toplevel)}
source_dir=$(cd "$source_dir" && pwd -P)
build_dir=${build_dir:-$source_dir/build/linux-automation}
if [[ -z $evidence_dir ]]; then
  stamp=$(date -u +%Y%m%dT%H%M%SZ)
  short_commit=$(git -c "safe.directory=$source_dir" -C "$source_dir" rev-parse --short=12 HEAD)
  evidence_dir="$source_dir/out/linux-automation/$stamp-$short_commit"
fi
flatpak_policy=${flatpak_policy:-$([[ ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]] && echo required || echo auto)}
case "$flatpak_policy" in off | auto | required) ;; *)
  echo "error: --flatpak must be off, auto, or required" >&2
  exit 64
esac
if [[ ${LAYERC_RELEASE_EVIDENCE:-0} == 1 && $flatpak_policy != required ]]; then
  echo "error: release evidence requires --flatpak required" >&2
  exit 64
fi
if [[ $flatpak_only == true ]] &&
    [[ $flatpak_policy != required || ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]]; then
  echo "error: --flatpak-only needs --flatpak required and no release evidence" >&2
  exit 64
fi

mkdir -p "$build_dir" "$evidence_dir"
build_dir=$(cd "$build_dir" && pwd -P)
evidence_dir=$(cd "$evidence_dir" && pwd -P)
if [[ $build_dir == / || $evidence_dir == / ||
      $build_dir == "$source_dir" || $evidence_dir == "$source_dir" ||
      $build_dir == "$evidence_dir" ]]; then
  echo "error: unsafe build or evidence directory" >&2
  exit 64
fi
if find "$evidence_dir" -mindepth 1 -print -quit | grep -q .; then
  echo "error: evidence directory is not empty: $evidence_dir" >&2
  exit 73
fi
mkdir -p "$evidence_dir/stages"

commit=$(git -c "safe.directory=$source_dir" -C "$source_dir" rev-parse --verify HEAD)
record_stage() {
  local name=$1 status=$2 reason=${3:-}
  printf '%s\n' "$status" >"$evidence_dir/stages/$name.status"
  printf '%s\n' "$reason" >"$evidence_dir/stages/$name.reason"
}
run_logged() {
  local name=$1
  shift
  record_stage "$name" running
  if "$@" 2>&1 | tee "$evidence_dir/$name.log"; then
    record_stage "$name" pass
  else
    local rc=$?
    record_stage "$name" fail "command exited $rc"
    return "$rc"
  fi
}
finish() {
  local rc=$1
  trap - EXIT
  set +e
  [[ $rc -eq 0 ]] || record_stage overall fail "automation exited $rc"
  [[ -f $evidence_dir/stages/overall.status ]] || record_stage overall pass
  if ! git -c "safe.directory=$source_dir" -C "$source_dir" status --short \
      >"$evidence_dir/git-status-after.txt"; then
    record_stage overall fail "could not inspect final source tree"
    rc=1
  fi
  if [[ ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]] &&
      { [[ -s $evidence_dir/git-status.txt ]] ||
        [[ -s $evidence_dir/git-status-after.txt ]]; }; then
    record_stage overall fail "release evidence requires a clean source tree"
    rc=1
  fi
  EVIDENCE_DIR="$evidence_dir" SOURCE_COMMIT="$commit" \
    FLATPAK_POLICY="$flatpak_policy" python3 - <<'PY'
import json
import os
from datetime import datetime, timezone
from pathlib import Path

root = Path(os.environ["EVIDENCE_DIR"])
stages = {}
for path in sorted((root / "stages").glob("*.status")):
    name = path.stem
    reason_path = path.with_suffix(".reason")
    stages[name] = {
        "status": path.read_text(encoding="utf-8").strip(),
        "reason": reason_path.read_text(encoding="utf-8").strip()
        if reason_path.exists()
        else "",
    }
git_status = root / "git-status.txt"
git_status_after = root / "git-status-after.txt"
source_tree_clean = all(
    path.exists() and not path.read_text(encoding="utf-8").strip()
    for path in (git_status, git_status_after)
)
summary = {
    "schemaVersion": 1,
    "recordedAtUtc": datetime.now(timezone.utc).isoformat(),
    "commit": os.environ["SOURCE_COMMIT"],
    "sourceTreeClean": source_tree_clean,
    "flatpakPolicy": os.environ["FLATPAK_POLICY"],
    "automatedEvidence": stages,
    "physicalEvidence": {
        "status": "pending",
        "required": ["gnome-xorg-native", "gnome-xorg-flatpak", "kde-x11-native", "kde-x11-flatpak"],
        "note": "Automation does not attest pointer, GPU, real-monitor, or perceptual checks.",
    },
}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
PY
  local summary_rc=$?
  if [[ $summary_rc -ne 0 ]]; then
    echo "error: could not write automation summary" >&2
    record_stage overall fail "summary generation failed"
    rc=1
  fi
  layerc_write_sha256sums "$evidence_dir"
  local checksum_rc=$?
  if [[ $checksum_rc -ne 0 ]]; then
    echo "error: could not write evidence checksums" >&2
    record_stage overall fail "checksum generation failed"
    rm -f -- "$evidence_dir/summary.json"
    rc=1
  fi
  exit "$rc"
}
trap 'finish $?' EXIT

{
  printf 'commit=%s\n' "$commit"
  printf 'recordedAtUtc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'sourceDir=%s\nbuildDir=%s\nevidenceDir=%s\n' \
    "$source_dir" "$build_dir" "$evidence_dir"
  uname -a
  cat /etc/os-release 2>/dev/null || true
} >"$evidence_dir/environment.txt"
git -c "safe.directory=$source_dir" -C "$source_dir" status --short >"$evidence_dir/git-status.txt" 2>/dev/null || true
{
  cmake --version
  ctest --version
  ninja --version
  "${CXX:-g++}" --version
  Xvfb -version 2>&1 || true
  python3 --version
  flatpak --version 2>/dev/null || true
  flatpak-builder --version 2>/dev/null || true
} >"$evidence_dir/tool-versions.txt"

run_logged report-helper-tests \
  python3 -m unittest "$source_dir/tools/tests/test_linux_test_report.py"
run_logged shell-helper-tests bash "$source_dir/tools/tests/test_linux_lib.sh"

run_logged configure cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLAYERC_BUILD_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$build_dir/install"
if [[ $flatpak_only == true ]]; then
  # The native Linux jobs own the build, CTest, and native smokes; the Flatpak
  # smoke only needs the fixture window from the native tree.
  run_logged build cmake --build "$build_dir" --parallel \
    --target layerc_linux_x11_smoke_fixture
  record_stage native not-run \
    'flatpak-only mode; native coverage is recorded by the native Linux jobs'
else
  run_logged build cmake --build "$build_dir" --parallel
  run_logged install cmake --install "$build_dir"

  ctest --test-dir "$build_dir" --show-only=json-v1 \
    >"$evidence_dir/ctest-registration.json"

  headless_junit="$evidence_dir/ctest-headless.xml"
  run_logged ctest-headless env -u DISPLAY ctest --test-dir "$build_dir" \
    -L '^headless$' --output-on-failure --no-tests=error \
    --output-junit "$headless_junit"
  run_logged verify-headless python3 "$source_dir/tools/linux_test_report.py" \
    --registration "$evidence_dir/ctest-registration.json" \
    --junit "$headless_junit" --gate linux-headless \
    --report "$evidence_dir/ctest-headless-summary.json"

  run_logged native-headless-smoke env -u DISPLAY \
    bash "$source_dir/tools/linux-runtime-smoke.sh" headless \
    "$build_dir/install/bin" "$evidence_dir/native-headless" \
    "$source_dir/examples/lctest.json"

  x11_junit="$evidence_dir/ctest-x11.xml"
  run_logged ctest-x11 xvfb-run -a bash -c \
    'source "$1"; shift; layerc_x11_exec 50 0.2 "$@"' _ "$source_dir/tools/linux-lib.sh" \
    ctest --test-dir "$build_dir" -L '^x11$' --output-on-failure --no-tests=error \
    --output-junit "$x11_junit"
  run_logged verify-x11 python3 "$source_dir/tools/linux_test_report.py" \
    --registration "$evidence_dir/ctest-registration.json" \
    --junit "$x11_junit" --gate linux-x11 \
    --report "$evidence_dir/ctest-x11-summary.json"
  run_logged native-x11-smoke xvfb-run -a \
    bash "$source_dir/tools/linux-runtime-smoke.sh" x11 \
    "$build_dir/install/bin" "$evidence_dir/native-x11" \
    "$source_dir/examples/lctest.json" "$build_dir/layerc_linux_x11_smoke_fixture"
fi

run_flatpak=false
flatpak_reason=
if [[ $flatpak_policy == off ]]; then
  flatpak_reason='disabled by policy; no Flatpak coverage was claimed'
elif ! command -v flatpak >/dev/null || ! command -v flatpak-builder >/dev/null; then
  flatpak_reason='flatpak or flatpak-builder is unavailable'
elif ! flatpak info org.freedesktop.Platform//25.08 >/dev/null 2>&1 ||
     ! flatpak info org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
  flatpak_reason='org.freedesktop Platform/SDK 25.08 is not installed'
else
  run_flatpak=true
fi

if [[ $run_flatpak == false ]]; then
  flatpak_status=unavailable
  [[ $flatpak_policy == off ]] && flatpak_status=not-run
  record_stage flatpak "$flatpak_status" "$flatpak_reason"
  if [[ $flatpak_policy == required ]]; then
    echo "error: required Flatpak coverage unavailable: $flatpak_reason" >&2
    exit 1
  fi
else
  manifest="$source_dir/packaging/flatpak/io.github.EnderKeera.LayerC.json"
  {
    flatpak info org.freedesktop.Platform//25.08
    flatpak info org.freedesktop.Sdk//25.08
  } >"$evidence_dir/flatpak-runtime-refs.txt"
  python3 - "$manifest" >"$evidence_dir/flatpak-permissions.json" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
manifest = json.loads(path.read_text(encoding="utf-8"))
actual = manifest.get("finish-args", [])
expected = ["--socket=x11"]
result = {"expected": expected, "actual": actual, "result": "pass" if actual == expected else "fail"}
print(json.dumps(result, indent=2))
if actual != expected:
    raise SystemExit("Flatpak permission boundary changed")
PY
  app_id=io.github.EnderKeera.LayerC
  mkdir -p "$source_dir/build"
  flatpak_root=$(mktemp -d "$source_dir/build/flatpak-automation.XXXXXX")
  printf '%s\n' "$flatpak_root" >"$evidence_dir/flatpak-build-root.txt"
  flatpak_build="$flatpak_root/stage"
  flatpak_repo="$flatpak_root/repo"
  export FLATPAK_USER_DIR="$flatpak_root/user-install"
  export HOME="$flatpak_root/home"
  export XDG_CONFIG_HOME="$HOME/.config"
  export XDG_DATA_HOME="$HOME/.local/share"
  export XDG_CACHE_HOME="$HOME/.cache"
  export XDG_STATE_HOME="$HOME/.local/state"
  mkdir -p "$FLATPAK_USER_DIR" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" \
    "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
  run_logged flatpak-build flatpak-builder --force-clean \
    --repo="$flatpak_repo" "$flatpak_build" "$manifest"
  run_logged flatpak-add-remote flatpak --user remote-add \
    --if-not-exists --no-gpg-verify layerc-automation "file://$flatpak_repo"
  run_logged flatpak-install flatpak --user install \
    --noninteractive -y layerc-automation "$app_id"
  flatpak info --user "$app_id" >"$evidence_dir/flatpak-installed-info.txt"
  flatpak info --user --show-permissions "$app_id" \
    >"$evidence_dir/flatpak-installed-permissions.txt"
  run_logged flatpak-headless-smoke env -u DISPLAY \
    bash "$source_dir/tools/linux-runtime-smoke.sh" headless \
    "flatpak:$app_id" "$evidence_dir/flatpak-headless" \
    "$source_dir/examples/lctest.json"
  run_logged flatpak-x11-smoke xvfb-run -a \
    bash "$source_dir/tools/linux-runtime-smoke.sh" x11 \
    "flatpak:$app_id" "$evidence_dir/flatpak-x11" \
    "$source_dir/examples/lctest.json" "$build_dir/layerc_linux_x11_smoke_fixture"
  record_stage flatpak pass
fi

record_stage overall pass
echo "Linux automation evidence: $evidence_dir"
