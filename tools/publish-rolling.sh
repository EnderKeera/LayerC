#!/usr/bin/env bash
# Publish the Windows ZIP from this green main commit as the rolling prerelease.
#
# Usage: publish-rolling.sh <artifact-dir>
#
# Requires gh with GH_TOKEN set, and a git checkout (gh resolves owner/repo
# from the git remote). Runnable locally for a dry review of the state
# machine; CI calls it after windows-msvc + linux are both green.
set -Eeuo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <artifact-dir>" >&2
  exit 64
fi
input=$1

mapfile -t packages < <(find "$input" -type f -name '*.zip' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
  echo "Expected one rolling package, found ${#packages[@]}." >&2
  exit 1
fi

asset="$input/layerc-windows-x64.zip"
mv "${packages[0]}" "$asset"
notes="Rolling Windows x64 build of main@${GITHUB_SHA:-local}; windows-msvc and linux both green. Linux V2.6 remains pre-release with no packaged artifact."

if gh release view rolling >/dev/null 2>&1; then
  gh release upload rolling "$asset" --clobber
  while IFS= read -r old_asset; do
    if [[ -n "$old_asset" && "$old_asset" != 'layerc-windows-x64.zip' ]]; then
      gh release delete-asset rolling "$old_asset" --yes
    fi
  done < <(gh release view rolling --json assets --jq '.assets[].name')
  gh release edit rolling --target "${GITHUB_SHA:-}" \
    --title 'LayerC rolling build' --prerelease --notes "$notes"
else
  gh release create rolling "$asset" --target "${GITHUB_SHA:-}" \
    --title 'LayerC rolling build' --prerelease --notes "$notes"
fi
