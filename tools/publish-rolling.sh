#!/usr/bin/env bash
# Publish the Windows ZIP from this green main commit as the rolling prerelease.
#
# Usage: publish-rolling.sh <artifact-dir>
#        publish-rolling.sh --verify
#
# Requires gh with GH_TOKEN set, and a git checkout (gh resolves owner/repo
# from the git remote). CI calls it after every gate is green, serialized by
# the publish-rolling concurrency group.
#
# GitHub cannot replace a release's assets, tag, and notes atomically, so the
# publication is a staged promotion whose commit point is the notes edit:
#
#   1. Refuse to replace a newer publication (out-of-order main runs).
#   2. Upload the package under a non-ZIP staging name and verify the bytes
#      GitHub serves. Consumers only download *.zip, so staging is invisible.
#   3. Swap: delete every published ZIP, then rename staging to the one
#      canonical name.
#   4. Move the rolling tag explicitly. `gh release edit --target` only sets
#      the target for a tag that does not exist yet; it never moves one.
#   5. Commit: write notes naming the commit and the package SHA-256.
#
# A consumer accepts rolling only when the tag, the commit in the notes, and
# the SHA-256 of the single ZIP all agree (`--verify`, tools/pull-ci.ps1).
# Every interruption therefore leaves either the previous consistent
# publication or a detectably inconsistent one; rerunning the job converges
# because each step is idempotent and stale staging is removed first.
set -Eeuo pipefail
# Byte ranges: in many locales [a-f] also matches upper-case letters.
export LC_ALL=C

tag=rolling
canonical=layerc-windows-x64.zip
api="repos/{owner}/{repo}"
assets_query='.assets[] | [.id, .name] | @tsv'

fail() {
  echo "publish-rolling: $*" >&2
  exit 1
}

sha256_of() {
  sha256sum "$1" | cut -d' ' -f1
}

release_exists() {
  gh api "$api/releases/tags/$tag" --jq .id >/dev/null 2>&1
}

tag_commit() {
  gh api "$api/git/ref/tags/$tag" --jq .object.sha 2>/dev/null || true
}

# Prints "<id>\t<name>" for each asset of the rolling release.
list_assets() {
  gh api "$api/releases/tags/$tag" --jq "$assets_query"
}

# Downloads one asset and prints the SHA-256 of the bytes GitHub serves.
served_sha256() {
  local name=$1 directory
  directory=$(mktemp -d)
  gh release download "$tag" --pattern "$name" --dir "$directory" >/dev/null
  [[ -f "$directory/$name" ]] || fail "asset $name could not be downloaded"
  sha256_of "$directory/$name"
  rm -rf "$directory"
}

# Exits 0 only for a complete, mutually consistent publication.
verify_publication() {
  local body tag_sha claimed_sha claimed_digest zips served
  release_exists || fail "no $tag release exists"
  tag_sha=$(tag_commit)
  body=$(gh api "$api/releases/tags/$tag" --jq .body)
  claimed_sha=$(grep -oE 'main@[0-9a-f]{40}' <<<"$body" | head -n1 | cut -d@ -f2 || true)
  claimed_digest=$(grep -oE "$canonical SHA-256: [0-9a-f]{64}" <<<"$body" |
    head -n1 | awk '{print $3}' || true)
  zips=$(list_assets | cut -f2 | grep -cE '\.zip$' || true)
  [[ -n "$tag_sha" && "$tag_sha" == "$claimed_sha" ]] ||
    fail "tag ($tag_sha) and notes (${claimed_sha:-none}) name different commits"
  [[ "$zips" == 1 ]] && list_assets | cut -f2 | grep -qx "$canonical" ||
    fail "expected exactly one ZIP named $canonical, found $zips ZIP asset(s)"
  served=$(served_sha256 "$canonical")
  [[ -n "$claimed_digest" && "$served" == "$claimed_digest" ]] ||
    fail "served $canonical ($served) does not match the notes (${claimed_digest:-none})"
  echo "publish-rolling: $tag consistently publishes main@$tag_sha ($canonical $served)"
}

if [[ $# -eq 1 && $1 == --verify ]]; then
  verify_publication
  exit 0
fi
if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <artifact-dir> | --verify" >&2
  exit 64
fi
input=$1
sha=${GITHUB_SHA:-}
[[ $sha =~ ^[0-9a-f]{40}$ ]] || fail "GITHUB_SHA must name the tested commit"

mapfile -t packages < <(find "$input" -type f -name '*.zip' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
  fail "expected one rolling package, found ${#packages[@]}"
fi
staged="layerc-windows-x64.$sha.staged"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "${packages[0]}" "$work/$staged"
digest=$(sha256_of "$work/$staged")
notes="Rolling Windows x64 build of main@$sha; every ci.yml gate is green. Linux V2.6 is source/CLI only, with no packaged artifact.

$canonical SHA-256: $digest"
title='LayerC rolling build'

if ! release_exists; then
  # A first publication creates tag, asset, and notes together.
  cp "$work/$staged" "$work/$canonical"
  gh release create "$tag" "$work/$canonical" --target "$sha" \
    --title "$title" --prerelease --notes "$notes"
  verify_publication
  exit 0
fi

current=$(tag_commit)
if [[ -n "$current" && "$current" != "$sha" ]]; then
  relation=$(gh api "$api/compare/$current...$sha" --jq .status)
  if [[ "$relation" == behind ]]; then
    echo "publish-rolling: $tag already publishes newer main@$current; leaving it unchanged"
    exit 0
  fi
fi

# Remove staging left by an interrupted run before staging this package.
while IFS=$'\t' read -r id name; do
  if [[ "$name" == *.staged ]]; then
    gh api -X DELETE "$api/releases/assets/$id" >/dev/null
  fi
done < <(list_assets)

gh release upload "$tag" "$work/$staged"
[[ "$(served_sha256 "$staged")" == "$digest" ]] ||
  fail "staged package bytes do not match the tested package"
staged_id=$(list_assets | awk -F'\t' -v name="$staged" '$2 == name {print $1}')
[[ -n "$staged_id" ]] || fail "staged package is missing after upload"

while IFS=$'\t' read -r id name; do
  if [[ "$name" == *.zip ]]; then
    gh api -X DELETE "$api/releases/assets/$id" >/dev/null
  fi
done < <(list_assets)
gh api -X PATCH "$api/releases/assets/$staged_id" -f name="$canonical" >/dev/null

if [[ -n "$current" ]]; then
  gh api -X PATCH "$api/git/refs/tags/$tag" -f sha="$sha" -F force=true >/dev/null
else
  gh api -X POST "$api/git/refs" -f ref="refs/tags/$tag" -f sha="$sha" >/dev/null
fi
gh release edit "$tag" --title "$title" --prerelease --notes "$notes"
verify_publication
