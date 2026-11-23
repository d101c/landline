#!/usr/bin/env bash
# L'état du lien par l'icône seule (#51) : un démarrage du Shell isolé contre
# le faux moteur ; pour le repos puis chaque état dégradé, l'icône agrandie
# et le menu ouvert.
#
#   SHOTS=/chemin/captures INNER=tools/shell-test/proto-marques.sh tools/shell-test/run.sh
#   (SCHEME=prefer-light pour le style clair, STYLE=handset pour le combiné)
set -u
OUT="$PILOTE_DIR"; RT="$XDG_RUNTIME_DIR"
UUID="landline@d101c.github.io"
SOCK="$FAUX_SOCKET"
failed=0
P="${PREFIXE:-m}"

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
}
# L'icône agrandie ×8 (au plus proche voisin), puis le menu ouvert.
pair() {
    local name="$1"
    read -r x y w h < <(field "${name}-i" "' '.join(str(d['icon'][k]) for k in ('x', 'y', 'width', 'height'))" | tr -d '"')
    shot "${name}-barre"
    magick "$OUT/${name}-barre.png" -crop "$((w + 16))x$((h + 16))+$((x - 8))+$((y - 8))" +repage \
        -filter point -resize 800% "$SHOTS/$P-$name-icone.png"
    pilote click; sleep 1
    shot "${name}-menu"
    magick "$OUT/${name}-menu.png" -gravity NorthEast -crop "540x780+0+0" +repage "$SHOTS/$P-$name-menu.png"
    pilote key Escape; sleep 0.6
}
moteur() { echo "$*" >&3; }
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
pilote state s00
[ -e "$OUT/s00.json" ] || { echo "FAIL le pilote ne répond pas"; exit 1; }
pilote steady off
pilote helper; sleep 1
[ "${STYLE:-phone}" = handset ] && pilote texte icon-style handset && sleep 0.5

# --- tout va bien : l'icône nue, le menu sans un mot sur le lien
expect "relié" "$(field m00 "[d['view']['link'], d['icon']['state'], d['view']['card']['notices']]")" '["linked", "linked", []]'
pair 1-repos

# --- hors de portée
moteur hors-portee; sleep 1.5
expect "hors de portée : atténuée, une ligne" "$(field m10 "[d['icon']['state'], d['view']['card']['notices']]")" '["away", ["Out of range"]]'
pair 2-hors-de-portee
moteur relie; sleep 1.5

# --- un lien mort avant d'avoir servi, le moteur rappelle l'iPhone : reconnexion
moteur hors-portee; sleep 1.2
moteur lien-inutilise 1 holding_off; sleep 1.8
expect "reconnexion" "$(field m20 "[d['icon']['state'], len(d['view']['card']['notices'])]")" '["reconnecting", 1]'
pair 3-reconnexion
moteur relie; sleep 2

# --- notifications en pause
moteur notifications-en-pause phone_silent; sleep 1.5
expect "pause : les deux barres" "$(field m30 "[d['icon']['state'], d['view']['card']['paused']['cause']]")" '["paused", "phone_silent"]'
pair 4-notifications-en-pause
moteur notifications-reprises; sleep 1.5

# --- un réglage de l'ordinateur manque : la marque ambre, Finish Setting Up
echo '{"experimental": true, "ertm": false, "persistent": true, "le_params": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 1.5
expect "réglage manquant : attention" "$(field m40 "[d['icon']['state'], d['view']['card']['permissionAction']]")" '["attention", true]'
pair 5-reglage-manquant
echo '{"experimental": true, "ertm": true, "persistent": true, "le_params": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 1.5

# --- notifications pas encore partagées par l'iPhone : Continue Setup
moteur sobre; moteur sans-accord; sleep 1.5
expect "notifications non partagées : attention" "$(field m50 "[d['icon']['state'], d['view']['card']['setupAction']]")" '["attention", true]'
pair 6-notifications-non-partagees
moteur accord; moteur complet; moteur relie; sleep 1.5

# --- messages indisponibles : Fix It après 20 s
moteur obex-injoignable; sleep 22
expect "messages indisponibles : attention, Fix It" "$(field m60 "[d['icon']['state'], d['view']['card']['fixAction']]")" '["attention", true]'
pair 7-messages-indisponibles
moteur obex-retabli; moteur relie; sleep 2

# --- batterie faible : la petite pile rouge, jusqu'à l'ouverture du menu
moteur batterie-faible; sleep 1.5
expect "batterie faible" "$(field m70 "[d['icon']['state'], d['view']['card']['batteryLow']]")" '["battery", true]'
pair 8-batterie-faible

exit $failed
