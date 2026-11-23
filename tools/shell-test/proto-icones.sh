#!/usr/bin/env bash
# Icônes des applis de l'iPhone (#48) : un démarrage du Shell isolé contre le
# faux moteur, une capture par endroit où une appli est montrée. L'App Store
# est doublé par le pilote : ses icônes viennent de $FAUX_APPSTORE (des PNG
# téléchargés hors du dépôt, jamais commités).
#
#   FAUX_APPSTORE=/chemin/icones SHOTS=/chemin/captures \
#     INNER=tools/shell-test/proto-icones.sh tools/shell-test/run.sh
#   (SCHEME=prefer-light pour le style clair)
set -u
S="$(cd "$(dirname "$0")" && pwd)"; OUT="$PILOTE_DIR"; RT="$XDG_RUNTIME_DIR"
UUID="landline@d101c.github.io"
SOCK="$FAUX_SOCKET"
failed=0
P="${PREFIXE:-i}"

pilote() {
    echo "$*" > "$OUT/cmd"
    for _ in $(seq 100); do [ -e "$OUT/cmd" ] || return 0; sleep 0.05; done
    echo "le pilote n'a pas pris : $*" >&2; failed=1
}
field() {
    pilote state "$1"
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(json.dumps($2, ensure_ascii=False))" "$OUT/$1.json" 2>&1 | tail -1
}
expect() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1 (obtenu $2, attendu $3)"; failed=1; fi; }
shot() {
    pilote shot "$1"
    for _ in $(seq 60); do tail -c 12 "$OUT/$1.png" 2>/dev/null | grep -q IEND && break; sleep 0.05; done
    cp "$OUT/$1.png" "$SHOTS/$P-$1.png"
}
moteur() { echo "$*" >&3; }
cleanup() { kill "${SH:-}" "${MOTEUR:-}" 2>/dev/null; }
trap cleanup EXIT

echo '{"experimental": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
rm -f "$RT/scenario"; mkfifo "$RT/scenario"
python3 "$REPO/tools/faux-moteur.py" --socket "$SOCK" --etat relie < "$RT/scenario" >> "$OUT/moteur.log" 2>&1 &
MOTEUR=$!
exec 3> "$RT/scenario"
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.desktop.interface color-scheme "$SCHEME"
gsettings set org.gnome.desktop.interface enable-animations true
gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x1024 \
    --wayland-display=wayland-ipl > "$OUT/shell.log" 2>&1 &
SH=$!
sleep 6
dbus-update-activation-environment WAYLAND_DISPLAY=wayland-ipl
kill -0 "$SH" 2>/dev/null || { echo "FAIL le Shell n'a pas démarré :"; tail -5 "$OUT/shell.log"; exit 1; }
gnome-extensions enable pilote@test
for _ in $(seq 60); do [ -e "$OUT/doublures" ] && break; sleep 0.1; done
gnome-extensions enable "$UUID"
sleep 3
pilote steady off
expect "relié" "$(field s00 "d['view']['link']")" '"linked"'
CACHE="$XDG_CACHE_HOME/landline/app-icons"

# --- la pastille : une notification, puis trois d'applis différentes ------------
moteur notification-appli whatsapp; sleep 1.5
expect "pastille : l'icône de WhatsApp à la place des initiales" "$(field a10 "[d['view']['pill']['strong'], d['pillAppIcons']]")" '["WhatsApp", 1]'
expect "gardée dans le cache" "$(ls "$CACHE")" "net.whatsapp.WhatsApp.png"
shot pastille-une
pilote click; sleep 1.2
expect "sa carte : l'icône en tête" "$(field a11 "[d['view']['card']['kind'], d['menuAppIcons']]")" '["note", 1]'
shot carte
pilote key Escape; sleep 1
moteur notification-appli teams Léa; sleep 0.3; moteur notification-appli telegram; sleep 0.3; moteur notification-appli signal Marc; sleep 1.5
expect "trois en rafale : trois icônes" "$(field a12 "[d['view']['pill']['strong'], d['pillAppIcons']]")" '["3 notifications", 3]'
shot pastille-groupee
sleep 7

# --- « On your iPhone », puis le résumé au retour --------------------------------
moteur notification-appli mail Régie; sleep 0.3; moteur notification-appli calendrier Dentiste; sleep 7
pilote click; sleep 1.5
shot menu-on-your-iphone
expect "le menu : une icône par appli, initiales pour les inconnues" "$(field a20 "d['menuAppIcons'] >= 5")" 'true'
pilote key Escape; sleep 0.8
pilote clearall; sleep 0.3
moteur pile 9; sleep 0.3; moteur reconnexion; sleep 3.6
expect "retour : le résumé et ses icônes" "$(field a30 "[d['view']['mode'], d['pillAppIcons']]")" '["summary", 2]'
shot resume-retour
pilote click; sleep 1.5
shot menu-apres-resume
pilote key Escape; sleep 0.8

# --- « Earlier » ------------------------------------------------------------------
pilote reglage keep-history on; sleep 0.5
moteur notification-appli teams Léa; sleep 0.4; moteur notification-appli telegram Zoé; sleep 7.5
moteur retire-notification; sleep 0.4; moteur retire-notification; sleep 0.8
pilote click; sleep 1.2
pilote label Earlier; sleep 1.2
shot menu-earlier
pilote key Escape; sleep 0.8
pilote reglage keep-history off; sleep 0.5

# --- réglage éteint : plus d'icônes de l'App Store ----------------------------------
pilote reglage show-app-icons off; sleep 1
moteur notification-appli whatsapp Léo; sleep 1.5
expect "éteint : les initiales reviennent" "$(field a40 "[d['view']['pill']['strong'], d['pillAppIcons']]")" '["WhatsApp", 0]'
shot pastille-eteint
sleep 7
# L'appli installée sur cet ordinateur prête la sienne (Signal Desktop, ici).
if [ -e /usr/share/applications/signal-desktop.desktop ]; then
    moteur notification-appli signal Marc; sleep 1.5
    expect "éteint : l'icône de Signal Desktop" "$(field a41 "d['pillAppIcons']")" '1'
    shot pastille-eteint-appli-du-bureau
    pilote click; sleep 1.2
    shot carte-eteint-appli-du-bureau
    pilote key Escape; sleep 1
fi
pilote reglage show-app-icons on; sleep 0.5

# --- Phone Settings : le réglage --------------------------------------------------------
pilote click; sleep 1; pilote label "Phone Settings"
for _ in $(seq 40); do [ "$(field w "d['windows']")" = 1 ] && break; sleep 0.3; done
sleep 4
# Sans moteur installé, la fenêtre s'ouvre sur « Engine » : l'onglet iPhone.
pilote clicxy 580 262; sleep 1.5
pilote scroll ${DEFILE:-14}; sleep 1.5
shot reglages
pilote fermefenetres; sleep 1

echo "--- erreurs JS du Shell"
grep -n "JS ERROR\|landline.*\(Error\|warn\)\|CRITICAL" "$OUT/shell.log" | grep -v pilote | head -20
[ "$failed" = 0 ] && echo "tout est passé" || echo "DES ÉCHECS"
exit $failed
