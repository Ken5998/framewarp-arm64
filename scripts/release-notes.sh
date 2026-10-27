#!/usr/bin/env bash
# Prints the changes since the previous release as Markdown for the release
# notes: the conventional commits between the last vX.Y.Z tag and REV, grouped
# by type. Build, CI and release chores are left out.
#
# Usage: scripts/release-notes.sh TAG [REV]   (TAG is the release being made)
set -euo pipefail

tag="$1"
rev="${2:-HEAD}"
prev="$(git tag --list 'v[0-9]*' --merged "$rev" --sort=-v:refname | grep -vxF "$tag" | head -1 || true)"
range="${prev:+$prev..}$rev"

declare -A items=()
while IFS= read -r subject; do
  re='^([a-z]+)(\(([^)]*)\))?!?: (.+)$'
  [[ $subject =~ $re ]] || continue
  type="${BASH_REMATCH[1]}" text="${BASH_REMATCH[4]}"
  case "$type" in feat|fix|perf|docs) ;; *) continue ;; esac
  text="${text^}"
  items[$type]+="- $text"$'\n'
done < <(git log --no-merges --reverse --format=%s "$range")

if [ -n "$prev" ]; then
  echo "## What's new since $prev"
else
  echo "## What's new"
fi
echo
if [ ${#items[@]} -eq 0 ]; then
  echo "Build and maintenance updates only, nothing that changes the layer."
  echo
fi
for section in "feat:New" "fix:Fixes" "perf:Performance" "docs:Guide"; do
  type="${section%%:*}"
  [ -n "${items[$type]:-}" ] || continue
  echo "### ${section#*:}"
  echo
  printf '%s\n' "${items[$type]}"
done
