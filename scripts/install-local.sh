#!/usr/bin/env bash
# Installe le moteur, son unité systemd utilisateur et l'extension GNOME dans
# le dossier de l'utilisateur, sans droits administrateur.
#
#   scripts/install-local.sh               installe et active
#   scripts/install-local.sh --dry-run     affiche ce qui serait fait, sans rien faire
#   scripts/install-local.sh --no-activate copie les fichiers sans rien démarrer ni activer
#
# Les noms (dossiers, service, binaires) viennent de scripts/noms.sh ; ils sont
# écrits ci-dessous avec <préfixe> et <uuid>.
#
# Ce qui est posé :
#   ~/.local/lib/<préfixe>/engine/          le démon et l'outil en ligne de commande
#   ~/.local/lib/<préfixe>/engine/lib/      bibliothèques hors système (voir plus bas)
#   ~/.local/lib/<préfixe>/engine/locale/   traductions du moteur
#   ~/.local/lib/<préfixe>/engine/LICENSE   licence MIT du moteur (Tether, Zack Bartel)
#   ~/.local/lib/<préfixe>/<préfixe>-setup  assistant des réglages système
#       (hors du dossier de l'extension : l'extension ne le lance jamais avec
#       des droits ; elle n'y lit que « status », et montre la commande
#       « sudo … apply » à lancer soi-même tant que la copie de root manque)
#   ~/.config/systemd/user/<préfixe>-engine.service
#   ~/.local/share/gnome-shell/extensions/<uuid>/   copie de extension/
#
# Migration. Une installation sous les noms d'avant (extension et service
# « iphone-link », données du moteur dans des dossiers « tether ») est reprise
# sans perdre l'appairage :
#   - l'ancien service est arrêté, désactivé et retiré ;
#   - l'ancienne extension est désactivée et retirée, ses réglages recopiés ;
#   - ~/.config/tether, ~/.local/share/tether et ~/.local/state/tether sont
#     déplacés vers les nouveaux dossiers (jamais par-dessus un dossier existant) ;
#   - la règle WirePlumber du son de l'iPhone est renommée si elle existe ;
#   - ~/.local/lib/iphone-link est retiré.
# La clé du trousseau qui chiffre les messages gardés n'est pas déplacée ici :
# le moteur la reprend lui-même à son premier démarrage sous le nouveau nom
# (l'ancienne entrée reste en place). L'appairage lui-même vit dans BlueZ et ne
# dépend d'aucun de ces noms. Les réglages système posés sous l'ancien nom
# sont repris par « <préfixe>-setup apply », pas ici.
#
# Bibliothèques : le moteur est compilé par scripts/build-engine.sh avec un
# RUNPATH vers le sysroot du banc d'essai. Chaque binaire est passé à ldd ;
# toute bibliothèque trouvée hors des dossiers du système est copiée dans
# engine/lib/, pour que le moteur ne dépende plus du dossier du banc.
#
# Variables :
#   BUILD_DIR      binaires du moteur (défaut : engine/build/local)
#   EXTENSION_DIR  source de l'extension (défaut : extension/)
#   HOME           dossier de l'utilisateur ; s'il n'est pas le vrai (essais),
#                  rien n'est démarré, arrêté ni activé
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$REPO/scripts/noms.sh"

BUILD_DIR="${BUILD_DIR:-$REPO/engine/build/local}"
INSTALL_DIR="$HOME/.local/lib/$NAME_PREFIX"
ENGINE_DIR="$INSTALL_DIR/engine"
UNIT_SRC="$REPO/system/$ENGINE_UNIT"
UNIT_DIR="$HOME/.config/systemd/user"
SETUP_SRC="$REPO/system/$SETUP_NAME"
EXT_SRC="${EXTENSION_DIR:-$REPO/extension}"
EXT_BASE="$HOME/.local/share/gnome-shell/extensions"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
BINARIES=("$ENGINE_DAEMON" "$ENGINE_CLI")

dry=0
activate=1
for arg in "$@"; do
    case "$arg" in
        --dry-run)     dry=1 ;;
        --no-activate) activate=0 ;;
        -h|--help)     awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "option inconnue : $arg" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'erreur : %s\n' "$*" >&2; exit 1; }
run() {
    if [ "$dry" = 1 ]; then
        say "ferait : $*"
    else
        say "fait : $*"
        "$@"
    fi
}
# best_effort <commande…> : comme run, mais un échec n'arrête pas le script.
best_effort() {
    if [ "$dry" = 1 ]; then
        say "ferait : $*"
    elif "$@" 2>/dev/null; then
        say "fait : $*"
    else
        say "sans effet : $*"
    fi
}

# Vrai dossier de l'utilisateur ? Sinon (HOME factice), on ne touche ni à
# systemd ni à GNOME Shell, qui eux sont bien réels.
real_home="$(getent passwd "$(id -u)" | cut -d: -f6 || true)"
is_real_home=1
if [ "$(readlink -f "$HOME")" != "$(readlink -f "${real_home:-/nonexistent}")" ]; then
    is_real_home=0
    if [ "$activate" = 1 ]; then
        say "HOME ($HOME) n'est pas le dossier réel de l'utilisateur : rien ne sera démarré ni activé."
        activate=0
    fi
fi

[ -x "$BUILD_DIR/$ENGINE_DAEMON" ] || die "moteur introuvable dans $BUILD_DIR : lancer d'abord scripts/build-engine.sh"
[ -f "$UNIT_SRC" ] || die "$UNIT_SRC introuvable"

# --- migration d'une installation sous les anciens noms -----------------------

# move_dir <ancien> <nouveau> : déplace un dossier de données, jamais
# par-dessus un dossier qui existe déjà.
move_dir() {
    local old="$1" new="$2"
    [ -d "$old" ] || return 0
    if [ -e "$new" ]; then
        say "attention : $new existe déjà ; $old est laissé en place (à fusionner ou retirer à la main)"
        return 0
    fi
    run mkdir -p "$(dirname "$new")"
    run mv "$old" "$new"
}

migrate() {
    local old_unit="$UNIT_DIR/$OLD_ENGINE_UNIT" old_ext="$EXT_BASE/$OLD_EXT_UUID"
    local old_install="$HOME/.local/lib/$OLD_INSTALL_NAME"
    local old_rule="$CONFIG_HOME/wireplumber/wireplumber.conf.d/$OLD_AUDIO_RULE"
    local found=0 path
    for path in "$old_unit" "$old_ext" "$old_install" "$old_rule" \
                "$CONFIG_HOME/$OLD_DATA_NAME" "$DATA_HOME/$OLD_DATA_NAME" "$STATE_HOME/$OLD_DATA_NAME"; do
        if [ -e "$path" ] || [ -L "$path" ]; then found=1; fi
    done
    if [ "$found" = 0 ]; then
        say "aucune installation sous les anciens noms"
        return 0
    fi

    # L'ancien moteur doit être arrêté avant de déplacer ses dossiers.
    if [ "$activate" = 1 ]; then
        if [ -e "$old_unit" ]; then
            best_effort systemctl --user disable --now "$OLD_ENGINE_UNIT"
        fi
        # Un ancien démon lancé à la main, hors du service, tiendrait encore l'iPhone.
        if pgrep -u "$(id -u)" -x "$OLD_ENGINE_DAEMON" >/dev/null 2>&1 && [ "$dry" = 0 ]; then
            best_effort pkill -u "$(id -u)" -x "$OLD_ENGINE_DAEMON"
        fi
    elif [ "$is_real_home" = 1 ] && [ "$dry" = 0 ] && pgrep -u "$(id -u)" -x "$OLD_ENGINE_DAEMON" >/dev/null 2>&1; then
        die "l'ancien moteur ($OLD_ENGINE_DAEMON) tourne encore : l'arrêter d'abord (systemctl --user stop $OLD_ENGINE_UNIT), ou relancer sans --no-activate"
    fi
    for path in "$UNIT_DIR/graphical-session.target.wants/$OLD_ENGINE_UNIT" "$old_unit"; do
        if [ -e "$path" ] || [ -L "$path" ]; then run rm -f "$path"; fi
    done

    if [ -d "$old_ext" ]; then
        if [ "$activate" = 1 ]; then
            best_effort gnome-extensions disable "$OLD_EXT_UUID"
            if command -v dconf >/dev/null 2>&1; then
                # Les réglages (réponses toutes faites, silence…) suivent.
                if [ "$dry" = 1 ]; then
                    say "ferait : dconf dump $OLD_EXT_SCHEMA_PATH | dconf load $EXT_SCHEMA_PATH ; dconf reset -f $OLD_EXT_SCHEMA_PATH"
                elif dconf dump "$OLD_EXT_SCHEMA_PATH" | dconf load "$EXT_SCHEMA_PATH" 2>/dev/null; then
                    dconf reset -f "$OLD_EXT_SCHEMA_PATH" 2>/dev/null || true
                    say "fait : réglages de l'extension recopiés de $OLD_EXT_SCHEMA_PATH vers $EXT_SCHEMA_PATH"
                else
                    say "sans effet : copie des réglages de l'extension (ils repartent des valeurs par défaut)"
                fi
            fi
        fi
        run rm -rf "$old_ext"
    fi

    move_dir "$CONFIG_HOME/$OLD_DATA_NAME" "$CONFIG_HOME/$NAME_PREFIX"
    move_dir "$DATA_HOME/$OLD_DATA_NAME" "$DATA_HOME/$NAME_PREFIX"
    move_dir "$STATE_HOME/$OLD_DATA_NAME" "$STATE_HOME/$NAME_PREFIX"
    # Le journal garde son contenu sous le nouveau nom. À blanc, il est
    # encore dans l'ancien dossier.
    local log_dir="$STATE_HOME/$NAME_PREFIX"
    if [ "$dry" = 1 ] && [ ! -e "$log_dir" ]; then log_dir="$STATE_HOME/$OLD_DATA_NAME"; fi
    if [ -f "$log_dir/$OLD_ENGINE_LOG" ] && [ ! -e "$STATE_HOME/$NAME_PREFIX/$ENGINE_LOG" ]; then
        run mv "$STATE_HOME/$NAME_PREFIX/$OLD_ENGINE_LOG" "$STATE_HOME/$NAME_PREFIX/$ENGINE_LOG"
    fi
    if [ -f "$old_rule" ]; then
        run mv "$old_rule" "$(dirname "$old_rule")/$AUDIO_RULE"
    fi

    if [ -e "$old_install" ]; then run rm -rf "$old_install"; fi

    say "clé du trousseau : reprise par le moteur à son premier démarrage (l'ancienne entrée reste en place)"
    if [ -e "/usr/local/libexec/$OLD_INSTALL_NAME" ]; then
        say "réglages système posés sous l'ancien nom : ils restent valables ; « sudo $INSTALL_DIR/$SETUP_NAME apply » les reprend sous le nouveau nom"
    fi
}

say "== Migration"
migrate

# --- moteur -----------------------------------------------------------------

is_system_path() {
    case "$1" in
        /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) return 0 ;;
        *) return 1 ;;
    esac
}

say "== Moteur : bibliothèques liées (ldd)"
present=()
declare -A bundle=()      # nom de bibliothèque -> chemin d'origine
for name in "${BINARIES[@]}"; do
    bin="$BUILD_DIR/$name"
    [ -x "$bin" ] || die "$bin manquant"
    present+=("$name")
    runpath="$(readelf -d "$bin" 2>/dev/null | sed -n 's/.*R\(UN\)\{0,1\}PATH.*\[\(.*\)\]/\2/p' | head -n 1 || true)"
    outside=()
    while read -r lib arrow path _; do
        [ "$arrow" = "=>" ] || continue
        if [ "$path" = "not" ]; then
            die "$name : bibliothèque $lib introuvable sur ce système"
        fi
        if ! is_system_path "$path"; then
            outside+=("$lib")
            bundle[$lib]="$path"
        fi
    done < <(ldd "$bin")
    if [ "${#outside[@]}" -eq 0 ]; then
        say "$name : bibliothèques du système seulement (RUNPATH de compilation : ${runpath:-aucun}, sans effet)"
    else
        for lib in "${outside[@]}"; do
            say "$name : dépend de $lib, pris hors du système dans ${bundle[$lib]}"
        done
    fi
done

say "== Moteur : copie vers $ENGINE_DIR"
# Rien d'une installation précédente ne doit rester (binaires renommés ou retirés).
if [ -e "$ENGINE_DIR" ]; then run rm -rf "$ENGINE_DIR"; fi
run mkdir -p "$ENGINE_DIR/lib"
for name in "${present[@]}"; do
    run install -m 0755 "$BUILD_DIR/$name" "$ENGINE_DIR/$name"
done
for lib in "${!bundle[@]}"; do
    # -L : copie le fichier visé par le lien, sous le nom que l'éditeur de liens cherche.
    run cp -L "${bundle[$lib]}" "$ENGINE_DIR/lib/$lib"
done
if [ -d "$BUILD_DIR/locale" ]; then
    run cp -r "$BUILD_DIR/locale" "$ENGINE_DIR/locale"
fi
# La licence MIT du moteur suit ses binaires.
run install -m 0644 "$REPO/engine/LICENSE" "$ENGINE_DIR/LICENSE"

# Le RUNPATH de compilation reste inscrit dans les binaires. patchelf, s'il
# existe, le remplace ; sinon LD_LIBRARY_PATH (posé par l'unité) passe avant lui.
if command -v patchelf >/dev/null 2>&1; then
    for name in "${present[@]}"; do
        # shellcheck disable=SC2016  # $ORIGIN est lu par l'éditeur de liens, pas par le shell
        run patchelf --set-rpath '$ORIGIN/lib' "$ENGINE_DIR/$name"
    done
else
    say "patchelf absent : le RUNPATH de compilation reste dans les binaires ; l'unité pose LD_LIBRARY_PATH=$ENGINE_DIR/lib, qui passe avant lui."
fi

if [ "$dry" = 0 ]; then
    say "== Moteur : vérification des copies"
    for name in "${present[@]}"; do
        while read -r lib arrow path _; do
            [ "$arrow" = "=>" ] || continue
            [ "$path" != "not" ] || die "$name installé : $lib introuvable"
            case "$path" in
                "$ENGINE_DIR"/lib/*) ;;
                *) is_system_path "$path" || die "$name installé dépend encore de $path" ;;
            esac
        done < <(LD_LIBRARY_PATH="$ENGINE_DIR/lib" ldd "$ENGINE_DIR/$name")
        say "$name : ne dépend que du système et de $ENGINE_DIR/lib"
    done
fi

say "== Assistant des réglages système"
run install -m 0755 "$SETUP_SRC" "$INSTALL_DIR/$SETUP_NAME"

# --- unité utilisateur ------------------------------------------------------

say "== Unité utilisateur"
run mkdir -p "$UNIT_DIR"
run install -m 0644 "$UNIT_SRC" "$UNIT_DIR/$ENGINE_UNIT"

# --- extension --------------------------------------------------------------

say "== Extension GNOME"
uuid=""
if [ -f "$EXT_SRC/metadata.json" ]; then
    uuid="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["uuid"])' "$EXT_SRC/metadata.json")"
    case "$uuid" in
        ""|*/*|.*) die "uuid invalide dans $EXT_SRC/metadata.json : « $uuid »" ;;
    esac
    ext_dir="$EXT_BASE/$uuid"
    run mkdir -p "$EXT_BASE"
    run rm -rf "$ext_dir"
    run cp -r "$EXT_SRC" "$ext_dir"
    if [ -d "$EXT_SRC/schemas" ]; then
        if command -v glib-compile-schemas >/dev/null 2>&1; then
            run glib-compile-schemas "$ext_dir/schemas"
        else
            say "glib-compile-schemas absent : schémas non compilés (GNOME Shell 44+ le fait lui-même)"
        fi
    fi
else
    say "$EXT_SRC absent : étape ignorée"
fi

# --- activation -------------------------------------------------------------

say "== Activation"
if [ "$activate" = 0 ]; then
    say "ignorée. À faire dans la vraie session :"
    say "  systemctl --user daemon-reload"
    say "  systemctl --user enable --now $ENGINE_UNIT"
    [ -z "$uuid" ] || say "  gnome-extensions enable $uuid"
else
    run systemctl --user daemon-reload
    run systemctl --user enable --now "$ENGINE_UNIT"
    if [ -n "$uuid" ]; then
        if [ "$dry" = 1 ]; then
            say "ferait : gnome-extensions enable $uuid"
        elif gnome-extensions enable "$uuid" 2>/dev/null; then
            say "fait : gnome-extensions enable $uuid"
        else
            say "GNOME Shell ne connaît pas encore l'extension (normal sous Wayland juste après la copie) :"
            say "fermer puis rouvrir la session, puis : gnome-extensions enable $uuid"
        fi
    fi
fi

say
say "Réglages Bluetooth du système : $INSTALL_DIR/$SETUP_NAME status (voir docs/systeme.md)."
