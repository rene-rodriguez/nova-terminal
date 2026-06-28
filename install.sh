#!/bin/sh
# =============================================================================
# Fangs — one-line installer
#
#   curl -fsSL https://raw.githubusercontent.com/rene-rodriguez/fangs/main/install.sh | sh
#
# Detects OS + arch, downloads the matching release asset, and installs:
#   - macOS: Fangs.app under ~/Applications by default
#   - Linux: CLI binary under ~/.local by default
#
# Private repo? Provide a token with `repo` scope and the installer authenticates
# both the API lookup and the asset download:
#   curl -fsSL .../install.sh | FANGS_GITHUB_TOKEN=ghp_xxx sh
#
# Env:
#   FANGS_INSTALL       auto, app, or cli          (default: auto)
#   FANGS_APP_DIR       macOS app install dir      (default: $HOME/Applications)
#   FANGS_PREFIX        CLI install prefix         (default: $HOME/.local)
#   FANGS_VERSION       release tag to install     (default: latest)
#   FANGS_GITHUB_TOKEN  token for a private repo    (falls back to GH_TOKEN / GITHUB_TOKEN)
# =============================================================================
set -eu

REPO="rene-rodriguez/fangs"
FANGS_INSTALL="${FANGS_INSTALL:-auto}"
APP_DIR="${FANGS_APP_DIR:-$HOME/Applications}"
PREFIX="${FANGS_PREFIX:-$HOME/.local}"
TOKEN="${FANGS_GITHUB_TOKEN:-${GH_TOKEN:-${GITHUB_TOKEN:-}}}"
API="https://api.github.com/repos/${REPO}"

err() { printf '%s\n' "$*" >&2; }

os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
    Linux)  plat="linux" ;;
    Darwin) plat="macos" ;;
    *) err "Unsupported OS: $os (Linux and macOS only)."; exit 1 ;;
esac

case "$arch" in
    x86_64|amd64)  a="x86_64" ;;
    arm64|aarch64) a="arm64" ;;
    *) err "Unsupported architecture: $arch."; exit 1 ;;
esac

# curl against the GitHub API, adding the auth header only when a token is set.
api_get() {
    # usage: api_get <url>   (Accept: github+json)
    if [ -n "$TOKEN" ]; then
        curl -fsSL -H "Accept: application/vnd.github+json" \
                   -H "Authorization: Bearer $TOKEN" "$1"
    else
        curl -fsSL -H "Accept: application/vnd.github+json" "$1"
    fi
}

# Resolve the release JSON (latest unless pinned via FANGS_VERSION).
if [ "${FANGS_VERSION:-latest}" = "latest" ]; then
    rel="$(api_get "${API}/releases/latest" 2>/dev/null || true)"
else
    rel="$(api_get "${API}/releases/tags/${FANGS_VERSION}" 2>/dev/null || true)"
fi

if [ -z "$rel" ]; then
    err "Could not fetch release metadata from ${REPO}."
    if [ -z "$TOKEN" ]; then
        err "If the repository is private, pass a token with 'repo' scope:"
        err "  curl -fsSL .../install.sh | FANGS_GITHUB_TOKEN=ghp_xxx sh"
    fi
    exit 1
fi

tag="$(printf '%s' "$rel" | grep -oE '"tag_name"[ ]*:[ ]*"[^"]+"' | head -1 | sed 's/.*"tag_name"[ ]*:[ ]*"//;s/"$//')"
[ -n "$tag" ] || { err "Could not parse the release tag."; exit 1; }
version_no_v="${tag#v}"

case "$FANGS_INSTALL" in
    auto)
        if [ "$plat" = "macos" ]; then install_mode="app"; else install_mode="cli"; fi
        ;;
    app|cli)
        install_mode="$FANGS_INSTALL"
        ;;
    *)
        err "Unsupported FANGS_INSTALL=$FANGS_INSTALL (expected auto, app, or cli)."
        exit 1
        ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

release_has_asset() {
    # usage: release_has_asset <asset-name>
    printf '%s' "$rel" | tr -d '\n' | tr '{' '\n' \
        | grep "\"name\"[ ]*:[ ]*\"$1\"" >/dev/null 2>&1
}

asset_api_url() {
    # usage: asset_api_url <asset-name>
    printf '%s' "$rel" | tr -d '\n' | tr '{' '\n' \
        | grep "\"name\"[ ]*:[ ]*\"$1\"" \
        | grep -oE '"url"[ ]*:[ ]*"https://api.github.com[^"]*/assets/[0-9]+"' \
        | head -1 | sed 's/.*"url"[ ]*:[ ]*"//;s/"$//'
}

download_asset() {
    # usage: download_asset <asset-name> <output-path>
    asset_name="$1"
    out="$2"

    if [ -n "$TOKEN" ]; then
        # Private assets must be fetched via the asset API URL with an octet-stream
        # Accept header (browser_download_url only works for public repos). Pull the
        # API URL of the asset object whose "name" matches ours.
        asset_url="$(asset_api_url "$asset_name" || true)"
        [ -n "$asset_url" ] || return 1
        curl -fSL -H "Accept: application/octet-stream" \
                  -H "Authorization: Bearer $TOKEN" \
                  "$asset_url" -o "$out"
    else
        url="https://github.com/${REPO}/releases/download/${tag}/${asset_name}"
        curl -fSL "$url" -o "$out"
    fi
}

install_cli() {
    asset="fangs-${plat}-${a}.tar.gz"
    printf 'Installing fangs %s CLI (%s-%s) -> %s\n' "$tag" "$plat" "$a" "$PREFIX"

    dl_ok=0
    download_asset "$asset" "$tmp/$asset" && dl_ok=1 || dl_ok=0

    if [ "$dl_ok" -ne 1 ]; then
        err ""
        err "No prebuilt binary for ${plat}-${a} in release ${tag}."
        err "Build from source instead: https://github.com/${REPO}#build-from-source"
        exit 1
    fi

    tar -C "$tmp" -xzf "$tmp/$asset"
    src="$tmp/fangs-${plat}-${a}"

    mkdir -p "$PREFIX/bin" "$PREFIX/lib"
    cp "$src/bin/fangs" "$PREFIX/bin/fangs"
    chmod +x "$PREFIX/bin/fangs"
    # Bundled libghostty-vt (resolved via the binary's relative RPATH -> ../lib).
    cp -P "$src/lib/"libghostty-vt.* "$PREFIX/lib/" 2>/dev/null || true

    printf 'Installed: %s/bin/fangs\n' "$PREFIX"

    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) printf 'Note: add %s/bin to your PATH:\n  export PATH="%s/bin:$PATH"\n' "$PREFIX" "$PREFIX" ;;
    esac
}

install_app() {
    if [ "$plat" != "macos" ]; then
        err "FANGS_INSTALL=app is only supported on macOS."
        exit 1
    fi

    signed_asset="fangs-${version_no_v}-macos-${a}.zip"
    unsigned_asset="fangs-${version_no_v}-macos-${a}-unsigned.zip"
    asset=""

    if release_has_asset "$signed_asset"; then
        asset="$signed_asset"
    elif release_has_asset "$unsigned_asset"; then
        asset="$unsigned_asset"
    else
        asset="$signed_asset"
    fi

    printf 'Installing Fangs %s app (%s-%s) -> %s/Fangs.app\n' "$tag" "$plat" "$a" "$APP_DIR"

    dl_ok=0
    download_asset "$asset" "$tmp/$asset" && dl_ok=1 || dl_ok=0
    if [ "$dl_ok" -ne 1 ]; then
        err ""
        err "No Fangs.app bundle for ${plat}-${a} in release ${tag}."
        err "Try the CLI install instead:"
        err "  curl -fsSL https://raw.githubusercontent.com/${REPO}/main/install.sh | FANGS_INSTALL=cli sh"
        exit 1
    fi

    mkdir -p "$tmp/app"
    /usr/bin/ditto -x -k "$tmp/$asset" "$tmp/app"
    [ -d "$tmp/app/Fangs.app" ] || { err "Release asset did not contain Fangs.app."; exit 1; }

    mkdir -p "$APP_DIR"
    rm -rf "$APP_DIR/Fangs.app"
    /usr/bin/ditto "$tmp/app/Fangs.app" "$APP_DIR/Fangs.app"

    printf 'Installed: %s/Fangs.app\n' "$APP_DIR"
    printf 'Open it with:\n  open "%s/Fangs.app"\n' "$APP_DIR"

    case "$asset" in
        *-unsigned.zip)
            printf '\n'
            printf 'Note: this is an unsigned tester build. If macOS blocks it, right-click Fangs.app and choose Open once.\n'
            ;;
    esac
}

case "$install_mode" in
    app) install_app ;;
    cli) install_cli ;;
esac
