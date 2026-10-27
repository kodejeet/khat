#!/usr/bin/env bash
# uninstall.sh — remove the per-user khat installation.
#
# Usage:
#   ./uninstall.sh [--prefix=$HOME/.local] [--purge]
#
#   --purge   additionally delete configuration and synchronization state
#             (~/.config/khat, ~/.local/share/khat). Supplying OAuth tokens in
#             the GNOME keyring stays unaffected; revoke access yourself at
#             https://myaccount.google.com/permissions.
#
# Without --purge your account data (OAuth client file, state database) is
# kept, so reinstalling keeps working accounts.

set -euo pipefail

PREFIX="$HOME/.local"
PURGE=0

for arg in "$@"; do
    case "$arg" in
        --prefix=*) PREFIX="${arg#--prefix=}" ;;
        --purge) PURGE=1 ;;
        -h|--help)
            echo "Usage: uninstall.sh [--prefix=DIR] [--purge]"
            exit 0 ;;
        *) echo "uninstall.sh: unknown option '$arg'"; exit 2 ;;
    esac
done
PREFIX="$(readlink -m "$PREFIX")"

log() { printf '\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33mwarning: %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
# 1. Stop and disable the application.
# ---------------------------------------------------------------------------
if command -v systemctl >/dev/null 2>&1; then
    if systemctl --user disable --now khat.service >/dev/null 2>&1; then
        log "Disabled and stopped the khat systemd user service"
    fi
fi
# Best effort: also terminate a manually started instance.
if pgrep -x khat >/dev/null 2>&1; then
    pkill -TERM -x khat >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5; do
        pgrep -x khat >/dev/null 2>&1 || break
        sleep 1
    done
    pgrep -x khat >/dev/null 2>&1 \
        && warn "khat did not exit within 5 seconds; remove any runtime files under \$XDG_RUNTIME_DIR/khat/ manually."
fi

# ---------------------------------------------------------------------------
# 2. Remove the installed files.
# ---------------------------------------------------------------------------
SIZES="16 22 24 32 48 64 128 256 512"
log "Removing files under $PREFIX"
rm -fv "$PREFIX/bin/khat"
rm -fv "$PREFIX/share/applications/khat.desktop"
rm -fv "$PREFIX/share/systemd/user/khat.service"
for size in $SIZES; do
    rm -fv "$PREFIX/share/icons/hicolor/${size}x${size}/apps/khat.png"
done

if [ -x "$(command -v update-desktop-database || true)" ] \
    && [ -d "$PREFIX/share/applications" ]; then
    update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi
if [ -x "$(command -v gtk-update-icon-cache || true)" ] \
    && [ -d "$PREFIX/share/icons/hicolor" ]; then
    gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor" 2>/dev/null || true
fi

systemctl --user daemon-reload >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# 3. User data handling.
# ---------------------------------------------------------------------------
if [ "$PURGE" = 1 ]; then
    log "Purging configuration and state"
    rm -rf "$HOME/.config/khat" "$HOME/.local/share/khat"
    echo
    log "khat removed completely."
    cat <<PURGE

Also revoke the Google grant so the stored refresh token becomes useless:
    https://myaccount.google.com/permissions
(OAuth tokens live in the GNOME keyring, not in these directories.)
PURGE
else
    echo
    log "khat removed."
    cat <<KEPT

User data was kept (configuration: \$HOME/.config/khat, state:
\$HOME/.local/share/khat). Reinstalling restores your accounts as they were.
To erase everything:  ./uninstall.sh --purge
KEPT
fi
