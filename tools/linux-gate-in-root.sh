#!/usr/bin/env bash
# Run tools/run-linux-tests.sh as root inside a disposable container root
# (tools/ci-linux.sh). Installs the build dependencies and, unless --editor off
# is selected, the editor dependencies (Debian 13 takes AT-SPI 2.62 from
# trixie-backports). The launcher must declare the root disposable
# (LAYERC_DISPOSABLE_ROOT=1) and the root must be a container; this script
# never declares it on its own, so it cannot install packages on a host.
set -Eeuo pipefail

if [[ $# -lt 3 || $(id -u) -ne 0 ]]; then
  echo "Usage (as root, in a disposable container): $0 <source-dir> <build-dir> <evidence-dir> [run-linux-tests options...]" >&2
  exit 64
fi
if [[ ${LAYERC_DISPOSABLE_ROOT:-0} != 1 ]] ||
    [[ ! -e /.dockerenv && ! -e /run/.containerenv ]]; then
  echo "error: refusing to install packages outside a container launched with LAYERC_DISPOSABLE_ROOT=1" >&2
  exit 64
fi
source_dir=$1
build_dir=$2
evidence_dir=$3
shift 3

editor=auto
arguments=("$@")
for ((i = 0; i < ${#arguments[@]}; ++i)); do
  [[ ${arguments[i]} == --editor ]] && editor=${arguments[i + 1]:-}
done

# One package per line; blank lines, comments and CR line endings are ignored,
# and a final line without a newline still counts.
read_list() {
  local line
  while IFS= read -r line || [[ -n $line ]]; do
    line=${line%%#*}
    line=${line//$'\r'/}
    [[ -n ${line//[[:space:]]/} ]] && printf '%s\n' "$line"
  done <"$1"
}

export DEBIAN_FRONTEND=noninteractive
apt-get update
mapfile -t packages < <(read_list "$source_dir/tools/linux-deps.txt")
apt-get install -y --no-install-recommends "${packages[@]}"

if [[ $editor != off ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  main=()
  backports=()
  while read -r suite package; do
    case $suite in
      main) main+=("$package") ;;
      backports) backports+=("$package") ;;
      *) echo "error: unknown suite $suite in linux-editor-deps.txt" >&2; exit 65 ;;
    esac
  done < <(read_list "$source_dir/tools/linux-editor-deps.txt")
  if [[ ${ID:-} == debian && ${VERSION_CODENAME:-} == trixie ]]; then
    apt-get install -y --no-install-recommends "${main[@]}"
    if ! grep -rqsE '^[^#]*trixie-backports' /etc/apt/sources.list /etc/apt/sources.list.d; then
      echo 'deb http://deb.debian.org/debian trixie-backports main' \
        >/etc/apt/sources.list.d/layerc-trixie-backports.list
      apt-get update
    fi
    apt-get install -y --no-install-recommends -t trixie-backports "${backports[@]}"
  else
    # Elsewhere the distribution's own versions apply, one package at a time
    # and with a visible warning; the CMake gate reports (or, when required,
    # fails on) a missing FLTK 1.4 or anything older than AT-SPI 2.62.
    for package in "${main[@]}" "${backports[@]}"; do
      apt-get install -y --no-install-recommends "$package" ||
        echo "warning: $package is unavailable on this distribution" >&2
    done
  fi
fi

exec bash "$source_dir/tools/run-linux-tests.sh" --source-dir "$source_dir" \
  --build-dir "$build_dir" --evidence-dir "$evidence_dir" "$@"
