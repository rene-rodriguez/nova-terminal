#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
README="$ROOT/README.md"
CASK="$ROOT/packaging/macos/fangs.rb"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -Fq '### Quick install (macOS app, Linux CLI)' "$README" \
  || fail "README must label the curl installer as the macOS app install path"

grep -Fq 'macOS installs `Fangs.app` under `~/Applications`' "$README" \
  || fail "README must say the curl installer installs Fangs.app on macOS"

grep -Fq 'FANGS_INSTALL=cli' "$README" \
  || fail "README must document the macOS CLI override"

grep -Fq 'FANGS_VERSION=v0.1.3 sh' "$README" \
  || fail "README pinned-version example must use the current release"

grep -Fq 'version "0.1.3"' "$CASK" \
  || fail "macOS cask must point at the current app bundle release"

grep -Fq 'sha256 "adf4647e681200f691e60cbdc1f8bf4c57ead8029ec57e37a72f89fb9ad41c97"' "$CASK" \
  || fail "macOS cask sha256 must match the v0.1.3 unsigned app zip"

grep -Fq 'fangs-#{version}-macos-arm64-unsigned.zip' "$CASK" \
  || fail "macOS cask must use the current unsigned tester app zip while Developer ID is unavailable"

echo "macos_app_install_docs_tests: ok"
