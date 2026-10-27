#!/usr/bin/env bash
# Installs the framewarp layer for the current user: the library in
# ~/.local/share/framewarp and its manifest in
# ~/.local/share/vulkan/implicit_layer.d. The layer stays off until a game is
# started with FRAMEWARP=1.
#
# Run it from an extracted release package, or from the repository after
# scripts/build.sh. --uninstall removes both files.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dest="$HOME/.local/share/framewarp"
manifest="$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_framewarp.json"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$manifest" "$dest/libVkLayer_framewarp.so"
  rmdir "$dest" 2>/dev/null || true
  echo "framewarp removed"
  exit 0
fi

lib="" template=""
for dir in "$here" "$here/../build" "$here/.."; do
  [ -z "$lib" ] && [ -f "$dir/libVkLayer_framewarp.so" ] && lib="$dir/libVkLayer_framewarp.so"
  [ -z "$template" ] && [ -f "$dir/VkLayer_framewarp.json.in" ] && template="$dir/VkLayer_framewarp.json.in"
  [ -z "$template" ] && [ -f "$dir/layer/VkLayer_framewarp.json.in" ] && template="$dir/layer/VkLayer_framewarp.json.in"
done
if [ -z "$lib" ] || [ -z "$template" ]; then
  echo "libVkLayer_framewarp.so or its manifest not found next to this script" >&2
  exit 1
fi

version="$(cat "$here/VERSION" 2>/dev/null || cat "$here/../VERSION")"
mkdir -p "$dest" "$(dirname "$manifest")"
install -m 0755 "$lib" "$dest/libVkLayer_framewarp.so"
sed -e "s|@FRAMEWARP_LIBRARY_PATH@|$dest/libVkLayer_framewarp.so|" "$template" > "$manifest"
echo "framewarp $version installed in $dest"

cat <<TEXT

The layer is off until a game starts with FRAMEWARP=1. In Steam, put the
variables in front of the launch options, for example:

  FRAMEWARP=1 FRAMEWARP_MULT=3 %command%

Each game run appends its measurements to ~/framewarp.log.
TEXT
