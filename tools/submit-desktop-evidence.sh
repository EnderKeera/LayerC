#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo 'Usage: submit-desktop-evidence.sh <pr-number|-> <gnome|kde> <native|flatpak> <window-xid|-> [--dry-run] [--commit SHA] [--repository OWNER/REPO] [--from-evidence DIR] [--flatpak-commit HEX] [--machine physical|vm]'
}
fail() { echo "error: $*" >&2; exit 1; }

if [[ $# -lt 4 ]]; then
  usage >&2
  exit 64
fi
pr=$1
desktop=$2
mode=$3
xid=$4
shift 4
[[ $pr == - || $pr =~ ^[1-9][0-9]*$ ]] || fail 'PR must be a positive number or -.'
case "$desktop" in gnome | kde) ;; *) fail 'Desktop must be gnome or kde.' ;; esac
case "$mode" in native | flatpak) ;; *) fail 'Package mode must be native or flatpak.' ;; esac
commit=
repository=
from_evidence=
flatpak_commit=
machine=physical
dry_run=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --commit | --repository | --from-evidence | --flatpak-commit | --machine)
      [[ $# -ge 2 ]] || fail "Missing value for $1."
      case "$1" in
        --commit) commit=$2 ;;
        --repository) repository=$2 ;;
        --from-evidence) from_evidence=$2 ;;
        --flatpak-commit) flatpak_commit=$2 ;;
        --machine) machine=$2 ;;
      esac
      shift 2
      ;;
    *) usage >&2; fail "Unknown option: $1" ;;
  esac
done
case "$machine" in physical | vm) ;; *) fail 'Machine must be physical or vm.' ;; esac
[[ $dry_run == 1 || $pr != - ]] || fail 'A PR number is required unless --dry-run is set.'
PYTHON=${PYTHON:-python3}
if [[ -n $from_evidence ]]; then
  from_evidence=$(cd -- "$from_evidence" && pwd)
fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd -- "$root"
if [[ -z $repository ]]; then
  repository=$(gh repo view --json nameWithOwner -q .nameWithOwner)
fi
[[ $repository =~ ^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$ ]] || fail 'Repository must be OWNER/REPO.'
if [[ -z $commit ]]; then
  [[ $pr != - ]] || fail 'Specify --commit or a PR number.'
  commit=$(gh pr view "$pr" --repo "$repository" --json headRefOid,state,mergeCommit \
    -q 'if .state == "MERGED" then .mergeCommit.oid else .headRefOid end')
fi
[[ $commit =~ ^[0-9a-fA-F]{40}$ ]] || fail 'Commit must be a full 40-digit hex SHA.'
commit=${commit,,}
mkdir -p -- "$root/out/desktop-evidence"
stage=$(mktemp -d "$root/out/desktop-evidence/linux-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
if [[ -n $from_evidence ]]; then
  evidence=$from_evidence
  if [[ $mode == flatpak ]]; then
    [[ $flatpak_commit =~ ^([0-9a-f]{40}|[0-9a-f]{64})$ ]] || fail 'Flatpak evidence requires --flatpak-commit (40 or 64 lowercase hex digits).'
  fi
else
  head=$(git rev-parse --verify HEAD)
  [[ $head == "$commit" ]] || fail "Checkout HEAD ($head) does not match $commit."
  changes=$(git status --porcelain)
  [[ -z $changes ]] || fail 'Tracked or untracked changes are present; use a clean checkout.'
  if [[ $mode == flatpak ]]; then
    command -v flatpak >/dev/null || fail 'flatpak is required.'
    command -v flatpak-builder >/dev/null || fail 'flatpak-builder is required.'
    # Automation uses a disposable Flatpak install; the physical run needs this checkout installed for the user.
    flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
      --state-dir="$stage/flatpak-state" "$stage/flatpak-build" packaging/flatpak/io.github.EnderKeera.LayerC.json
    flatpak_commit=$(flatpak info --user --show-commit io.github.EnderKeera.LayerC)
    [[ $flatpak_commit =~ ^([0-9a-f]{40}|[0-9a-f]{64})$ ]] || fail 'flatpak returned an invalid installed commit.'
    printf '%s\n' "$flatpak_commit" >"$stage/flatpak-installed-commit.txt"
  fi
  rc=0
  LAYERC_RUN_AUTOMATION=1 LAYERC_REQUIRE_AUTOMATION=1 LAYERC_EVIDENCE_DIR="$stage" \
    bash tools/run-linux-v26-evidence.sh "$desktop" "$mode" "$xid" || rc=$?
  if [[ $rc -ne 0 && $rc -ne 2 ]]; then
    exit "$rc"
  fi
  shopt -s nullglob
  directories=("$stage"/*-"$desktop"-"$mode"/)
  [[ ${#directories[@]} -eq 1 ]] || fail 'Expected exactly one evidence directory from the harness.'
  evidence=${directories[0]%/}
fi
os=$("$PYTHON" - <<'PY'
from pathlib import Path

path = Path("/etc/os-release")
if path.exists():
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("PRETTY_NAME="):
            print(line.partition("=")[2].strip('"\''))
            break
PY
)
if [[ -z $os ]]; then
  os=$(uname -sr)
fi
report="$stage/report.json"
comment="$stage/comment.md"
adapter_arguments=()
if [[ $mode == flatpak ]]; then
  adapter_arguments+=(--flatpak-commit "$flatpak_commit")
fi
"$PYTHON" tools/desktop_evidence_layerc.py linux-report --evidence "$evidence" \
  --desktop "$desktop" --package-mode "$mode" --commit "$commit" \
  --repository "$repository" --os "$os" --machine "$machine" --out "$report" "${adapter_arguments[@]}"
"$PYTHON" tools/desktop_evidence.py comment --config .github/desktop-evidence.json \
  --report "$report" --repository "$repository" --out "$comment"
if [[ $dry_run == 1 ]]; then
  cat -- "$comment"
  printf 'Dry run: not posted. Comment: %s\n' "$comment"
else
  gh pr comment "$pr" --repo "$repository" --body-file "$comment"
fi
