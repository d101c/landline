#!/usr/bin/env bash
# Vérifie l'archive portable du moteur dans des conteneurs propres, chacun avec
# les seules bibliothèques d'un bureau GNOME (GLib, libsecret, libnotify,
# systemd, D-Bus) : aucune bibliothèque manquante (ldd), « landlinectl --help »
# répond, et landlined démarre (isolé : HOME et XDG_RUNTIME_DIR jetables, sans
# bus système ni BlueZ ; il doit tenir quelques secondes et écrire sa version
# dans son journal).
#
#   scripts/check-release.sh dist/landline-engine-0.2.0-x86_64.tar.zst [image…]
#
# Images par défaut : ubuntu:24.04 ubuntu:26.04 debian:13 fedora:latest
# archlinux:latest. Variables : DOCKER (défaut : docker).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$REPO/scripts/noms.sh"

[ $# -ge 1 ] || { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}" >&2; exit 2; }
archive="$(readlink -f "$1")"; shift
[ -f "$archive" ] || { echo "erreur : $archive introuvable" >&2; exit 1; }
images=("$@")
[ ${#images[@]} -gt 0 ] || images=(ubuntu:24.04 ubuntu:26.04 debian:13 fedora:latest archlinux:latest)
DOCKER="${DOCKER:-docker}"

# Paquets « bureau GNOME » par famille : la bibliothèque seule, sans outils.
read -r -d '' inside <<'EOF' || true
set -eu
. /etc/os-release
case "$ID" in
    ubuntu|debian)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        glib=libglib2.0-0t64; apt-cache show "$glib" >/dev/null 2>&1 || glib=libglib2.0-0
        apt-get install -y -qq --no-install-recommends "$glib" libsecret-1-0 libnotify4 libsystemd0 libdbus-1-3 zstd >/dev/null ;;
    fedora)
        dnf install -y -q glib2 libsecret libnotify systemd-libs dbus-libs zstd >/dev/null 2>&1 ;;
    arch)
        pacman -Syu --noconfirm --needed -q glib2 libsecret libnotify systemd-libs dbus zstd >/dev/null 2>&1 ;;
    *) echo "distribution inconnue : $ID" >&2; exit 1 ;;
esac
printf '%s %s, glibc %s\n' "$ID" "${VERSION_ID:-rolling}" "$(ldd --version | head -n 1 | grep -o '[0-9.]*$')"
mkdir -p /opt/t && cd /opt/t && zstd -dc /archive.tar.zst | tar -x
eng="$(echo /opt/t/*/engine)"
for f in "$eng/${PREFIX}d" "$eng/${PREFIX}ctl" "$eng/$PREFIX-le-params"; do
    if ldd "$f" | grep -q 'not found'; then
        echo "ÉCHEC ldd ${f##*/} :"; ldd "$f" | grep 'not found'; exit 1
    fi
    bundled="$(ldd "$f" | awk -v d="$eng/lib/" 'index($3, d) == 1 { print $1 }' | tr '\n' ' ')"
    echo "ldd ${f##*/} : rien ne manque${bundled:+ ; pris dans lib/ : $bundled}"
done
"$eng/${PREFIX}ctl" --help >/tmp/help.txt 2>&1 || { echo "ÉCHEC ${PREFIX}ctl --help"; cat /tmp/help.txt; exit 1; }
echo "${PREFIX}ctl --help : $(wc -l < /tmp/help.txt) lignes ; --version : $("$eng/${PREFIX}ctl" --version 2>&1 | head -n 1)"
export HOME=/tmp/h XDG_RUNTIME_DIR=/tmp/h/run LANDLINE_LOCALEDIR="$eng/locale"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
"$eng/${PREFIX}d" </dev/null >/tmp/h/out 2>&1 & pid=$!
sleep 4
if kill -0 "$pid" 2>/dev/null; then
    echo "${PREFIX}d : en marche après 4 s"; kill "$pid"; wait "$pid" 2>/dev/null || true
else
    code=0; wait "$pid" || code=$?; echo "${PREFIX}d : arrêté (code $code)"
fi
grep -m1 'version' "$HOME/.local/state/$PREFIX/${PREFIX}d.log" 2>/dev/null || { echo "journal absent ; sortie :"; cat /tmp/h/out; }
EOF

status=0
for image in "${images[@]}"; do
    echo "== $image"
    # seccomp levé : un Docker plus ancien que l'image refuse ses appels
    # système récents (apt-get, dnf échouent) ; voir build-release.sh.
    if ! "$DOCKER" run --rm --security-opt seccomp=unconfined -v "$archive:/archive.tar.zst:ro" -e PREFIX="$NAME_PREFIX" \
            "$image" bash -c "$inside"; then
        echo "ÉCHEC sur $image"
        status=1
    fi
done
exit $status
