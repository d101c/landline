#!/usr/bin/env bash
# Prototype « notifs » : un seul démarrage du Shell isolé contre le faux moteur,
# une capture par point (n1 à n5). Pas de vérification fine : on regarde que
# l'extension se charge sans erreur JS et que les nouveaux écrans s'affichent.
#
#   INNER=tools/shell-test/proto-notifs.sh tools/shell-test/run.sh
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
pilote steady off
expect "relié" "$(field s00 "d['view']['link']")" '"linked"'

# --- 1. un seul système de notification -------------------------------------
moteur notification-appli whatsapp; sleep 1.2
expect "notification d'appli : la pastille" "$(field n10 "[d['view']['mode'], d['view']['pill']['strong']]")" '["note", "WhatsApp"]'
shot n1-pastille-appli
pilote click; sleep 1.2
expect "clic : sa carte" "$(field n11 "[d['menuOpen'], d['view']['card']['kind']]")" '[true, "note"]'
shot n1-carte-appli
pilote label Dismiss; sleep 1
expect "Dismiss : effacée sur l'iPhone, carte fermée" "$(field n12 "d['menuOpen']"), $(recu 'bt_notification_action.*negative')" 'false, 1'
moteur notification-appli whatsapp Léo; sleep 0.4; moteur notification; sleep 0.4; moteur notification-appli mail; sleep 1
expect "trois en rafale : une seule pastille" "$(field n13 "d['view']['pill']['strong']")" '"3 notifications"'
shot n1-pastille-groupee
sleep 7
moteur appel-manque; sleep 0.5; moteur batterie-faible; sleep 1.2
expect "appel manqué, batterie faible : l'icône, pas de pastille" "$(field n14 "[d['view']['mode'], d['view']['icon']['state']]")" '["idle", "missed"]'
pilote click; sleep 1.2
shot n1-menu-manque-batterie
pilote key Escape; sleep 0.8
expect "menu vu : l'icône revient au repos" "$(field n15 "d['view']['icon']['state']")" '"linked"'

# --- 2. « On your iPhone » ---------------------------------------------------
moteur pile; sleep 0.3; moteur reconnexion; sleep 1.5
pilote click; sleep 1.2
TOTAL=$(field n20 "d['view']['card']['phone']['total']")
expect "ce qui attendait sur l'iPhone est listé, plafonné" "$(field n20 "[d['view']['card']['phone']['hidden'] > 0, 'On your iPhone' in d['menuTexts']]")" '[true, true]'
shot n2-menu-on-your-iphone
pilote label "Show all ($TOTAL)"; sleep 1.2
expect "Show all" "$(field n21 "d['view']['card']['phone']['hidden']")" 0
shot n2-menu-show-all
moteur retire-notification; sleep 1
expect "retirée sur l'iPhone : une de moins" "$(field n22 "d['view']['card']['phone']['total']")" "$((TOTAL - 1))"
A0=$(recu 'bt_notification_action.*negative')
pilote label "Clear All"; sleep 1.5
expect "Clear All : tout est effacé sur l'iPhone, la section disparaît" "$(field n23 "d['view']['card']['phone']"), $(( $(recu 'bt_notification_action.*negative') - A0 ))" "null, $((TOTAL - 1))"
shot n2-menu-vide
pilote key Escape; sleep 0.8

# --- 3. résumé au retour ------------------------------------------------------
moteur pile 9; sleep 0.3; moteur reconnexion; sleep 3.6
expect "retour de l'iPhone : un résumé" "$(field n30 "[d['view']['mode'], d['view']['pill']['strong'], d['view']['pill']['text']]")" '["summary", "While you were away:", "5 WhatsApp, 2 Mail, 1 Calendar and 1 more"]'
shot n3-resume-retour
pilote click; sleep 1.2
expect "clic : le menu, avec ce qui attend" "$(field n31 "[d['menuOpen'], d['view']['card']['phone']['total']]")" '[true, 9]'
shot n3-menu-apres-resume
pilote key Escape; sleep 0.8

# --- 4. historique « Earlier », en option ---------------------------------------
expect "réglage éteint : pas d'historique" "$(field n40 "d['view']['card']['earlier']")" 'null'
pilote reglage keep-history on; sleep 0.5
moteur notification Coiffeur; sleep 0.5; moteur notification-appli signal Marc; sleep 7.5
moteur retire-notification; sleep 0.4; moteur retire-notification; sleep 0.8
pilote click; sleep 1.2
pilote label Earlier; sleep 1
expect "historique actif : ce qui a quitté l'iPhone est sous Earlier" "$(field n41 "[len(d['view']['card']['earlier']), 'Clear history' in d['menuTexts']]")" '[2, true]'
shot n4-menu-earlier
pilote label "Clear history"; sleep 1
expect "Clear history" "$(field n42 "d['view']['card']['earlier']")" '[]'
pilote key Escape; sleep 0.8
pilote reglage keep-history off; sleep 0.5

# --- 5. aperçu des icônes dans Phone Settings -----------------------------------
pilote click; sleep 1; pilote label "Phone Settings"
for _ in $(seq 40); do [ "$(field w "d['windows']")" = 1 ] && break; sleep 0.3; done
sleep 4
expect "Phone Settings ouvert" "$(field n50 "d['windows']")" 1
shot n5-reglages-haut
pilote scroll ${DEFILE:-34}; sleep 1.5
shot n5-reglages-icones
pilote texte icon-style handset; sleep 1
expect "style changé : l'icône de la barre suit" "$(field n51 "d['icon']['style'] if isinstance(d.get('icon'), dict) else None")" '"handset"'
shot n5-reglages-icones-handset
pilote texte icon-style phone; sleep 0.5
pilote fermefenetres; sleep 1

# --- suite
echo "--- erreurs JS du Shell"
grep -n "JS ERROR\|landline.*\(Error\|warn\)\|CRITICAL" "$OUT/shell.log" | grep -v pilote | head -20
[ "$failed" = 0 ] && echo "tout est passé" || echo "DES ÉCHECS"
exit $failed
