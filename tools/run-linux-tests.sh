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
                          Flatpak stages; needs --flatpak required and
                          --package off. For CI jobs whose native coverage comes
                          from another job.
  --editor POLICY         off, auto, or required (default: auto; required for
                          release mode). Selects the LayerE editor gate: the
                          editor CTest gate and the installed editor smoke.
  --package POLICY        off, auto, or required (default: off; required for
                          release mode). Builds and checks the Debian package;
                          installing it needs a disposable container root.

Environment:
  LAYERC_RELEASE_EVIDENCE=1  Requires Flatpak, editor and package coverage.
  LAYERC_DISPOSABLE_ROOT=1   Set by the container launcher (tools/ci-linux.sh):
                             as root inside a container, the package stage may
                             install, smoke and remove the package there.
EOF
}

source_dir=
build_dir=
evidence_dir=
flatpak_policy=
flatpak_only=false
editor_policy=
package_policy=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-dir) source_dir=${2:?}; shift 2 ;;
    --build-dir) build_dir=${2:?}; shift 2 ;;
    --evidence-dir) evidence_dir=${2:?}; shift 2 ;;
    --flatpak) flatpak_policy=${2:?}; shift 2 ;;
    --flatpak-only) flatpak_only=true; shift ;;
    --editor) editor_policy=${2:?}; shift 2 ;;
    --package) package_policy=${2:?}; shift 2 ;;
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
release_or() {
  [[ ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]] && echo required || echo "$1"
}
editor_policy=${editor_policy:-$(release_or auto)}
package_policy=${package_policy:-$(release_or off)}
for policy in "$editor_policy" "$package_policy"; do
  case "$policy" in off | auto | required) ;; *)
    echo "error: --editor and --package must be off, auto, or required" >&2
    exit 64
  esac
done
if [[ ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]] &&
    [[ $editor_policy != required || $package_policy != required ]]; then
  echo "error: release evidence requires --editor required and --package required" >&2
  exit 64
fi
if [[ $package_policy == required && $editor_policy != required ]]; then
  echo "error: a required package needs --editor required" >&2
  exit 64
fi
if [[ $flatpak_only == true ]] &&
    [[ $flatpak_policy != required || $package_policy != off ||
       ${LAYERC_RELEASE_EVIDENCE:-0} == 1 ]]; then
  echo "error: --flatpak-only needs --flatpak required, --package off and no release evidence" >&2
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
    FLATPAK_POLICY="$flatpak_policy" EDITOR_POLICY="$editor_policy" \
    PACKAGE_POLICY="$package_policy" python3 - <<'PY'
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
    "editorPolicy": os.environ["EDITOR_POLICY"],
    "packagePolicy": os.environ["PACKAGE_POLICY"],
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
  fltk-config --version 2>/dev/null | sed 's/^/fltk /' || true
  pkg-config --modversion atk-bridge-2.0 2>/dev/null | sed 's/^/atk-bridge-2.0 /' || true
  dpkg-deb --version 2>/dev/null | head -n1 || true
} >"$evidence_dir/tool-versions.txt"

# The editor smoke runs in its own session bus, so its accessibility bridge
# starts (and must shut down) against a private bus and never autolaunches one.
editor_session=(env DBUS_SESSION_BUS_ADDRESS=disabled:)
command -v dbus-run-session >/dev/null && editor_session=(dbus-run-session --)
run_editor_smoke() {
  xvfb-run -a "${editor_session[@]}" bash -c \
    'source "$1"; shift; layerc_x11_exec 50 0.2 "$@"' _ "$source_dir/tools/linux-lib.sh" \
    bash "$source_dir/tools/linux-editor-smoke.sh" "$@"
}

# Statically linked miniz must ship its MIT notice in every Linux install tree.
assert_notices() {
  local root=$1 notice
  for notice in THIRD_PARTY_NOTICES.md miniz/LICENSE miniz/UPSTREAM.md; do
    if [[ ! -s $root/share/licenses/LayerC/$notice ]]; then
      echo "error: $root lacks share/licenses/LayerC/$notice" >&2
      return 1
    fi
  done
  echo "third-party notices present under $root/share/licenses/LayerC"
}

run_logged report-helper-tests \
  python3 -m unittest discover -s "$source_dir/tools/tests"
run_logged shell-helper-tests bash "$source_dir/tools/tests/test_linux_lib.sh"

# Explicit editor options override a reused cache; a required editor that
# cannot be built fails configuration instead of silently shrinking coverage.
case $editor_policy in
  off) editor_cmake=(-DLAYERC_BUILD_EDITOR=OFF -DLAYERC_REQUIRE_EDITOR=OFF) ;;
  auto) editor_cmake=(-DLAYERC_BUILD_EDITOR=ON -DLAYERC_REQUIRE_EDITOR=OFF) ;;
  required) editor_cmake=(-DLAYERC_BUILD_EDITOR=ON -DLAYERC_REQUIRE_EDITOR=ON) ;;
esac
native_editor_cmake=("${editor_cmake[@]}")
[[ $flatpak_only == false ]] ||
  native_editor_cmake=(-DLAYERC_BUILD_EDITOR=OFF -DLAYERC_REQUIRE_EDITOR=OFF)
run_logged configure cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLAYERC_BUILD_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$build_dir/install" "${native_editor_cmake[@]}"
if [[ $flatpak_only == true ]]; then
  # The native Linux jobs own the build, CTest, and native smokes; the Flatpak
  # smoke only needs the fixture window from the native tree.
  run_logged build cmake --build "$build_dir" --parallel \
    --target layerc_linux_x11_smoke_fixture
  record_stage native not-run \
    'flatpak-only mode; native coverage is recorded by the native Linux jobs'
  record_stage editor not-run \
    'flatpak-only mode; the Flatpak stage runs its own editor smoke'
  record_stage package not-run 'flatpak-only mode'
else
  # A reused tree must not let a stale editor binary or install tree stand in
  # for this configuration's result.
  rm -rf -- "$build_dir/layere" "$build_dir/install"
  run_logged build cmake --build "$build_dir" --parallel
  run_logged install cmake --install "$build_dir"
  run_logged install-notices assert_notices "$build_dir/install"

  ctest --test-dir "$build_dir" --show-only=json-v1 \
    >"$evidence_dir/ctest-registration.json"

  headless_junit="$evidence_dir/ctest-headless.xml"
  run_logged ctest-headless env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET ctest --test-dir "$build_dir" \
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
  run_logged ctest-x11 env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET LAYERC_DISPLAY_BACKEND=x11 xvfb-run -a bash -c \
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

  if [[ $editor_policy == off ]]; then
    record_stage editor not-run 'disabled by policy; no editor coverage was claimed'
  elif [[ ! -x $build_dir/layere ]]; then
    editor_reason=$(grep -o 'Linux LayerE not built: .*' "$evidence_dir/configure.log" |
      head -n1 || true)
    record_stage editor unavailable "${editor_reason:-layere was not built}"
    if [[ $editor_policy == required ]]; then
      echo "error: required editor coverage unavailable: ${editor_reason:-layere was not built}" >&2
      exit 1
    fi
  else
    editor_junit="$evidence_dir/ctest-editor.xml"
    run_logged ctest-editor env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET LAYERC_DISPLAY_BACKEND=x11 xvfb-run -a "${editor_session[@]}" bash -c \
      'source "$1"; shift; layerc_x11_exec 50 0.2 "$@"' _ "$source_dir/tools/linux-lib.sh" \
      ctest --test-dir "$build_dir" -L '^editor$' --output-on-failure --no-tests=error \
      --output-junit "$editor_junit"
    run_logged verify-editor python3 "$source_dir/tools/linux_test_report.py" \
      --registration "$evidence_dir/ctest-registration.json" \
      --junit "$editor_junit" --gate linux-editor \
      --report "$evidence_dir/ctest-editor-summary.json"
    run_logged native-editor-smoke run_editor_smoke "$build_dir/install/bin" \
      "$evidence_dir/native-editor" "$source_dir/examples/lctest.json"
    record_stage editor pass
  fi

  package_reason=
  package_editor=no
  [[ $editor_policy != off && -x $build_dir/layere ]] && package_editor=yes
  if [[ $package_policy == off ]]; then
    record_stage package not-run 'disabled by policy; no package coverage was claimed'
  else
    for tool in cpack file dpkg-deb dpkg-shlibdeps desktop-file-validate appstreamcli; do
      command -v "$tool" >/dev/null || { package_reason="$tool is unavailable"; break; }
    done
    # A package without the editor is never a release package.
    if [[ -z $package_reason && $package_editor == no ]]; then
      package_reason='the editor was disabled or not built, so a release package cannot be built'
    fi
  fi
  if [[ $package_policy != off && -n $package_reason ]]; then
    record_stage package unavailable "$package_reason"
    if [[ $package_policy == required ]]; then
      echo "error: required package coverage unavailable: $package_reason" >&2
      exit 1
    fi
  elif [[ $package_policy != off ]]; then
    # A separate /usr-prefixed release tree: the optional user unit embeds the
    # configured path, so the test tree's prefix never reaches a package.
    package_build="$build_dir-package"
    package_out="$evidence_dir/packages"
    mkdir -p "$package_out"
    run_logged package-configure cmake -S "$source_dir" -B "$package_build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DLAYERC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr -DLAYERC_INSTALL_SYSTEMD_USER_UNIT=ON \
      -DLAYERC_BUILD_EDITOR=ON -DLAYERC_REQUIRE_EDITOR=ON
    run_logged package-build cmake --build "$package_build" --parallel
    rm -rf -- "$package_build/cpack"
    run_logged package-deb cpack --config "$package_build/CPackConfig.cmake" \
      -B "$package_build/cpack"
    debs=("$package_build"/cpack/layerc_*.deb)
    if [[ ${#debs[@]} -ne 1 || ! -f ${debs[0]} ]]; then
      echo "error: CPack must produce exactly one layerc package" >&2
      exit 1
    fi
    cp -- "${debs[0]}" "$package_out/"
    deb=$package_out/$(basename -- "${debs[0]}")
    package_version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$package_build/CMakeCache.txt")
    run_logged package-check python3 "$source_dir/tools/linux_package_check.py" "$deb" \
      --version "$package_version" --editor yes \
      --report "$evidence_dir/package-check.json"
    # Installing is confined to a container its launcher declared disposable.
    if [[ $(id -u) -eq 0 && ${LAYERC_DISPOSABLE_ROOT:-0} == 1 ]] &&
        [[ -e /.dockerenv || -e /run/.containerenv ]]; then
      run_logged package-install env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y --no-install-recommends "$deb"
      {
        dpkg -s layerc
        apt-cache policy libatk-bridge2.0-0t64 libatspi2.0-0t64 libfltk1.4 2>/dev/null || true
      } >"$evidence_dir/package-installed.txt"
      run_logged package-headless-smoke env -u DISPLAY \
        bash "$source_dir/tools/linux-runtime-smoke.sh" headless \
        /usr/bin "$evidence_dir/package-headless" "$source_dir/examples/lctest.json"
      run_logged package-x11-smoke xvfb-run -a \
        bash "$source_dir/tools/linux-runtime-smoke.sh" x11 \
        /usr/bin "$evidence_dir/package-x11" \
        "$source_dir/examples/lctest.json" "$build_dir/layerc_linux_x11_smoke_fixture"
      run_logged package-editor-smoke run_editor_smoke /usr/bin \
        "$evidence_dir/package-editor" "$source_dir/examples/lctest.json"
      run_logged package-remove env DEBIAN_FRONTEND=noninteractive \
        apt-get remove -y layerc
      if dpkg -s layerc >/dev/null 2>&1 &&
          [[ $(dpkg-query -W -f='${db:Status-Status}' layerc) == installed ]]; then
        echo "error: layerc is still installed after removal" >&2
        exit 1
      fi
      for installed in /usr/bin/layerd /usr/bin/layerctl /usr/bin/layercal \
          /usr/bin/layere /usr/share/applications/io.github.EnderKeera.LayerC.desktop \
          /usr/share/metainfo/io.github.EnderKeera.LayerC.metainfo.xml \
          /usr/share/icons/hicolor/256x256/apps/io.github.EnderKeera.LayerC.png; do
        if [[ -e $installed ]]; then
          echo "error: removing the package left $installed" >&2
          exit 1
        fi
      done
      record_stage package pass
    else
      package_reason='installing the package needs a disposable container root (root with LAYERC_DISPOSABLE_ROOT=1)'
      record_stage package unavailable "$package_reason"
      if [[ $package_policy == required ]]; then
        echo "error: required package coverage unavailable: $package_reason" >&2
        exit 1
      fi
    fi
  fi
fi

run_flatpak=false
flatpak_reason=
if [[ $flatpak_policy == off ]]; then
  flatpak_reason='disabled by policy; no Flatpak coverage was claimed'
elif ! command -v flatpak >/dev/null || ! command -v flatpak-builder >/dev/null; then
  flatpak_reason='flatpak or flatpak-builder is unavailable'
elif ! flatpak --system info org.freedesktop.Platform//25.08 >/dev/null 2>&1 ||
     ! flatpak --system info org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
  # The build uses a private user installation, so only system runtimes count.
  flatpak_reason='org.freedesktop Platform/SDK 25.08 is not installed system-wide'
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
    flatpak --system info org.freedesktop.Platform//25.08
    flatpak --system info org.freedesktop.Sdk//25.08
  } >"$evidence_dir/flatpak-runtime-refs.txt"
  python3 - "$manifest" >"$evidence_dir/flatpak-permissions.json" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
manifest = json.loads(path.read_text(encoding="utf-8"))
actual = manifest.get("finish-args", [])
expected = [
    "--socket=x11",
    "--filesystem=xdg-documents",
    "--filesystem=xdg-download",
]
result = {"expected": expected, "actual": actual, "result": "pass" if actual == expected else "fail"}
print(json.dumps(result, indent=2))
if actual != expected:
    raise SystemExit("Flatpak permission boundary changed")
PY
  app_id=io.github.EnderKeera.LayerC
  # Outside the source tree, which may be read-only; removed after success.
  flatpak_root=$(mktemp -d "$build_dir-flatpak.XXXXXX")
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
  # Keep the builder's download/build cache inside the disposable root, not
  # in the caller's working directory (it must share the stage's filesystem).
  # That cache is per-run, so the rofiles-fuse guard against corrupting a
  # shared one is not needed, and the gate also runs where FUSE is not.
  run_logged flatpak-build flatpak-builder --force-clean --disable-rofiles-fuse \
    --state-dir="$flatpak_root/state" \
    --repo="$flatpak_repo" "$flatpak_build" "$manifest"
  run_logged flatpak-notices assert_notices "$flatpak_build/files"
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
  if [[ $editor_policy == off ]]; then
    record_stage flatpak-editor-smoke not-run 'editor disabled by policy'
  else
    run_logged flatpak-editor-smoke run_editor_smoke "flatpak:$app_id" \
      "$evidence_dir/flatpak-editor" "$source_dir/examples/lctest.json"
  fi
  mkdir -p "$evidence_dir/packages"
  run_logged flatpak-bundle flatpak build-bundle "$flatpak_repo" \
    "$evidence_dir/packages/LayerC.flatpak" "$app_id"
  rm -rf -- "$flatpak_root"
  record_stage flatpak pass
fi

record_stage overall pass
echo "Linux automation evidence: $evidence_dir"
