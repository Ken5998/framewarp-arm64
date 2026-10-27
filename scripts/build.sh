#!/usr/bin/env bash
# Builds the layer and packs it with its installer as
# OUT/framewarp-VERSION-ARCH.tar.gz.
#
# Usage: scripts/build.sh [OUT]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mkdir -p "${1:-out}" && cd "${1:-out}" && pwd)"
version="$(cat "$root/VERSION")"
arch="$(uname -m)"
build="$root/build"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$(nproc)"

pkg="$(mktemp -d)/framewarp"
mkdir -p "$pkg"
install -m 0755 "$build/libVkLayer_framewarp.so" "$pkg/"
install -m 0644 "$root/layer/VkLayer_framewarp.json.in" "$pkg/"
install -m 0755 "$root/scripts/install-armadaos.sh" "$pkg/install.sh"
install -m 0644 "$root/README.md" "$root/LICENSE" "$pkg/"
echo "$version" > "$pkg/VERSION"
tar -czf "$out/framewarp-$version-$arch.tar.gz" -C "$(dirname "$pkg")" framewarp
echo "$out/framewarp-$version-$arch.tar.gz"
