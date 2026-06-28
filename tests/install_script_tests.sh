#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$ROOT/install.sh"
README="$ROOT/README.md"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -Fq 'FANGS_INSTALL="${FANGS_INSTALL:-auto}"' "$INSTALL" \
  || fail "install.sh must expose FANGS_INSTALL=auto|app|cli"

grep -Fq 'APP_DIR="${FANGS_APP_DIR:-$HOME/Applications}"' "$INSTALL" \
  || fail "install.sh must default app installs to ~/Applications"

grep -Fq 'fangs-${version_no_v}-macos-${a}.zip' "$INSTALL" \
  || fail "install.sh must try the signed macOS app zip name"

grep -Fq 'fangs-${version_no_v}-macos-${a}-unsigned.zip' "$INSTALL" \
  || fail "install.sh must fall back to the unsigned macOS tester app zip"

grep -Fq "printf 'Installed: %s/Fangs.app\\n' \"\$APP_DIR\"" "$INSTALL" \
  || fail "install.sh must install the app bundle, not only the CLI, on macOS"

grep -Fq 'Quick install (macOS app, Linux CLI)' "$README" \
  || fail "README install heading must match macOS app default"

grep -Fq 'macOS installs `Fangs.app` under `~/Applications`' "$README" \
  || fail "README must explain the macOS app install target"

grep -Fq 'FANGS_INSTALL=cli' "$README" \
  || fail "README must document the macOS CLI override"

echo "install_script_tests: ok"
