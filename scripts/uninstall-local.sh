#!/usr/bin/env bash
# Retire ce que scripts/install-local.sh a posé dans le dossier de
# l'utilisateur. Sans droits administrateur.
#
#   scripts/uninstall-local.sh               arrête, désactive et retire
#   scripts/uninstall-local.sh --dry-run     affiche ce qui serait fait, sans rien faire
#   scripts/uninstall-local.sh --no-activate retire les fichiers sans parler à systemd ni à GNOME Shell
#   scripts/uninstall-local.sh --purge       retire aussi les données du moteur
#                                            (~/.config/<préfixe>, ~/.local/share/<préfixe>,
#                                            ~/.local/state/<préfixe> : appareil choisi,
#                                            messages gardés, journal) et la règle
#                                            WirePlumber du son de l'iPhone
#
# Les noms viennent de scripts/noms.sh. Une installation restée sous les noms
# d'avant (service et extension « iphone-link ») est retirée de la même façon ;
# ses données (dossiers « tether ») ne partent qu'avec --purge.
#
# Variables : HOME ; EXTENSION_DIR (source de l'extension, pour lire son uuid ;
# défaut : extension/).
#
# Les réglages système ne sont pas touchés. Pour les annuler, AVANT de
# désinstaller ou après :
#   sudo /usr/local/libexec/<préfixe>/<préfixe>-setup revert
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$REPO/scripts/noms.sh"

EXT_SRC="${EXTENSION_DIR:-$REPO/extension}"
UNIT_DIR="$HOME/.config/systemd/user"
EXT_BASE="$HOME/.local/share/gnome-shell/extensions"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"

dry=0
activate=1
purge=0
for arg in "$@"; do
    case "$arg" in
        --dry-run)     dry=1 ;;
        --no-activate) activate=0 ;;
        --purge)       purge=1 ;;
        -h|--help)     awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "option inconnue : $arg" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
run() {
    if [ "$dry" = 1 ]; then
        say "ferait : $*"
    else
        say "fait : $*"
        "$@"
    fi
}
# remove <chemin> : seulement s'il existe.
remove() {
    if [ -e "$1" ] || [ -L "$1" ]; then
        run rm -rf "$1"
    else
        say "absent : $1"
    fi
}

real_home="$(getent passwd "$(id -u)" | cut -d: -f6 || true)"
if [ "$activate" = 1 ] && [ "$(readlink -f "$HOME")" != "$(readlink -f "${real_home:-/nonexistent}")" ]; then
    say "HOME ($HOME) n'est pas le dossier réel de l'utilisateur : systemd et GNOME Shell ne sont pas sollicités."
    activate=0
fi

# Dossiers de l'extension : celui que désigne extension/metadata.json, celui
# du nom courant et celui de l'ancien nom.
ext_dirs=()
add_ext() {
    local dir="$EXT_BASE/$1" seen
    [ -n "$1" ] && [ -d "$dir" ] || return 0
    for seen in "${ext_dirs[@]}"; do
        [ "$seen" != "$dir" ] || return 0
    done
    ext_dirs+=("$dir")
}
if [ -f "$EXT_SRC/metadata.json" ]; then
    uuid="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["uuid"])' "$EXT_SRC/metadata.json")"
    case "$uuid" in
        ""|*/*|.*) uuid="" ;;
    esac
    add_ext "$uuid"
fi
add_ext "$EXT_UUID"
add_ext "$OLD_EXT_UUID"

say "== Arrêt"
if [ "$activate" = 1 ]; then
    for unit in "$ENGINE_UNIT" "$OLD_ENGINE_UNIT"; do
        if [ -e "$UNIT_DIR/$unit" ]; then
            run systemctl --user disable --now "$unit"
        fi
    done
    for dir in "${ext_dirs[@]}"; do
        if [ "$dry" = 1 ]; then
            say "ferait : gnome-extensions disable $(basename "$dir")"
        else
            gnome-extensions disable "$(basename "$dir")" 2>/dev/null || true
            say "fait : gnome-extensions disable $(basename "$dir")"
        fi
    done
else
    say "ignoré (--no-activate ou HOME factice)"
fi

say "== Fichiers"
for unit in "$ENGINE_UNIT" "$OLD_ENGINE_UNIT"; do
    remove "$UNIT_DIR/graphical-session.target.wants/$unit"
    remove "$UNIT_DIR/$unit"
done
remove "$UNIT_DIR/$OBEX_DROPIN_DIR/$OBEX_DROPIN"
# Le dossier du complément n'est retiré que s'il ne contient rien d'autre.
if [ -d "$UNIT_DIR/$OBEX_DROPIN_DIR" ] && [ -z "$(ls -A "$UNIT_DIR/$OBEX_DROPIN_DIR")" ]; then
    run rmdir "$UNIT_DIR/$OBEX_DROPIN_DIR"
fi
remove "$HOME/.local/lib/$NAME_PREFIX"
remove "$HOME/.local/lib/$OLD_INSTALL_NAME"
for dir in "${ext_dirs[@]}"; do
    remove "$dir"
done
if [ "$activate" = 1 ]; then
    run systemctl --user daemon-reload
fi

say "== Données du moteur"
data=("$CONFIG_HOME/$NAME_PREFIX" "$DATA_HOME/$NAME_PREFIX" "$STATE_HOME/$NAME_PREFIX"
      "$CONFIG_HOME/wireplumber/wireplumber.conf.d/$AUDIO_RULE"
      "$CONFIG_HOME/$OLD_DATA_NAME" "$DATA_HOME/$OLD_DATA_NAME" "$STATE_HOME/$OLD_DATA_NAME"
      "$CONFIG_HOME/wireplumber/wireplumber.conf.d/$OLD_AUDIO_RULE")
if [ "$purge" = 1 ]; then
    for path in "${data[@]}"; do
        remove "$path"
    done
    say "La clé du trousseau (« $NAME_PREFIX message store key ») n'est pas retirée : elle ne sert plus à rien une fois les données parties."
else
    say "gardées (appareil choisi, messages, journal). --purge pour les retirer :"
    for path in "${data[@]}"; do
        if [ -e "$path" ]; then say "  $path"; fi
    done
fi

say
if [ -x "$SYSTEM_HELPER" ]; then
    say "Les réglages Bluetooth du système sont toujours en place. Pour les annuler :"
    say "  sudo $SYSTEM_HELPER revert"
elif [ -x "/usr/local/libexec/$OLD_INSTALL_NAME/$OLD_INSTALL_NAME-setup" ]; then
    say "Des réglages Bluetooth posés sous l'ancien nom sont toujours en place. Pour les annuler :"
    say "  sudo /usr/local/libexec/$OLD_INSTALL_NAME/$OLD_INSTALL_NAME-setup revert"
else
    say "Aucun réglage système n'est installé ($SYSTEM_HELPER absent)."
fi
