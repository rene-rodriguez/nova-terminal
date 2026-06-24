#!/usr/bin/env bash
# Regenerates the macOS app icon (assets/fangs.icns) from the source artwork.
#
# Source of truth: assets/fangs-icon.png (a 1024x1024 hand-edited PNG). Edit that
# file, then run this script to rebuild the multi-resolution .icns the bundle
# installs (see scripts/macos-bundle.sh, CFBundleIconFile=fangs).
#
#   scripts/macos-icon.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/assets/fangs-icon.png"
OUT="$ROOT/assets/fangs.icns"

[ -f "$SRC" ] || { echo "missing source artwork: $SRC" >&2; exit 1; }

ICONSET="$(mktemp -d)/fangs.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z "$s" "$s" "$SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  d=$((s * 2))
  sips -z "$d" "$d" "$SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
cp "$SRC" "$ICONSET/icon_512x512@2x.png"
iconutil -c icns "$ICONSET" -o "$OUT"
echo "wrote $OUT"
