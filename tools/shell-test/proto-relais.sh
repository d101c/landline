#!/usr/bin/env bash
# #49 et #50 : passer le relais aux applis officielles, liens tel:. Un seul
# démarrage du Shell isolé contre le faux moteur, une capture par point (r1 à r6).
#
#   INNER=tools/shell-test/proto-relais.sh SHOTS=/tmp/captures tools/shell-test/run.sh
#
# Les applis vues sont celles du système (XDG_DATA_DIRS n'est pas changé) :
# ce que le bouton « Open in … » propose dépend donc de la machine. Aucun clic
# n'est fait sur ce bouton (il ouvrirait la vraie appli).
set -u
S="$(cd "$(dirname "$0")" && pwd)"; OUT="$PILOTE_DIR"; RT="$XDG_RUNTIME_DIR"
UUID="landline@d101c.github.io"
SOCK="$FAUX_SOCKET"
failed=0

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
    cp "$OUT/$1.png" "$SHOTS/$1.png"
}
moteur() { echo "$*" >&3; }
recu() { grep -c "RECU .*$1" "$OUT/moteur.log"; }
lien() {
    gdbus call --session --dest org.gnome.Shell --object-path /io/github/d101c/Landline \
        --method io.github.d101c.Landline.PhoneLinks.Call "$1" > "$OUT/gdbus.log" 2>&1
}
cleanup() { kill "${SH:-}" "${MOTEUR:-}" 2>/dev/null; }
trap cleanup EXIT

echo '{"experimental": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
rm -f "$RT/scenario"; mkfifo "$RT/scenario"
python3 "$REPO/tools/faux-moteur.py" --socket "$SOCK" --etat relie ${MOTEUR_DEBUT:-} < "$RT/scenario" >> "$OUT/moteur.log" 2>&1 &
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
# Le pilote et l'extension prennent parfois quelques secondes de plus.
for _ in $(seq 20); do
    rm -f "$OUT/s00.json"; echo "state s00" > "$OUT/cmd"; sleep 0.5
    lie=$(python3 -c "import json; print(json.load(open('$OUT/s00.json'))['view']['link'])" 2>/dev/null)
    [ "$lie" = linked ] && break
done
pilote steady off
expect "relié" "$(field s00 "d['view']['link']")" '"linked"'

# --- 1. Signal : la bonne conversation, le vocal dit où l'écouter ---------------
moteur notification-appli signal-vocal +41 79 000 00 07; sleep 1.2
pilote click; sleep 1.2
CARTE=$(field r10 "d['view']['card']")
echo "     carte Signal : $CARTE"
expect "vocal Signal : la phrase dit où l'écouter" "$(field r10 "d['view']['card']['handoff']['media'].startswith('Voice message — play it ')")" 'true'
SIGNAL=$(field r10 "d['view']['card']['handoff']['label']")
if [ "$SIGNAL" = '"Open in Signal"' ]; then
    expect "Signal Desktop installé : sur la conversation" "$(field r11 "d['view']['card']['handoff']['open']")" '{"uri": "sgnl://signal.me/#p/+41790000007"}'
    expect "...le bouton est dans la carte" "$(field r12 "'Open in Signal' in d['menuTexts']")" 'true'
else
    echo "     (Signal Desktop non vu par ce Shell : $SIGNAL)"
fi
shot r1-signal-vocal
pilote key Escape; sleep 0.8

# --- 2. Telegram absent : dit une fois, sobrement --------------------------------
moteur notification-appli telegram-vocal; sleep 1.2
pilote click; sleep 1.2
expect "Telegram Desktop absent : Telegram Web, et une ligne qui le dit" \
    "$(field r20 "[d['view']['card']['handoff']['label'], (d['view']['card']['suggestion'] or {}).get('get')]")" \
    '["Open in Telegram Web", "Get Telegram Desktop"]'
expect "...retenu pour de bon" "$(field r21 "d['view']['card']['suggestion'] is not None")" 'true'
shot r2-telegram-suggestion
pilote key Escape; sleep 0.8
expect "...noté dans les réglages" "$(gsettings --schemadir "$HOME/data/gnome-shell/extensions/$UUID/schemas" get org.gnome.shell.extensions.landline suggested-apps)" "['telegram']"
sleep 6
moteur notification-appli telegram Léo; sleep 1.2
pilote click; sleep 1.2
expect "Telegram encore : plus rien de suggéré" "$(field r22 "d['view']['card']['suggestion']")" 'null'
shot r2-telegram-deuxieme
pilote key Escape; sleep 7

# --- 3. Discord, Teams ----------------------------------------------------------
moteur notification-appli teams; sleep 1.2
pilote click; sleep 1.2
expect "Teams : son client web ou l'appli" "$(field r30 "d['view']['card']['handoff']['label'] in ['Open Teams in the Browser', 'Open in Teams']")" 'true'
shot r3-teams
pilote key Escape; sleep 7
moteur notification-appli whatsapp-photo; sleep 1.2
pilote click; sleep 1.2
expect "WhatsApp, photo : le client web officiel, sur la conversation" \
    "$(field r31 "[d['view']['card']['handoff']['label'], d['view']['card']['handoff']['media']]")" \
    '["Open in WhatsApp Web", "Photo — see it in WhatsApp Web"]'
shot r3-whatsapp-photo
pilote key Escape; sleep 7

# --- 4. un lien tel: ------------------------------------------------------------
lien "tel:+41 79 000 00 01"
expect "option éteinte : personne n'écoute, l'appel échoue tout de suite" "$(grep -c 'does not exist' "$OUT/gdbus.log")" 1
pilote reglage phone-links on; sleep 0.5
expect "option allumée : l'objet est exporté" "$(field r40 "d['phoneLinks']")" 'true'
lien "tel:+41-79-000-00-01"; sleep 1.2
expect "lien tel: : la confirmation descend de la barre" \
    "$(field r41 "[d['menuOpen'], d['view']['card']['kind'], d['view']['card']['name'], d['view']['card']['canCall']]")" \
    '[true, "phonelink", "Call Camille Martin?", true]'
shot r4-confirmation-appel
A0=$(recu bt_call_dial)
pilote label Call; sleep 1.5
expect "Call : le chemin de « Call… »" "$(( $(recu bt_call_dial) - A0 ))" 1
shot r4-appel-en-route
moteur raccroche; sleep 1
pilote key Escape; sleep 1

lien "tel:0790000009"; sleep 1.2
expect "numéro inconnu : le numéro, tel qu'écrit" "$(field r42 "[d['view']['card']['name'], d['view']['card']['subtitle']]")" '["Call 079 000 00 09?", "Through your iPhone"]'
pilote label Cancel; sleep 1
expect "Cancel : rien n'est composé" "$(field r43 "d['menuOpen']"), $(( $(recu bt_call_dial) - A0 ))" 'false, 1'

# --- 5. l'iPhone absent ------------------------------------------------------------
moteur hors-portee; sleep 2
lien "tel:+41790000001"; sleep 1.2
expect "iPhone absent : la carte le dit" "$(field r50 "[d['view']['card']['why'], d['view']['card']['canCall']]")" \
    '["Your iPhone isn’t connected, so the call can’t be placed from here.", false]'
shot r5-iphone-absent
pilote key Escape; sleep 1
moteur relie; sleep 2

# --- 6. extension désactivée : le lien échoue proprement --------------------------
gnome-extensions disable "$UUID"; sleep 1.5
lien "tel:+41790000001"
expect "extension désactivée : l'appel échoue tout de suite" "$(grep -c 'does not exist' "$OUT/gdbus.log")" 1
gnome-extensions enable "$UUID"; sleep 3

# --- 7. Phone Settings : les nouvelles lignes ---------------------------------------
pilote click; sleep 1; pilote label "Phone Settings"
for _ in $(seq 40); do [ "$(field w "d['windows']")" = 1 ] && break; sleep 0.3; done
sleep 4
# Sans moteur installé dans ce HOME, la fenêtre s'ouvre sur « Engine » : l'onglet « iPhone ».
pilote pointe ${ONGLET_IPHONE:-580 262}; sleep 1.5
pilote scroll ${DEFILE_APPELS:-9}; sleep 1.5
shot r6-reglages-appels
# L'interrupteur « Use Landline for phone links » : une autre appli a callto:
# sur cette machine, la question vient d'abord ; Échap = Cancel, rien d'écrit.
pilote pointe ${INTER_LIENS:-905 435}; sleep 1.5
shot r6-reglages-question
pilote key Escape; sleep 1
expect "question refusée : aucun fichier desktop écrit" "$(ls "$HOME/data/applications/landline-phone-links.desktop" 2>/dev/null | wc -l)" 0
pilote pointe ${INTER_LIENS:-905 435}; sleep 1.5
pilote pointe ${BOUTON_OUI:-710 588}; sleep 2
expect "Use Landline : le fichier est écrit, défaut de tel: (dans le HOME de l'essai)" \
    "$(ls "$HOME/data/applications/landline-phone-links.desktop" 2>/dev/null | wc -l), $(gio mime x-scheme-handler/tel 2>/dev/null | head -1 | grep -c landline-phone-links)" '1, 1'
shot r6-reglages-allume
# Le chemin entier : « gio open tel:… » lance le fichier desktop, qui appelle l'extension.
gio open "tel:+41 79 000 00 01" > /dev/null 2>> "$OUT/gdbus.log"; sleep 2
expect "gio open tel: : la confirmation descend de la barre" \
    "$(field r60 "[d['menuOpen'], d['view']['card']['kind'], d['view']['card']['name']]")" '[true, "phonelink", "Call Camille Martin?"]'
shot r6-gio-open-tel
pilote key Escape; sleep 1
pilote pointe ${INTER_LIENS:-905 435}; sleep 2
expect "éteint : le fichier est retiré, tel: rendu" \
    "$(ls "$HOME/data/applications/landline-phone-links.desktop" 2>/dev/null | wc -l), $(gio mime x-scheme-handler/tel 2>/dev/null | head -1 | grep -c landline-phone-links)" '0, 0'
pilote scroll ${DEFILE_APPLIS:-7}; sleep 1.5
shot r6-reglages-applis
pilote fermefenetres; sleep 1

echo "--- erreurs JS du Shell"
grep -n "JS ERROR\|landline.*\(Error\|warn\)\|CRITICAL" "$OUT/shell.log" | grep -v pilote | head -20
[ "$failed" = 0 ] && echo "tout est passé" || echo "DES ÉCHECS"
exit $failed
