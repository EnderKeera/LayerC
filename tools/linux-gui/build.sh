#!/usr/bin/env bash
# Mirror a checkout's file list into Linux storage and build the GUI runner's
# targets there. Used by tools/test-linux-gui.ps1: Debian WSL may lack Git,
# and building on the Windows-mounted tree is slow.
#
#   build.sh --repo DIR --files LIST --source DIR --build DIR [--env FILE]
#            [--stamp FILE] [-- CMAKE_ARGUMENT...]
#
# --stamp records the built executables' SHA-256 digests; the runner's
# --expect-stamp refuses to run anything else.
#
# The mirror is exact, so it deletes files absent from LIST. It therefore only
# ever writes into a dedicated mirror: a new or empty directory, which it
# marks, or one that already carries that marker. Home, root, the checkout and
# overlapping source/build directories are refused.
set -euo pipefail
# WSLENV can forward a Windows HOME; resolve the account's own home.
HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
export HOME
marker=.layerc-gui-mirror
repo= files= source_dir= build= env_file= stamp=
while (($#)); do
  case $1 in
    --repo) repo=$2; shift 2 ;;
    --files) files=$2; shift 2 ;;
    --source) source_dir=$2; shift 2 ;;
    --build) build=$2; shift 2 ;;
    --env) env_file=$2; shift 2 ;;
    --stamp) stamp=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done
[[ -n $repo && -n $files && -n $source_dir && -n $build ]] || {
  echo "usage: $0 --repo DIR --files LIST --source DIR --build DIR [--env FILE] [-- ARGS]" >&2
  exit 64
}
expand_home() { [[ $1 == "~"* ]] && printf '%s' "$HOME${1:1}" || printf '%s' "$1"; }
repo=$(realpath -e "$repo")
source_dir=$(realpath -m "$(expand_home "$source_dir")")
build=$(realpath -m "$(expand_home "$build")")
home=$(realpath -m "$HOME")
# overlaps A B: A equals B, or either contains the other.
overlaps() { [[ $1 == "$2" || $1 == "$2"/* || $2 == "$1"/* ]]; }
refuse() { echo "refusing $1" >&2; exit 64; }
[[ $source_dir != / && $build != / ]] || refuse "the filesystem root as a mirror or build directory"
for protected in "$home" "$repo"; do
  if [[ $source_dir == "$protected" || $protected == "$source_dir"/* ]]; then
    refuse "mirror $source_dir: it is or contains $protected"
  fi
  if [[ $build == "$protected" || $protected == "$build"/* ]]; then
    refuse "build directory $build: it is or contains $protected"
  fi
done
overlaps "$source_dir" "$repo" && refuse "mirror $source_dir overlapping the checkout"
overlaps "$build" "$source_dir" && refuse "build directory overlapping the mirror"
overlaps "$build" "$repo" && refuse "build directory overlapping the checkout"
if [[ -e $source_dir ]]; then
  [[ -d $source_dir && ! -L $source_dir ]] || refuse "mirror $source_dir: not a directory"
  if [[ ! -f "$source_dir/$marker" ]] && [[ -n $(find "$source_dir" -mindepth 1 -print -quit) ]]; then
    refuse "to mirror into unmarked, nonempty $source_dir (choose a dedicated directory)"
  fi
fi
if [[ -n $env_file ]]; then
  # shellcheck source=/dev/null
  source "$(expand_home "$env_file")"
fi
for command in rsync cmake ninja flock; do
  command -v "$command" >/dev/null || { echo "missing required dependency: $command" >&2; exit 1; }
done
# One run per mirror/build pair: refuse while a runner (shared lock, held for
# the whole session) or another build uses them.
mkdir -p "$(dirname "$build")" "$(dirname "$source_dir")"
exec 8>>"$source_dir.lock" 9>>"$build.lock"
if ! flock -n 8 || ! flock -n 9; then
  echo "another LayerC GUI run is using $source_dir / $build" >&2
  exit 75
fi

list=$(mktemp)
trap 'rm -f -- "$list"' EXIT
tr -d '\r' < "$files" | LC_ALL=C sort -u > "$list"
mkdir -p "$source_dir"
printf 'LayerC GUI runner mirror of %s; contents are replaced on every run.\n' "$repo" \
  > "$source_dir/$marker"
# Every listed file must exist: the caller lists only files present in the
# checkout, so a missing input is an error rather than a stale mirror copy.
rsync -a --files-from="$list" "$repo/" "$source_dir/"
# Keep the mirror exact: drop files and symlinks (never following them)
# that left the checkout's file list, then any directories left empty.
(cd "$source_dir" && find . \( -type f -o -type l \) ! -path "./$marker" -printf '%P\n' |
  LC_ALL=C sort) |
  LC_ALL=C comm -23 - "$list" |
  while IFS= read -r stale; do rm -f -- "$source_dir/$stale"; done
find "$source_dir" -mindepth 1 -type d -empty -delete
# A Windows checkout may hold CRLF scripts; match a Linux checkout.
find "$source_dir" -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r$//' {} +

cmake -S "$source_dir" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLAYERC_BUILD_TESTS=ON "$@" >/dev/null
cmake --build "$build" --target layere layerd layerctl layerc_linux_x11_smoke_fixture
if [[ -n $stamp ]]; then
  (cd "$build" && sha256sum layere layerd layerctl layerc_linux_x11_smoke_fixture) > "$stamp"
fi
