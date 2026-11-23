#!/usr/bin/env bash
# #52, #53, #50 (proposition) : le menu et la carte des liens tel: dans le
# Shell isolé, un seul démarrage contre le faux moteur.
#
#   INNER=tools/shell-test/proto-cartes.sh SHOTS=/tmp/captures tools/shell-test/run.sh
set -u
OUT="$PILOTE_DIR"; RT="$XDG_RUNTIME_DIR"
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
# Le lien comme le ferait le fichier desktop.
lien() {
    gdbus call --session --dest org.gnome.Shell --object-path /io/github/d101c/Landline \
        --method io.github.d101c.Landline.PhoneLinks.Call "$1" > "$OUT/gdbus.log" 2>&1
}
erreurs() { grep -c "JS ERROR" "$OUT/shell.log"; }
cle() { gsettings --schemadir "$HOME/data/gnome-shell/extensions/$UUID/schemas" get org.gnome.shell.extensions.landline "$1"; }
cleanup() { kill "${SH:-}" "${MOTEUR:-}" 2>/dev/null; }
trap cleanup EXIT

echo '{"experimental": true, "ertm": true, "persistent": true, "le_params": true}' > "$FAUX_SETUP_STATE"
rm -f "$RT/scenario"; mkfifo "$RT/scenario"
python3 "$REPO/tools/faux-moteur.py" --socket "$SOCK" --etat relie < "$RT/scenario" >> "$OUT/moteur.log" 2>&1 &
MOTEUR=$!
exec 3> "$RT/scenario"
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.desktop.interface color-scheme "$SCHEME"
gsettings set org.gnome.desktop.interface enable-animations false
gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x1024 \
    --wayland-display=wayland-ipl > "$OUT/shell.log" 2>&1 &
SH=$!
sleep 6
dbus-update-activation-environment WAYLAND_DISPLAY=wayland-ipl
kill -0 "$SH" 2>/dev/null || { echo "FAIL le Shell n'a pas démarré :"; tail -5 "$OUT/shell.log"; exit 1; }
gnome-extensions enable pilote@test
for _ in $(seq 60); do [ -e "$OUT/doublures" ] && break; sleep 0.1; done
[ -e "$OUT/doublures" ] || { echo "FAIL le pilote n'a pas posé ses doublures"; exit 1; }
gnome-extensions enable "$UUID"
sleep 3
for _ in $(seq 20); do
    rm -f "$OUT/s00.json"; echo "state s00" > "$OUT/cmd"; sleep 0.5
    lie=$(python3 -c "import json; print(json.load(open('$OUT/s00.json'))['view']['link'])" 2>/dev/null)
    [ "$lie" = linked ] && break
done
pilote steady off
pilote helper; sleep 1
expect "relié" "$(field s00 "d['view']['link']")" '"linked"'
E0=$(erreurs)

# --- #50 : la proposition du premier lancement --------------------------------
# Cette machine a peut-être un gestionnaire de tel: ou callto: (Zoom…) : le
# pilote dit au modèle « aucun », comme sur une machine qui n'en a pas.
pilote offre on
pilote click; sleep 1
expect "#50 menu ouvert : la proposition, notée aussitôt" \
    "$(field p01 "[d['view']['card']['phoneLinksOffer'], 'Use Landline' in d['menuTexts'], 'Not Now' in d['menuTexts']]"), $(cle phone-links-offered)" \
    '[true, true, true], true'
shot c1-proposition-liens
pilote label Not Now; sleep 1
expect "#50 Not Now : la ligne s'en va, rien d'écrit" \
    "$(field p02 "[d['view']['card']['phoneLinksOffer'], 'Use Landline' in d['menuTexts']]"), $(ls "$HOME/data/applications/landline-phone-links.desktop" 2>/dev/null | wc -l)" \
    '[false, false], 0'
pilote key Escape; sleep 0.6
pilote offre on
pilote click; sleep 1
expect "#50 plus jamais (la clé est posée)" "$(field p03 "d['view']['card']['phoneLinksOffer']")" 'false'
pilote key Escape; sleep 0.6

# --- #53 : une touche à l'ouverture ne compose jamais -------------------------
pilote reglage phone-links on; sleep 0.5
A0=$(recu bt_call_dial)
lien "tel:+41790000001"; sleep 0.15
# Tab vers Call puis Entrée tout de suite : ignorée (moins de 500 ms).
pilote key Tab; pilote key Return; sleep 0.1
expect "#53 Entrée sur Call dans les 500 ms : rien de composé" \
    "$(field c02 "[d['menuOpen'], d['view']['card']['kind'], d['focusName']]"), $(( $(recu bt_call_dial) - A0 ))" '[true, "phonelink", "Call"], 0'
sleep 0.6
pilote key Return; sleep 1.2
expect "#53 Entrée sur Call ensuite (Tab) : composé" "$(( $(recu bt_call_dial) - A0 ))" 1
moteur raccroche; sleep 1
pilote key Escape; sleep 0.8

A1=$(recu bt_call_dial)
lien "tel:+41790000002"; sleep 0.1
pilote label Call; sleep 1.2
expect "#53 un clic sur Call, même aussitôt : composé" "$(( $(recu bt_call_dial) - A1 ))" 1
moteur raccroche; sleep 1
pilote key Escape; sleep 0.8

A2=$(recu bt_call_dial)
lien "tel:+41790000003"; sleep 0.2
expect "#53 la carte descend, le focus sur Cancel" \
    "$(field c01 "[d['menuOpen'], d['view']['card']['kind'], d['focusName']]")" '[true, "phonelink", "Cancel"]'
shot c2-lien-focus-cancel
pilote key Return; sleep 0.8
expect "#53 Entrée à l'ouverture : Cancel, rien de composé" \
    "$(field c03 "d['menuOpen']"), $(( $(recu bt_call_dial) - A2 ))" 'false, 0'

# --- #52 : la carte devient « direct » menu ouvert -----------------------------
# (Sans appel en cours, quoi qu'il soit arrivé plus haut.)
moteur raccroche; sleep 1
pilote click; sleep 1
expect "#52 menu ouvert" "$(field c10 "d['menuOpen']")" 'true'
moteur sans-iphone; moteur appairage-partiel; sleep 2
expect "#52 Link again menu ouvert : le menu se ferme, aucune erreur JS" \
    "$(field c11 "[d['view']['card']['kind'], d['menuOpen']]"), $(( $(erreurs) - E0 ))" '["direct", false], 0'
shot c3-link-again-menu-ferme

echo "--- erreurs JS du Shell"
grep -n "JS ERROR\|landline.*\(Error\|warn\)\|CRITICAL" "$OUT/shell.log" | grep -v pilote | head -20
[ "$failed" = 0 ] && echo "tout est passé" || echo "DES ÉCHECS"
exit $failed
