#!/usr/bin/env bash
# Captures de Réglages › Engine dans chacun de ses états, sans rien installer :
#   tools/shell-test/engine-page.sh            captures dans docs/captures
#   SHOTS=/tmp/x tools/shell-test/engine-page.sh
#
# La fenêtre s'ouvre dans un mutter sans écran (--headless), sur un bus
# D-Bus privé ; HOME et les dossiers XDG sont temporaires ; systemd et le
# moteur sont des doublures (tools/shell-test/engine-page.js). Rien n'est
# téléchargé ni installé.
set -u
S="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$S/../.." && pwd)"
SHOTS="${SHOTS:-$REPO/docs/captures}"
mkdir -p "$SHOTS"
T=$(mktemp -d /tmp/ll-page.XXXX)
mkdir -p "$T/home" "$T/run"; chmod 700 "$T/run"
if [ "${1:-}" != --inner ]; then
    env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
        HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_CACHE_HOME="$T/home/.cache" \
        XDG_DATA_HOME="$T/home/.local/share" XDG_RUNTIME_DIR="$T/run" GSETTINGS_BACKEND=memory \
        LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 LANGUAGE= SHOTS="$SHOTS" \
        dbus-run-session -- "$0" --inner 2> "$T/session.log"
    status=$?
    [ "$status" = 0 ] || tail -20 "$T/session.log" | grep -v dbus-daemon
    rm -rf "$T"
    ls "$SHOTS"/engine-*.png 2>/dev/null
    exit $status
fi
mutter --headless --wayland --no-x11 --virtual-monitor 900x1000 --wayland-display=ll-page >/dev/null 2>&1 &
M=$!
for _ in $(seq 50); do [ -e "$XDG_RUNTIME_DIR/ll-page" ] && break; sleep 0.1; done
WAYLAND_DISPLAY=ll-page GDK_BACKEND=wayland gjs -m "$S/engine-page.js" "$SHOTS"
status=$?
kill "$M" 2>/dev/null
exit $status
