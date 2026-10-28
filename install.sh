#!/usr/bin/env bash
# install.sh — build and install khat for the current user (no sudo required).
#
# Two ways to use it:
#
#   from a checkout or extracted source tree:
#       ./install.sh
#
#   without the repository, straight from the network:
#       curl -fsSL https://github.com/kodejeet/khat/raw/main/install.sh | bash
#   (the script downloads the source tarball itself, builds, and installs)
#
# Options:
#   --prefix=DIR    install prefix, default: $HOME/.local
#   --no-systemd    install but do not enable the systemd user unit
#
# What gets installed (per user):
#   $PREFIX/bin/khat
#   $PREFIX/share/applications/khat.desktop
#   $PREFIX/share/systemd/user/khat.service
#   $PREFIX/share/icons/hicolor/<size>x<size>/apps/khat.png

set -euo pipefail

REPO_URL="https://github.com/kodejeet/khat"
PREFIX="$HOME/.local"
ENABLE_SYSTEMD=1

usage() {
    cat <<USAGE
Usage: install.sh [--prefix=DIR] [--no-systemd]

Builds khat from the current source checkout (or downloads it first when
run through curl) and installs it for the current user.

  --prefix=DIR    install prefix, default: $HOME/.local
  --no-systemd    install but do not enable the systemd user unit
USAGE
}

for arg in "$@"; do
    case "$arg" in
        --prefix=*) PREFIX="${arg#--prefix=}" ;;
        --no-systemd) ENABLE_SYSTEMD=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install.sh: unknown option '$arg' (see --help)"; exit 2 ;;
    esac
done
set --    # clear the argument list; every option was consumed above
PREFIX="$(readlink -m "$PREFIX")"

log() { printf '\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33mwarning: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Get the source: use the checkout the script lives in, or download it.
# ---------------------------------------------------------------------------
SOURCE_DIR=""
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
    candidate="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if grep -q "project(khat" "$candidate/CMakeLists.txt" 2>/dev/null; then
        SOURCE_DIR="$candidate"
        log "Using source at $SOURCE_DIR"
    fi
fi

if [ -z "$SOURCE_DIR" ]; then
    TMPDIR_INSTALL="$(mktemp -d "${TMPDIR:-/tmp/opencode}/khat-install.XXXXXX")"
    trap 'rm -rf "$TMPDIR_INSTALL"' EXIT
    log "Downloading source from $REPO_URL"
    mkdir -p "$TMPDIR_INSTALL/src"
    curl -fsSL "$REPO_URL/archive/refs/heads/main.tar.gz" \
        | tar -xz -C "$TMPDIR_INSTALL/src" --strip-components=1
    SOURCE_DIR="$TMPDIR_INSTALL/src"
    log "Source extracted to $SOURCE_DIR"
fi

# ---------------------------------------------------------------------------
# 2. Check the build prerequisites.
# ---------------------------------------------------------------------------
for tool in cmake curl tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' is required but not installed." >&2
        exit 1
    fi
done
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    echo "error: a C++20 compiler is required (g++ or clang++)." >&2
    exit 1
fi

JOBS="$(nproc 2>/dev/null || echo 2)"

# ---------------------------------------------------------------------------
# 3. Configure, build, install. A missing Qt 6 turns into a package hint.
# ---------------------------------------------------------------------------
BUILD_DIR="$SOURCE_DIR/build-release"
log "Configuring (prefix: $PREFIX, jobs: $JOBS)"
if ! cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBUILD_TESTING=OFF; then
    warn "CMake configuration failed. On Ubuntu this usually means the toolchain or Qt 6 is missing:"
    warn "    sudo apt install build-essential cmake qt6-base-dev"
    warn "(Qt 6.5 or newer is required; older LTS releases ship Qt 6.4, see docs/installation.md)"
    exit 1
fi

log "Building khat"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

log "Installing to $PREFIX"
cmake --install "$BUILD_DIR"

# ---------------------------------------------------------------------------
# 4. Refresh desktop integration (best effort; optional tools may be absent).
# ---------------------------------------------------------------------------
if [ -x "$(command -v update-desktop-database || true)" ] \
    && [ -d "$PREFIX/share/applications" ]; then
    update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi
if [ -x "$(command -v gtk-update-icon-cache || true)" ] \
    && [ -d "$PREFIX/share/icons/hicolor" ]; then
    gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor" 2>/dev/null || true
fi
# Index the installed manual page so `whatis khat` / `man khat` work.
# Run as the user; man-db (>= 2.9) adds ~/.local/share/man to the user
# manpath automatically once the directory exists.
if [ -d "$PREFIX/share/man/man1" ] && command -v mandb >/dev/null 2>&1; then
    mandb -q 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 5. Enable the systemd user service so khat starts at login.
# ---------------------------------------------------------------------------
if [ "$ENABLE_SYSTEMD" = 1 ] && command -v systemctl >/dev/null 2>&1; then
    if [ -f "$PREFIX/share/systemd/user/khat.service" ] \
        && systemctl --user daemon-reload >/dev/null 2>&1 \
        && systemctl --user enable --now khat.service >/dev/null 2>&1; then
        log "Enabled and started the khat systemd user service"
        # A previous installation may already be running with the old unit.
        systemctl --user try-restart khat.service >/dev/null 2>&1 || true
    else
        warn "Systemd user service could not be enabled (headless session?)."
        warn "Start khat manually instead:   $PREFIX/bin/khat --background"
        warn "Open the settings window with:  $PREFIX/bin/khat --settings"
    fi
fi

# ---------------------------------------------------------------------------
# 6. Next steps.
# ---------------------------------------------------------------------------
echo
log "khat is installed."
cat <<NEXT

Next steps (details: docs/installation.md and docs/google-oauth-setup.md):

  1. Create a Google Cloud project, enable the Gmail API, create an OAuth
     "Desktop app" client, and download its JSON to:

         ~/.config/khat/oauth-client.json

     Console: https://console.cloud.google.com/apis/credentials

  2. Authorize your account:

         $PREFIX/bin/khat --settings     (Settings -> Accounts -> Add Account...)

  The settings window can be closed afterwards; polling keeps running in
  the background service.
NEXT
