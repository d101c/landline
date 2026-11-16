#!/usr/bin/env bash
# Tourne dans le bus D-Bus privé ouvert par run.sh.
set -u
S="$(cd "$(dirname "$0")" && pwd)"; OUT="$PILOTE_DIR"; RT="$XDG_RUNTIME_DIR"
UUID="landline@d101c.github.io"
SOCK="$FAUX_SOCKET"
failed=0

pilote() {  # pilote <commande...> : passe une commande à pilote@test et attend
    echo "$*" > "$OUT/cmd"
    for _ in $(seq 100); do [ -e "$OUT/cmd" ] || return 0; sleep 0.05; done
    echo "le pilote n'a pas pris : $*" >&2; failed=1
}
field() {   # field <état> <expression python sur d>
    pilote state "$1"
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(json.dumps($2, ensure_ascii=False))" "$OUT/$1.json" 2>&1 | tail -1
}
expect() {  # expect <libellé> <obtenu> <attendu>
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1 (obtenu $2, attendu $3)"; failed=1; fi
}
shot() {    # shot <nom> : capture, attend la fin de l'écriture, range une copie allégée
    pilote shot "$1"
    for _ in $(seq 60); do tail -c 12 "$OUT/$1.png" 2>/dev/null | grep -q IEND && break; sleep 0.05; done
    magick "$OUT/$1.png" -colors 255 "PNG8:$SHOTS/$1.png" 2>/dev/null || cp "$OUT/$1.png" "$SHOTS/$1.png"
}
moteur() { echo "$*" >&3; }
start_moteur() {
    rm -f "$RT/scenario"; mkfifo "$RT/scenario"
    python3 "$REPO/tools/faux-moteur.py" --socket "$SOCK" "$@" < "$RT/scenario" >> "$OUT/moteur.log" 2>&1 &
    MOTEUR=$!
    exec 3> "$RT/scenario"
}
recu() { grep -c "RECU .*$1" "$OUT/moteur.log"; }
cleanup() { kill "${SH:-}" "${MOTEUR:-}" 2>/dev/null; }
trap cleanup EXIT

rm -f "$FAUX_SETUP_STATE"
start_moteur --etat sans-iphone
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.desktop.interface color-scheme "$SCHEME"
gsettings set org.gnome.desktop.interface enable-animations true
[ "$SCALE" = 1 ] || gsettings set org.gnome.desktop.interface scaling-factor "$SCALE"
gnome-shell --headless --wayland --no-x11 --virtual-monitor "$((1280 * SCALE))x$((720 * SCALE))" \
    --wayland-display=wayland-ipl > "$OUT/shell.log" 2>&1 &
SH=$!
sleep 6
# Pour que la fenêtre des réglages, lancée par le bus, s'ouvre dans ce Shell.
dbus-update-activation-environment WAYLAND_DISPLAY=wayland-ipl
kill -0 "$SH" 2>/dev/null || { echo "FAIL le Shell n'a pas démarré :"; tail -5 "$OUT/shell.log"; exit 1; }
# Le pilote d'abord : il pose ses doublures avant que l'extension ne démarre.
gnome-extensions enable pilote@test
for _ in $(seq 60); do [ -e "$OUT/doublures" ] && break; sleep 0.1; done
[ -e "$OUT/doublures" ] || { echo "FAIL le pilote n'a pas posé ses doublures :"; grep -A4 -i "pilote\|JS ERROR" "$OUT/shell.log" | head -20; exit 1; }
gnome-extensions enable "$UUID"
sleep 3
pilote state s00
[ -e "$OUT/s00.json" ] || { echo "FAIL le pilote ne répond pas :"; grep -A4 -i "pilote\|JS ERROR" "$OUT/shell.log" | head -20; exit 1; }

# Les parties écrites avant l'indicateur stable attendent chaque état dans la
# seconde : le délai de 3 s et le verrou « Link again » n'y sont pas en service.
# La partie « stable » les remet.
pilote steady off

wait_for() {  # wait_for <expression python sur d> <attendu> [essais] : attend, 0,3 s par essai
    for _ in $(seq "${3:-100}"); do [ "$(field w "$1")" = "$2" ] && return 0; sleep 0.3; done
    return 1
}
want() { [ -z "$SECTIONS" ] || case " $SECTIONS " in *" $1 "*) return 0;; *) return 1;; esac; }
tour() { field "$1" "$2"; }

if want parcours; then
# ============================================================================
# Le parcours d'accueil, du premier clic à « tout marche » (captures t*)
# ============================================================================
moteur delai balayage 12
expect "réglages manquants : Permission needed" "$(field p01 "[d['view']['link'], d['strong']]")" '["permission", "Permission needed"]'
shot t00-pastille-permission
pilote click; sleep 1.2
expect "clic : le parcours s'ouvre sur l'autorisation, pas de menu" "$(field t01 "[d['tourDialog'], d['menuOpen'], d['view']['tour']['step'], d['tourStepLabel']]")" '[true, false, "permission", "Step 1 of 7 — Permission"]'
expect "le clavier est sur le parcours, sur Continue" "$(field t01 "[d['grabIsTour'], d['focusInTour'], d['focusLabel']]")" '[true, true, "Continue"]'
expect "deux réglages manquent : deux points, plus de « hands-free »" "$(field t01 "[d['view']['tour']['permissionNeeds'], any('Two Bluetooth settings' in t for t in d['tourTexts']), any('hands-free' in t.lower() for t in d['tourTexts'])]")" '[["experimental", "ertm"], true, false]'
shot t01-autorisation
pilote key Escape; sleep 0.8
expect "Échap : rien ne change, rien n'est demandé" "$(field t02 "[d['tourDialog'], d['strong']]"), $(cat "$OUT/pkexec.log" 2>/dev/null | wc -l)" '[false, "Permission needed"], 0'

touch "$RT/refuse"
pilote click; sleep 1.2; pilote key Return; sleep 1.6
expect "Entrée lance la demande ; mot de passe refusé : on reste sur l'étape, avec Try Again" "$(field t03 "[d['tourDialog'], d['view']['tour']['step'], d['view']['tour']['permission'], d['tourButtons'], d['focusLabel']]"), $(grep -c "landline-setup apply" "$OUT/pkexec.log")" '[true, "permission", "refused", ["Not Now", "Try Again"], "Try Again"], 1'
shot t02-autorisation-refusee
rm "$RT/refuse"

# La fenêtre de mot de passe : pkexec attend, une fenêtre modale du Shell passe
# au-dessus du parcours (comme celle de polkit), puis tout reprend seul.
touch "$RT/attente"
pilote key Return; sleep 1
expect "en attente du mot de passe : dit sur le parcours, resté ouvert" "$(field t04 "[d['tourDialog'], d['view']['tour']['permission'], d['tourButtons']]")" '[true, "waiting", ["One moment…"]]'
pilote fakeauth open; sleep 1
expect "fenêtre du système au-dessus : elle a le clavier, le parcours est derrière" "$(field t05 "[d['authOpen'], d['tourDialog'], d['grabIsTour'], d['focusInTour']]")" '[true, true, false, false]'
shot t03-mot-de-passe
rm "$RT/attente"; sleep 2.2
expect "réglages appliqués pendant que la fenêtre est encore là : le parcours a déjà avancé" "$(field t06 "[d['view']['tour']['step'], d['authOpen'], d['focusInTour']]"), $(grep -c "landline-setup apply" "$OUT/pkexec.log")" '["find", true, false], 2'
pilote fakeauth close; sleep 1.2
expect "fenêtre fermée : le parcours reprend le clavier, sans clic" "$(field t07 "[d['grabIsTour'], d['focusInTour'], d['focusLabel'], d['tourStepLabel']]")" '[true, true, "Cancel", "Step 2 of 7 — Find your iPhone"]'
expect "la recherche est partie d'elle-même" "$(recu bt_scan)" 1
expect "étape 2 : où être sur l'iPhone" "$(field t07 "d['tourWhere']")" '"Settings › Bluetooth. Stay on that screen."'
shot t04-recherche

# Mis de côté, puis repris là où on en était
pilote key Escape; sleep 0.8
expect "Échap pendant la recherche : le parcours se met de côté, l'indicateur le dit" "$(field t08 "[d['tourDialog'], d['view']['tour']['step'], d['strong']]")" '[false, "find", "Continue setup"]'
shot t05-mis-de-cote
pilote click; sleep 1.2
expect "clic : repris à la même étape, sans nouvelle recherche" "$(field t09 "[d['tourDialog'], d['view']['tour']['step'], d['focusInTour']]"), $(recu bt_scan)" '[true, "find", true], 1'
for _ in $(seq 70); do [ "$(recu 'bt_pair"')" = 1 ] && break; sleep 0.2; done
sleep 0.5
expect "iPhone trouvé et appelé sans rien toucher : dit avec son nom" "$(field t10 "[d['view']['tour']['step'], d['view']['tour']['found'], d['view']['tour']['deviceName']]"), $(recu 'bt_pair"')" '["find", true, "iPhone de Camille"], 1'
shot t06-trouve
sleep 2.6
expect "étape 3 : le code, rien n'est le bouton par défaut" "$(field t11 "[d['view']['tour']['step'], d['view']['tour']['code'], d['tourButtons'], d['focusLabel'], d['focusInTour'], d['tourStepLabel']]")" '["code", "703405", ["They Differ", "They Match"], null, true, "Step 3 of 7 — Check the number"]'
shot t07-code
pilote key Return; sleep 0.6
expect "Entrée ne confirme pas le code" "$(recu 'bt_pair_confirm')" 0
moteur delai balayage 1
moteur expire-appairage; sleep 1.6
expect "délai dépassé : dit sans jargon, Try Again offert, étape gardée" "$(field t12 "[d['view']['tour']['step'], d['view']['tour']['canRetry'], d['tourButtons'], d['tourStepLabel']]")" '["failed", true, ["Cancel", "Try Again"], "Step 3 of 7 — Check the number"]'
shot t08-code-expire
pilote tpress retry; sleep 5.2
expect "Try Again : nouvelle recherche, nouveau code" "$(field t13 "d['view']['tour']['step']")" '"code"'
pilote tpress differ; sleep 1.6
expect "They Differ : refus envoyé, échec expliqué" "$(field t14 "d['view']['tour']['step']"), $(recu 'bt_pair_confirm.*false')" '"failed", 1'
shot t09-code-refuse
moteur delai liens 3
pilote tpress retry; sleep 5.2
pilote tpress match; sleep 0.9
expect "They Match : étape d'attente, rien n'est encore annoncé" "$(field t15 "[d['view']['tour']['step'], d['view']['link'], d['view']['tour']['links']]"), $(recu 'bt_pair_confirm.*true')" '["pairing", "away", {"classic": false, "le": false}], 1'
shot t10-jumelage
sleep 3.4
expect "les deux liens montés : étape des interrupteurs, rien de coché" "$(field t16 "[d['view']['tour']['step'], d['view']['tour']['switches']['messages'], d['tourStepLabel'], d['tourWhere']]")" '["switches", false, "Step 5 of 7 — Allow sharing", "Settings › Bluetooth › ⓘ next to “'"$(hostname)"'”"]'
expect "contrôle d'appel activé de lui-même une fois relié" "$(recu 'bt_set_calls.*true')" 1
shot t11-interrupteurs
moteur interrupteur notifications; moteur interrupteur messages; sleep 1
expect "deux interrupteurs vus par le moteur" "$(field t17 "[d['view']['tour']['switches'][k] for k in ('notifications', 'messages', 'contacts')]")" '[true, true, false]'
shot t12-deux-coches
moteur interrupteur contacts; sleep 0.5
expect "le troisième : la coche reste un instant à l'écran" "$(field t18 "[d['view']['tour']['step'], d['view']['tour']['allOn']]")" '["switches", true]'
sleep 2
expect "puis l'essai" "$(field t19 "[d['view']['tour']['step'], d['tourButtons'], d['tourStepLabel']]")" '["try", ["Back", "Skip"], "Step 6 of 7 — Try it"]'
shot t13-essai
moteur message Coucou, ça marche ?; sleep 1.2
expect "un message arrive pendant l'essai : montré, « It works »" "$(field t20 "[d['view']['tour']['proof'], d['tourButtons'], d['focusLabel']]")" '[{"name": "Camille Martin", "body": "Coucou, ça marche ?"}, ["Back", "Continue"], "Continue"]'
shot t14-essai-reussi
pilote tpress back; sleep 0.8
expect "Back : les interrupteurs, tous cochés, avec Continue" "$(field t21 "[d['view']['tour']['step'], d['tourButtons']]")" '["switches", ["Finish Later", "Continue"]]'
shot t15-retour-interrupteurs
pilote key Return; sleep 0.6; pilote key Return; sleep 1.2
expect "Entrée, Entrée : la fin, avec les vrais chiffres" "$(field t22 "[d['view']['tour']['step'], d['view']['tour']['contacts'], d['view']['tour']['conversations'], d['view']['tour']['calls'], d['tourStepLabel']]")" '["done", 214, 2, true, "Step 7 of 7 — Done"]'
shot t16-termine
pilote key Return; sleep 1
expect "Done : parcours fermé, relié" "$(field t23 "[d['tourDialog'], d['view']['link'], d['view']['tour']]")" '[false, "linked", null]'
sleep 7; pilote click; sleep 0.8; pilote press rows 0; sleep 0.6; pilote key Escape; sleep 0.6
moteur delai liens 0.8
fi

if want reparation; then
# ============================================================================
# Appairage qui existe mais ne marche pas (captures r*)
# ============================================================================
echo '{"experimental": true, "device_class": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur sans-iphone; moteur appairage-partiel; sleep 1.5
expect "appairage partiel fait hors du parcours : jamais « relié », Link again" "$(field r01 "[d['view']['link'], d['strong'], d['dot']]")" '["broken", "Link again", false]'
shot r1-appairage-a-refaire
U0=$(recu bt_unpair)
pilote click; sleep 1.2
expect "clic : le parcours commence par la réparation, rien n'est encore retiré" "$(field r02 "[d['view']['tour']['step'], d['view']['tour']['repair'], d['tourButtons'], d['focusLabel'], d['tourStepLabel']]"), $(( $(recu bt_unpair) - U0 ))" '["repair", "ask", ["Not Now", "Done on My iPhone"], "Done on My iPhone", "Step 1 of 7 — Start fresh"], 0'
shot r2-reparation
moteur delai oubli 2
pilote key Return; sleep 0.8
expect "Done on My iPhone : l'ordinateur oublie l'iPhone lui-même" "$(field r03 "d['view']['tour']['repair']"), $(( $(recu bt_unpair) - U0 ))" '"removing", 1'
shot r3-oubli-en-cours
moteur delai oubli 0.5
P0=$(recu 'bt_pair"')
sleep 7
expect "puis un vrai appairage, avec son code" "$(field r04 "[d['view']['tour']['step'], d['view']['tour']['code'], d['tourStepLabel']]"), $(( $(recu 'bt_pair"') - P0 )), $(recu 'bt_pair".*fresh')" '["code", "703405", "Step 3 of 7 — Check the number"], 1, 1'
pilote tpress match; sleep 4
expect "cette fois les deux liens montent : interrupteurs" "$(field r05 "[d['view']['tour']['step'], d['view']['tour']['links']]")" '["switches", {"classic": true, "le": true}]'
pilote tpress cancel; sleep 1
pilote click; sleep 0.8
expect "Finish Later : relié, et le menu propose de reprendre" "$(field r06 "[d['view']['link'], d['tourDialog'], 'setup' in d['controls']]")" '["linked", false, true]'
shot r4-menu-reprendre
pilote press setup; sleep 1.2
expect "Continue Setup… : le parcours reprend aux interrupteurs" "$(field r07 "[d['tourDialog'], d['menuOpen'], d['view']['tour']['step'], d['tourStepLabel']]")" '[true, false, "switches", "Step 1 of 3 — Allow sharing"]'
pilote tpress cancel; sleep 0.8

# Moteur d'avant le contrat d'appairage : même constat sans son aide, en cours de recherche
moteur contrat-ancien; moteur sans-iphone; sleep 1.2
moteur delai balayage 4
pilote click; sleep 1.2
expect "ancien moteur : parcours ouvert sur la recherche" "$(field r08 "d['view']['tour']['step']")" '"find"'
moteur appairage-partiel; sleep 2.2
expect "l'iPhone trouvé est déjà appairé ici : pas de bt_pair qui sauterait le code, la réparation" "$(field r09 "[d['view']['tour']['step'], d['view']['tour']['repair']]"), $(( $(recu 'bt_pair"') - P0 ))" '["repair", "ask"], 1'
shot r5-reparation-en-cours-de-route
pilote tpress cancel; sleep 0.8
moteur delai balayage 1
moteur contrat-nouveau

# Échecs d'authentification à répétition
moteur relie; sleep 1
moteur echecs-authentification; sleep 2.5
expect "échecs d'authentification : dit, pas « relié » ni attente sans fin" "$(field r10 "[d['view']['link'], d['strong']]")" '["broken", "Link again"]'
shot r6-echecs-authentification
pilote click; sleep 1.2
expect "clic : la réparation guidée" "$(field r11 "[d['tourDialog'], d['view']['tour']['step']]")" '[true, "repair"]'
pilote tpress cancel; sleep 0.8
moteur relie; sleep 1
fi

if want panne; then
# ============================================================================
# Messages et contacts non proposés (OBEX injoignable), oubli de l'iPhone,
# libellés de l'iPhone en français (captures o*, f*, l*)
# ============================================================================
NOTICE="This computer can’t offer messages and contacts right now."
relances() { grep -c -- "--user restart landline-engine.service" "$OUT/systemctl.log" 2>/dev/null; }
echo '{"experimental": true, "device_class": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur delai balayage 1; moteur delai liens 0.8; moteur delai oubli 0.5
moteur sans-iphone; moteur obex-injoignable; sleep 1.2
pilote click; sleep 1.2
wait_for "d['view']['tour']['step']" '"code"' 40
pilote tpress match; sleep 4.5
expect "appairé, OBEX injoignable : les interrupteurs, rien d'alarmant avant 20 s, l'aide repliée" "$(field o01 "[d['view']['tour']['step'], d['view']['tour']['obex'], d['tourButtons'], 'help' in d['tourControls'], d['focusLabel']]")" '["switches", null, ["Finish Later"], true, null]'
shot o1-interrupteurs-aide-repliee
pilote tpress help; sleep 0.8
expect "aide dépliée : ce que l'iPhone montre sans les interrupteurs, et l'oubli à portée" "$(field o02 "[any('shows only Device Type and Share System Notifications' in t for t in d['tourTexts']), 'If you only see this' in d['tourTexts'], 'forget' in d['tourControls']]")" '[true, true, true]'
shot o2-aide-depliee
pilote tpress help; sleep 0.6
wait_for "d['view']['tour']['obex']" '"down"' 80
sleep 0.5
expect "20 s de liens montés sans messages ni contacts : dit dans l'étape, Fix It par défaut" "$(field o03 "[d['view']['tour']['obex'], d['tourButtons'], d['focusLabel'], '$NOTICE' in d['tourTexts']]")" '["down", ["Finish Later", "Fix It"], "Fix It", true]'
shot o3-panne-detectee
moteur delai relance 4
pilote key Return; sleep 1.5
expect "Fix It : moteur relancé, le parcours reste là et le dit" "$(field o04 "[d['tourDialog'], d['view']['tour']['step'], d['view']['tour']['obex'], d['tourButtons']]"), $(relances)" '[true, "switches", "restarting", ["One moment…"]], 1'
shot o4-relance
wait_for "d['view']['tour']['obex']" '"fixed"' 40
sleep 0.5
expect "moteur revenu, liens remontés, services proposés : dit, et quoi faire sur l'iPhone" "$(field o05 "[d['view']['tour']['step'], d['engine'], d['view']['link'], 'Messages and contacts are on offer again' in d['tourTexts'], 'help' in d['tourControls']]"), $(grep -c -- "--user start iphone" "$OUT/systemctl.log" 2>/dev/null)" '["switches", "connected", "linked", true, true], 0'
shot o5-retabli
moteur interrupteur messages; sleep 1
expect "un interrupteur s'allume : retour au déroulé normal" "$(field o06 "[d['view']['tour']['obex'], d['view']['tour']['switches']['messages'], 'help' in d['tourControls']]")" '[null, true, true]'
expect "pendant l'étape, le moteur est prié de réessayer (bt_retry_profiles)" "$(( $(recu bt_retry_profiles) > 0 ))" 1

# Moteur récent : il nomme la panne (bus_closed), sort, et systemd le relance
moteur bus-ferme; sleep 0.8
expect "motif bus_closed : dit tout de suite, sans attendre 20 s" "$(field o13 "[d['view']['tour']['obex'], d['tourButtons']]")" '["down", ["Finish Later", "Fix It"]]'
shot o10-bus-ferme
wait_for "[d['engine'], d['view']['link'], d['view']['tour']['obex'] if d['view']['tour'] else 'perdu']" '["connected", "linked", null]' 40
expect "le moteur sort et revient de lui-même : le parcours est resté, à la même étape" "$(field o14 "[d['tourDialog'], d['view']['tour']['step'], d['view']['tour']['switches']['messages']]"), $(relances)" '[true, "switches", true], 1'

# Une panne que la relance ne répare pas : repartir à neuf
moteur delai relance 1.5
moteur obex-tenace
wait_for "d['view']['tour']['obex']" '"down"' 80
pilote tpress fix
wait_for "d['view']['tour']['obex']" '"still"' 100
sleep 0.5
expect "relance sans effet : repartir à neuf, pas de second Fix It" "$(field o07 "[d['view']['tour']['obex'], d['tourButtons'], d['focusLabel']]"), $(relances)" '["still", ["Finish Later", "Forget This iPhone…"], "Forget This iPhone…"], 2'
shot o6-relance-sans-effet
U0=$(recu bt_unpair)
pilote tpress forget; sleep 0.9
expect "Forget This iPhone… : une question d'abord, sans « Step n of N », Back pour revenir" "$(field f01 "[d['view']['tour']['step'], d['view']['tour']['forget'], d['tourButtons'], d['tourStepLabel'], d['focusLabel']]")" '["forget", {"state": "confirm", "back": true}, ["Back", "Forget"], "Forget this iPhone", null]'
shot f1-oubli-confirmation
pilote key Return; sleep 0.6
expect "Entrée n'oublie rien" "$(( $(recu bt_unpair) - U0 ))" 0
pilote tpress cancel; sleep 0.9
expect "Back : l'étape d'où l'on venait" "$(field f02 "[d['view']['tour']['step'], d['view']['tour']['obex']]")" '["switches", "still"]'
pilote tpress cancel; sleep 1
pilote click; sleep 0.8
expect "hors parcours : le menu dit la même chose et renvoie à Phone Settings pour l'oubli" "$(field o08 "[d['view']['link'], d['view']['card']['notices'], 'fix' in d['controls'], 'forget' in d['controls']]")" '["linked", ["Restarting did not bring messages and contacts back. Forget this iPhone in Phone Settings, then link it again."], false, false]'
shot o7-menu-relance-sans-effet
pilote key Escape; sleep 0.5

# La même panne vue au repos, réparée depuis le menu
moteur obex-retabli; sleep 1
moteur obex-injoignable
wait_for "d['view']['card'].get('fixAction')" 'true' 80
pilote click; sleep 0.8
expect "au repos : la panne dite dans le menu, avec Fix It" "$(field o09 "[d['view']['card']['notices'], 'fix' in d['controls'], 'setup' in d['controls']]")" '[["'"$NOTICE"'"], true, false]'
shot o8-menu-panne
pilote press fix; sleep 0.8
expect "Fix It du menu : menu fermé, relance demandée" "$(field o10 "d['menuOpen']"), $(relances)" 'false, 3'
wait_for "[d['view']['link'], d['view']['card'].get('fixAction')]" '["linked", false]' 40
pilote click; sleep 0.8
expect "réparé : plus rien à signaler que les interrupteurs restants" "$(field o11 "[d['engine'], d['view']['card']['notices'], 'fix' in d['controls'], 'setup' in d['controls']]")" '["connected", ["Some sharing switches are still off on the iPhone."], false, true]'
pilote key Escape; sleep 0.5
touch "$RT/refuse-relance"
moteur obex-injoignable
wait_for "d['view']['card'].get('fixAction')" 'true' 80
pilote click; sleep 0.8; pilote press fix; sleep 1.5; pilote click; sleep 0.8
expect "systemctl refuse : dit, Fix It encore offert" "$(field o12 "[d['view']['card']['notices'], 'fix' in d['controls']]")" '[["The phone link could not be restarted."], true]'
shot o9-menu-relance-refusee
pilote key Escape; sleep 0.5
rm -f "$RT/refuse-relance"

# Oublier l'iPhone : plus dans le menu, seulement depuis Phone Settings
moteur relie; sleep 1.2
pilote click; sleep 0.8
expect "relié : le menu ne propose plus Forget This iPhone…" "$(field f03 "['forget' in d['controls'], 'fix' in d['controls'], d['view']['card']['notices'], any('Forget' in t for t in d['menuTexts'])]")" '[false, false, [], false]'
shot f2-menu-sans-oubli
pilote key Escape; sleep 0.6
U0=$(recu bt_unpair)
moteur delai oubli 3
pilote forgetrequest; sleep 1.2
expect "demandé depuis Phone Settings : la question, dans la boîte du parcours ; Cancel et Forget, rien par défaut" "$(field f04 "[d['tourDialog'], d['menuOpen'], d['view']['tour']['forget'], d['tourButtons'], d['focusLabel'], d['tourWhere']]"), $(( $(recu bt_unpair) - U0 ))" '[true, false, {"state": "confirm", "back": false}, ["Cancel", "Forget"], null, "nothing yet. This part happens on this computer."], 0'
shot f3-oubli-confirmation-reglages
pilote tpress confirm; sleep 1
expect "Forget : bt_unpair parti, dit pendant l'attente" "$(field f05 "[d['view']['tour']['forget']['state'], d['tourButtons']]"), $(( $(recu bt_unpair) - U0 ))" '["removing", ["One moment…"]], 1'
shot f4-oubli-en-cours
wait_for "d['view']['tour']['forget']['state']" '"phone"' 30
sleep 0.6
expect "oublié ici : le dessin pour le faire sur l'iPhone, Link Again par défaut" "$(field f06 "[d['view']['link'], d['tourButtons'], d['focusLabel'], d['tourWhere']]")" '["setup", ["Close", "Link Again"], "Link Again", "Settings › Bluetooth › ⓘ next to “'"$(hostname)"'” › Forget This Device"]'
shot f5-oubli-cote-iphone
pilote tpress cancel; sleep 1
expect "Close : état clair, Link iPhone" "$(field f07 "[d['tourDialog'], d['strong'], d['view']['tour']]")" '[false, "Link iPhone", null]'
shot f6-apres-oubli
moteur delai oubli 0.5

# Depuis Phone Settings ; refus de BlueZ ; Link Again
moteur relie; sleep 1.2
moteur refuse-oubli
pilote forgetrequest; sleep 1.2
expect "demande venue de Phone Settings : la même question" "$(field f08 "[d['tourDialog'], d['view']['tour']['forget']]")" '[true, {"state": "confirm", "back": false}]'
pilote tpress confirm; sleep 1.5
expect "l'ordinateur n'a pas pu oublier : dit, rien n'est prétendu" "$(field f09 "[d['view']['tour']['forget']['state'], d['view']['link'], d['tourButtons']]")" '["error", "linked", ["Cancel", "Try Again"]]'
shot f7-oubli-refuse
moteur accepte-oubli; sleep 0.3
pilote tpress confirm
wait_for "d['view']['tour']['forget']['state']" '"phone"' 30
P0=$(recu 'bt_pair"')
pilote key Return; sleep 1.2
expect "Link Again : le parcours d'appairage, depuis la recherche" "$(field f10 "[d['view']['tour']['step'], d['tourStepLabel']]")" '["find", "Step 1 of 6 — Find your iPhone"]'
wait_for "d['view']['tour']['step']" '"code"' 40
expect "appairage demandé à neuf" "$(( $(recu 'bt_pair".*fresh') > 0 )), $(( $(recu 'bt_pair"') - P0 ))" '1, 1'
pilote tpress differ; sleep 1.6
expect "échec du parcours : l'oubli est aussi proposé là" "$(field f11 "[d['view']['tour']['step'], 'forget' in d['tourControls'], d['tourButtons']]")" '["failed", true, ["Cancel", "Try Again"]]'
shot f8-echec-avec-oubli

# iPhone en français : dessins et rappels entre parenthèses
pilote langue fr; sleep 0.6
expect "langue de l'iPhone : français" "$(field l00 "d['phoneLanguage']")" '"fr"'
moteur delai demande 4
pilote tpress retry; sleep 1.5
expect "recherche : le chemin avec le mot de l'iPhone entre parenthèses" "$(field l01 "[d['view']['tour']['step'], d['tourWhere']]")" '["find", "Settings (Réglages) › Bluetooth. Stay on that screen."]'
shot l1-recherche-fr
moteur delai demande 1.5
wait_for "d['view']['tour']['step']" '"code"' 40
sleep 0.5
expect "code : Pair (Jumeler)" "$(field l02 "any('tap Pair (Jumeler) on the iPhone' in t for t in d['tourTexts'])")" 'true'
shot l2-code-fr
pilote tpress match; sleep 4.5
expect "interrupteurs : libellés français rappelés" "$(field l03 "[d['view']['tour']['step'], 'Show Message Notifications (Afficher les notifications de message)' in d['tourTexts'], 'Sync Contacts (Synchroniser les contacts)' in d['tourTexts']]")" '["switches", true, true]'
shot l3-interrupteurs-fr
moteur interrupteur notifications; sleep 0.8
pilote tpress help; sleep 0.8
expect "aide : Type d'appareil nommé dans les deux langues" "$(field l04 "any('Device Type (Type d’appareil)' in t for t in d['tourTexts'])")" 'true'
shot l4-aide-fr
pilote tpress forget; sleep 0.9
pilote tpress confirm
wait_for "d['view']['tour']['forget']['state']" '"phone"' 30
sleep 0.6
expect "oubli : légende avec le libellé de l'iPhone" "$(field l05 "['Tap “Oublier cet appareil”' in d['tourTexts'], d['tourWhere']]")" '[true, "Settings (Réglages) › Bluetooth › ⓘ next to “'"$(hostname)"'” › Forget This Device (Oublier cet appareil)"]'
shot l5-oubli-fr
pilote tpress cancel; sleep 1
pilote langue auto; sleep 0.4
expect "langue automatique : celle du Shell d'essai, l'anglais" "$(field l06 "d['phoneLanguage']")" '"en"'
moteur relie; sleep 1
fi

if want cartes; then
# ============================================================================
# Cartes avec de vraies données (captures m*, c*)
# ============================================================================
echo '{"experimental": true, "device_class": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur relie; moteur historique-reel; sleep 1.2
N0=$(field m00 "d['neighbours']")
pilote click; sleep 0.8
MW=$(field m01 "d['menuWidth']")
expect "menu : code court avec pictogramme, aperçu sur une ligne" "$(field m01 "[[t['name'], t['initials'], t['preview'][:10]] for t in d['view']['card']['threads'] if t['name'] == '38200']")" '[["38200", "", "482913 est"]]'
shot m01-menu-vraies-donnees
pilote key Escape; sleep 0.6

moteur message-long; sleep 1.2
expect "long message d'un numéro : pastille bornée, pictogramme, numéro lisible" "$(field m02 "[d['strong'], d['pillWidth'] <= 300, d['view']['pill'].get('icon')]")" '["+41 76 555 01 42", true, "person"]'
expect "les icônes voisines n'ont pas bougé" "$(field m02 "d['neighbours']")" "$N0"
shot m02-pastille-longue
pilote click; sleep 1
expect "carte : même largeur que le menu, texte tronqué et dépliable" "$(field m03 "[d['menuWidth'] == $MW, d['bubbles'][0][0], d['focusInEntry']]")" '[true, true, true]'
H0=$(field m03 "d['bubbles'][0][1]")
shot m03-carte-longue
pilote expand 0; sleep 0.6
expect "clic sur le texte : déplié, largeur inchangée" "$(field m04 "[d['bubbles'][0][1] > $H0 + 30, d['menuWidth'] == $MW]")" '[true, true]'
shot m04-carte-longue-depliee
pilote expand 0; sleep 0.5
expect "second clic : replié" "$(field m05 "d['bubbles'][0][1]")" "$H0"
pilote key Escape; sleep 0.6

moteur message-numero; sleep 1.2
shot m05-pastille-numero
pilote click; sleep 1
expect "numéro sans nom : court, pas dépliable" "$(field m06 "[d['view']['card']['name'], d['view']['card']['initials'], d['bubbles'][0][0]]")" '["+41 76 555 01 42", "", false]'
shot m06-carte-numero
pilote key Escape; sleep 0.6

moteur message-code; sleep 1.2
pilote click; sleep 1
expect "code de vérification d'un code court : pas de réponses toutes faites" "$(field m07 "[d['view']['card']['name'], len(d['view']['card']['quick'])]")" '["38200", 0]'
shot m07-carte-code
pilote key Escape; sleep 0.6

moteur message-email; sleep 1.2
shot m08-pastille-adresse
pilote click; sleep 1
expect "adresse iMessage : initiales tirées de l'adresse" "$(field m08 "[d['view']['card']['name'], d['view']['card']['initials'], d['menuWidth'] == $MW]")" '["lea.rochat@exemple.ch", "LR", true]'
shot m09-carte-adresse
pilote key Escape; sleep 0.6

moteur message-suite; sleep 1.6
pilote click; sleep 1
expect "trois messages à la suite : tous dans la carte" "$(field m09 "len(d['view']['card']['messages'])")" 3
shot m10-carte-suite
moteur message Encore un; sleep 1
expect "un de plus, carte ouverte : il s'y ajoute" "$(field m10 "d['view']['card']['messages'][-1]['body']")" '"Encore un"'
shot m11-carte-suite-plus-un
pilote key Escape; sleep 0.6

moteur message-emoji; sleep 1.2
pilote click; sleep 1
shot m12-carte-emoji
pilote key Escape; sleep 0.6

moteur message-vide; sleep 1.2
expect "texte vide : la pastille dit pièce jointe" "$(field m11 "d['text']")" '"Attachment"'
shot m13-pastille-piece-jointe
pilote click; sleep 1
shot m14-carte-piece-jointe
pilote key Escape; sleep 0.6

moteur appel-inconnu; sleep 1.5
expect "appel d'un numéro sans nom : numéro lisible" "$(field c01 "[d['strong'], d['view']['card']['initials'], d['menuWidth'] == $MW or d['passiveOpen']]")" '["+41 22 555 01 99", "", true]'
shot c1-appel-inconnu
pilote press answer; sleep 1.5
pilote click; sleep 0.8
shot c2-carte-appel-inconnu
pilote press hangUp; sleep 1.2
moteur relie; sleep 1
fi

if want stable; then
# ============================================================================
# Indicateur stable : rien ne clignote, rien ne s'étire (captures s*)
# ============================================================================
echo '{"experimental": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur relie; sleep 1.2
pilote steady on; sleep 0.6
W0=$(field st00 "d['pillWidth']")
expect "relié : glyphe et point, tout de suite (liens montés, services ouverts)" "$(field st01 "[d['view']['link'], d['dot'], d['strong']]")" '["linked", true, ""]'
shot s1-stable-relie
moteur hors-portee; sleep 1
expect "liens tombés depuis 1 s : l'indicateur n'a pas bougé" "$(field st02 "[d['view']['link'], d['pillWidth'] == $W0]")" '["linked", true]'
sleep 3
expect "tombés depuis plus de 3 s : hors de portée, même largeur" "$(field st03 "[d['view']['link'], d['dot'], d['strong'], d['pillWidth'] == $W0]")" '["away", true, "", true]'
shot s2-stable-hors-portee
moteur relie; sleep 0.7
expect "retour confirmé (liens et services) : relié sans attendre" "$(field st04 "d['view']['link']")" '"linked"'
moteur message; sleep 8.5
expect "un non-lu : compteur à la place du point, même largeur" "$(field st05 "[d['count'], d['pillWidth'] == $W0]")" '["1", true]'
shot s3-stable-non-lu
pilote click; sleep 0.8; pilote press rows 0; sleep 0.6; pilote key Escape; sleep 0.8

# Le défaut vu en réel : appairage cassé d'un côté, les liens montent et retombent.
moteur liens-battants 8 1.5
seen=""
for i in $(seq 9); do
    seen="$seen $(field st06 "[d['strong'], d['dot'], d['pillWidth'] == $W0]")"
    [ "$i" = 5 ] && shot s4-stable-battement
    sleep 0.35
done
expect "liens qui battent toutes les 1,5 s : ni clignotement ni étirement" "$(printf '%s\n' $seen | sort -u | tr -d '\n')" '["",true,true]'
sleep 6.5
expect "trois chutes en 30 s : l'indicateur se fixe sur Link again" "$(field st07 "[d['view']['link'], d['strong'], d['view']['unsteady']]")" '["broken", "Link again", "flapping"]'
shot s5-stable-link-again
moteur liens-battants 4 1
sleep 4.5
expect "les liens battent encore : toujours Link again" "$(field st08 "d['strong']")" '"Link again"'
pilote click; sleep 1.2
expect "clic : la réparation guidée, même si les liens sont montés à cet instant" "$(field st09 "[d['tourDialog'], d['view']['tour']['step']]")" '[true, "repair"]'
shot s6-stable-reparation
pilote tpress cancel; sleep 0.8
moteur liens-stables; sleep 4
expect "liens stables et services ouverts : relié" "$(field st10 "[d['view']['link'], d['strong'], d['pillWidth'] == $W0]")" '["linked", "", true]'
pilote steady off
moteur relie; sleep 1
fi

if want silence; then
# ============================================================================
# Ne pas déranger, écran partagé, sourdine : rien ne surgit (captures q*)
# ============================================================================
echo '{"experimental": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur relie; sleep 1.2
pilote click; sleep 0.8; pilote key Escape; sleep 0.6
W0=$(field q00 "d['pillWidth']")
REST='["", "", false, null]'
bar() { field "$1" "[d['strong'], d['text'], d['passiveOpen'], d['banner']]"; }

pilote dnd on; sleep 0.6
moteur message-de leo Le code du coffre est 4471; sleep 1.5
expect "Ne pas déranger, message : ni pastille, ni nom, ni texte, ni carte" "$(bar q01)" "$REST"
expect "… un simple compteur, même largeur qu'au repos" "$(field q01 "[d['count'], d['pillWidth'] == $W0, d['view']['quiet']]")" '["1", true, "dnd"]'
shot q1-silence-message
moteur appel; sleep 1.5
expect "Ne pas déranger, appel entrant : la barre ne bouge pas, aucune carte ne descend" "$(bar q02)" "$REST"
shot q2-silence-appel
moteur notification Rendez-vous; sleep 1.2
expect "Ne pas déranger, autre appli : listée, sans bulle" "$(field q03 "[d['banner'], d['notifications'] > 0]")" '[null, true]'
pilote click; sleep 0.9
expect "menu ouvert exprès : l'appel y est, avec Decline et Answer" "$(field q04 "[d['menuOpen'], 'decline' in d['controls']]")" '[true, true]'
shot q3-silence-menu-voulu
pilote press decline; sleep 1.2
moteur raccroche; sleep 0.8
pilote key Escape; sleep 0.6
pilote dnd off; sleep 1.2
expect "sortie du Ne pas déranger : rien n'est rejoué, le compteur suffit" "$(bar q05), $(field q05 "d['count']")" "$REST, \"1\""

pilote sharing on; sleep 0.5
moteur message Tu rentres à quelle heure ?; sleep 1.5
expect "écran partagé, message : rien dans la barre" "$(bar q06), $(field q06 "[d['count'], d['view']['quiet']]")" "$REST, [\"2\", \"sharing\"]"
moteur appel; sleep 1.5
expect "écran partagé, appel : rien non plus" "$(bar q07)" "$REST"
shot q4-silence-partage
moteur raccroche; sleep 0.8
pilote click; sleep 0.9
expect "le menu dit pourquoi, et propose la sourdine" "$(field q08 "[d['view']['card']['notices'][0], 'mute' in d['controls']]")" '["Your screen is shared: nothing pops up here.", true]'
shot q5-silence-menu
pilote key Escape; sleep 0.6
pilote sharing auto; sleep 0.5

pilote click; sleep 0.9; pilote press mute; sleep 0.9
expect "Mute iPhone on this computer : réglage posé depuis le menu" "$(field q09 "[d['view']['quiet'], d['view']['card']['muted']]")" '["muted", true]'
shot q6-silence-sourdine
pilote key Escape; sleep 0.6
moteur message Encore un; sleep 1.5
expect "sourdine : rien dans la barre" "$(bar q10)" "$REST"
pilote reglage muted off; sleep 0.6

pilote reglage calls-through-do-not-disturb on; pilote dnd on; sleep 0.6
moteur appel; sleep 1.5
expect "appels autorisés en Ne pas déranger : l'appel se montre" "$(field q11 "[d['strong'], d['passiveOpen']]")" '["Camille is calling", true]'
shot q7-silence-appels-autorises
moteur raccroche; sleep 0.8
pilote reglage calls-through-do-not-disturb off; pilote dnd off; sleep 0.6
for row in 0 1; do pilote click; sleep 0.8; pilote press rows $row; sleep 0.6; pilote key Escape; sleep 0.8; done
expect "tout relu : retour au point" "$(field q12 "[d['count'], d['dot']]")" '["", true]'
moteur relie; sleep 1
fi

if want quotidien; then
# ============================================================================
# Le quotidien : son, présence, « Reply in app », connexion de l'iPhone
# (captures d*, h*)
# ============================================================================
echo '{"experimental": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur relie; sleep 1.2

# --- son de l'iPhone
pilote click; sleep 0.9
expect "menu : le réglage du son, éteint ; pas d'entrée de connexion sans profil réseau" "$(field d01 "[d['view']['card']['phoneAudioHere'], 'phoneAudio' in d['controls'], 'hotspot' in d['controls']]")" '[false, true, false]'
pilote press phoneAudio; sleep 1
expect "Play iPhone audio on this computer : demandé au moteur, montré allumé" "$(field d02 "d['view']['card']['phoneAudioHere']"), $(recu 'bt_set_phone_audio.*here')" 'true, 1'
shot d2-menu-son-ici
pilote press phoneAudio; sleep 1
expect "éteint de nouveau : le son reste sur l'iPhone" "$(field d03 "d['view']['card']['phoneAudioHere']"), $(recu 'bt_set_phone_audio.*phone')" 'false, 1'
pilote key Escape; sleep 0.6

# --- un moteur plus ancien : rien de nouveau n'est proposé
moteur quotidien-ancien; moteur relance; sleep 4
pilote click; sleep 0.9
expect "moteur d'avant : pas de réglage du son, le reste du menu est là" "$(field d04 "[d['engine'], 'phoneAudio' in d['controls'], 'mute' in d['controls']]")" '["connected", false, true]'
pilote key Escape; sleep 0.6
moteur appel; sleep 1.5
expect "moteur d'avant : un seul Answer" "$(field d05 "d['controls']")" '["decline", "answer"]'
moteur raccroche; sleep 0.8
moteur quotidien-nouveau; moteur relance; sleep 4

# --- présence : pourquoi l'iPhone n'est pas là
moteur eloigne switched_off; sleep 1.2
pilote click; sleep 0.9
expect "Bluetooth coupé sur l'iPhone : deux mots à côté du nom, la phrase dessous" "$(field d06 "[d['view']['card']['state'], d['view']['card']['notices'], d['header']['battery']]")" '["Bluetooth off", ["Bluetooth is off on the iPhone."], null]'
expect "...nom au milieu de la ligne, état calé à droite" "$(field d06 "[abs(2 * d['header']['name']['x'] + d['header']['name']['width'] - 2 * d['header']['row']['x'] - d['header']['row']['width']) <= 2, d['header']['state']['x'] + d['header']['state']['width'] == d['header']['row']['x'] + d['header']['row']['width']]")" '[true, true]'
shot d3-bluetooth-coupe-sur-iphone
pilote key Escape; sleep 0.6
moteur relie; sleep 1
moteur eloigne out_of_range locked; sleep 1.2
pilote click; sleep 0.9
expect "parti, session verrouillée par le moteur : dit sous le nom" "$(field d07 "[d['view']['card']['state'], d['view']['card']['notices']]")" '["Out of range", ["This computer was locked when the iPhone left."]]'
shot d4-parti-verrouille
pilote key Escape; sleep 0.6
moteur relie; sleep 1.2

# --- autres applis : « Reply in app » quand l'expéditeur est sûr
moteur notification-appli whatsapp; sleep 1.5
expect "WhatsApp d'un contact au numéro unique : lien vers la conversation" "$(field d08 "list(d['replyLinks'].values())")" '["https://web.whatsapp.com/send?phone=41790000001"]'
pilote hoverbanner; sleep 1.2
expect "bulle dépliée : le bouton Reply in app" "$(field d08b "'Reply in app' in d['bannerTexts']")" 'true'
shot d5-reply-in-app-bulle
pilote click; sleep 0.6; pilote key Escape; sleep 4.5
moteur notification-appli groupe; moteur notification-appli whatsapp Inconnue; sleep 1.5
expect "groupe, expéditeur absent des contacts : rien de proposé" "$(field d09 "[d['notifications'], len(d['replyLinks'])]")" '[3, 1]'
moteur notification-appli telegram; sleep 1.2
expect "Telegram sans client installé ici : rien de proposé non plus" "$(field d10 "len(d['replyLinks'])")" '1'
sleep 4; pilote clearall; sleep 0.8

# --- connexion Internet de l'iPhone (doublure de NetworkManager)
pilote hotspot profil; sleep 0.6
pilote click; sleep 0.9
expect "un profil réseau existe pour l'iPhone : l'entrée paraît, éteinte" "$(field h01 "[d['view']['card']['hotspot'], 'hotspot' in d['controls']]")" '[{"state": "off", "failed": false}, true]'
shot h1-connexion-eteinte
pilote press hotspot; sleep 0.9
expect "bascule : connexion en cours" "$(field h02 "d['view']['card']['hotspot']['state']")" '"connecting"'
shot h2-connexion-en-cours
sleep 2.6
expect "puis connecté" "$(field h03 "d['view']['card']['hotspot']")" '{"state": "on", "failed": false}'
shot h3-connexion-etablie
pilote press hotspot; sleep 0.9
expect "bascule : déconnecté" "$(field h04 "d['view']['card']['hotspot']['state']")" '"off"'
pilote hotspot refuse
pilote press hotspot; sleep 3.6
expect "la connexion retombe : dit, avec quoi faire sur l'iPhone" "$(field h05 "[d['view']['card']['hotspot'], 'Turn on Personal Hotspot on your iPhone, then try again.' in [t for t in [d.get('hotspotLine')] if t]]")" '[{"state": "off", "failed": true}, true]'
shot h4-connexion-refusee
pilote langue fr; sleep 0.6
pilote key Escape; sleep 0.5; pilote click; sleep 0.9
expect "iPhone en français : le libellé de l'iPhone est rappelé" "$(field h06 "d.get('hotspotLine')")" '"Turn on Personal Hotspot (Partage de connexion) on your iPhone, then try again."'
shot h5-connexion-refusee-fr
pilote langue auto
pilote key Escape; sleep 0.6
pilote hotspot sans-profil
moteur relie; sleep 1
fi

if want courant; then
echo '{"experimental": true, "device_class": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.6
moteur relie; sleep 1
ANS0=$(recu 'bt_call_action.*answer'); HUP0=$(recu 'bt_call_action.*hangup')
# --- les notifications déjà présentes sur l'iPhone ne reviennent pas à chaque reconnexion
moteur rafale 31; sleep 2
expect "31 notifications préexistantes : ni bulle, ni entrée dans la liste" "$(field n01 "[d['banner'], d['notifications'], d['trayTitles']]")" '[null, 0, []]'
pilote tray; sleep 1
shot n1-rafale-liste-vide
pilote closetray; sleep 0.6
moteur notification Dentiste; sleep 1.5
expect "puis une nouvelle, reçue en direct : une seule notification" "$(field n02 "[d['notifications'], d['trayTitles']]")" '[1, ["Agenda: Dentiste"]]'
sleep 4.5
moteur reconnexion; sleep 2.5
expect "reconnexion, toute la pile renvoyée : toujours une seule" "$(field n03 "[d['notifications'], d['trayTitles']]")" '[1, ["Agenda: Dentiste"]]'
moteur renvoi-bruyant; sleep 2
expect "pile renvoyée sans « silent », sous d'autres numéros : rien de plus" "$(field n04 "[d['notifications'], d['trayTitles']]")" '[1, ["Agenda: Dentiste"]]'
pilote tray; sleep 1
shot n5-apres-reconnexion
pilote closetray; sleep 0.6
pilote clearall; sleep 1
moteur relie; sleep 1

# --- repos, relié
expect "indicateur dans la boîte de droite, en premier" "$(field s01 "[d['enabled'], d['indexInRightBox']]")" "[true, 0]"
expect "relié : glyphe et point" "$(field s01 "[d['view']['link'], d['dot'], d['strong']]")" '["linked", true, ""]'
expect "moteur connecté" "$(field s01 "d['engine']")" '"connected"'
expect "tient dans la barre" "$(field s01 "d['height'] <= d['panelHeight']")" true
shot 01-repos-relie

# --- menu au repos, vrai clic
pilote click; sleep 0.8
expect "clic : menu ouvert" "$(field s02 "[d['menuOpen'], d['view']['card']['kind']]")" '[true, "menu"]'
shot 02-menu
# La première ligne : nom au milieu du menu, état et batterie calés à droite,
# les trois textes sur la même ligne de base.
CENTRE="abs(2 * d['header']['name']['x'] + d['header']['name']['width'] - 2 * d['header']['row']['x'] - d['header']['row']['width']) <= 2"
DROITE="d['header']['battery']['x'] + d['header']['battery']['width'] == d['header']['row']['x'] + d['header']['row']['width']"
expect "en-tête : nom de l'iPhone, entier, au milieu de la ligne" "$(field e01 "[d['header']['nameText'], d['header']['nameCut'], $CENTRE]")" '["iPhone de Camille", false, true]'
expect "en-tête : Linked puis la batterie, calés à droite, sans se toucher" "$(field e01 "[d['view']['card']['state'], $DROITE, d['header']['state']['x'] + d['header']['state']['width'] < d['header']['battery']['x'], d['header']['name']['x'] + d['header']['name']['width'] < d['header']['state']['x']]")" '["Linked", true, true, true]'
expect "en-tête : une seule ligne de base ; le glyphe seul à gauche" "$(field e01 "[len(set(d['header']['baselines'])), d['header']['glyph']['x'] == d['header']['row']['x']]")" '[1, true]'
moteur nom ☀️; sleep 0.8
expect "nom d'un seul emoji : au milieu, entier" "$(field e02 "[d['header']['nameText'], d['header']['nameCut'], $CENTRE]")" '["☀️", false, true]'
shot e1-menu-nom-emoji
moteur nom iPhone 17 Pro Max de Jean-Christophe ☀️; sleep 0.8
expect "nom trop long : coupé par des points de suspension, toujours au milieu, sans toucher l'état" "$(field e03 "[d['header']['nameCut'], $CENTRE, d['header']['name']['x'] + d['header']['name']['width'] < d['header']['state']['x'], d['header']['name']['x'] > d['header']['glyph']['x'] + d['header']['glyph']['width'], d['menuWidth'] == $(field e01 "d['menuWidth']")]")" '[true, true, true, true, true]'
shot e2-menu-nom-long
moteur nom; sleep 0.8
pilote press calls; sleep 0.6
shot 03-menu-appels-recents
pilote press rows 0; sleep 0.8
expect "conversation choisie : carte de réponse, menu resté ouvert" "$(field s03 "[d['menuOpen'], d['view']['card']['kind'], d['focusInEntry']]")" '[true, "reply", true]'
shot 04-carte-depuis-menu
pilote key Escape; sleep 0.6
expect "Échap ferme la carte" "$(field s04 "[d['menuOpen'], d['view']['mode']]")" '[false, "idle"]'

# --- nouveau message : recherche d'un contact, puis la carte de saisie
pilote click; sleep 0.8
expect "le menu propose New Message…" "$(field n02 "'compose' in d['controls']")" true
pilote press compose; sleep 0.8
expect "New Message… : champ de recherche actif, menu resté ouvert" "$(field n03 "[d['menuOpen'], d['view']['card']['kind'], d['focusInEntry']]")" '[true, "compose", true]'
shot n2-nouveau-message
pilote type cam; sleep 0.9
expect "trois lettres : le contact trouvé" "$(field n04 "[[r['name'], r['detail']] for r in d['view']['card']['results']]")" '[["Camille Martin", "+41 79 000 00 01"]]'
shot n3-nouveau-message-resultats
pilote press results 0; sleep 0.9
expect "contact choisi : la carte de saisie, champ actif" "$(field n05 "[d['view']['card']['kind'], d['view']['card']['name'], d['focusInEntry']]")" '["reply", "Camille Martin", true]'
pilote type Bonjour Camille; sleep 0.3
shot n4-nouveau-message-saisie
pilote key Return; sleep 0.4
expect "Entrée envoie au contact choisi" "$(recu 'bt_send_message.*tel:+41790000001.*Bonjour Camille')" 1
sleep 3.2
pilote click; sleep 0.8; pilote press compose; sleep 0.8
pilote type zzz; sleep 0.9
expect "rien de tel : dit, sans rien proposer" "$(field n06 "d['view']['card']['results']")" '[]'
pilote key Escape; sleep 0.6

# --- hors de portée
moteur hors-portee; sleep 1
expect "hors de portée : point creux, glyphe atténué" "$(field s05 "[d['view']['link'], 'landline-dim' in d['classes']]")" '["away", true]'
shot 05-hors-de-portee
pilote click; sleep 0.8
expect "hors de portée : même première ligne, nom au milieu, Out of range calé à droite, pas de batterie" "$(field e04 "[d['header']['nameText'], abs(2 * d['header']['name']['x'] + d['header']['name']['width'] - 2 * d['header']['row']['x'] - d['header']['row']['width']) <= 2, d['view']['card']['state'], d['header']['state']['x'] + d['header']['state']['width'] == d['header']['row']['x'] + d['header']['row']['width'], d['header']['battery'], len(set(d['header']['baselines'][:2]))]")" '["iPhone de Camille", true, "Out of range", true, null, 1]'
shot 06-menu-hors-de-portee
pilote key Escape; sleep 0.5
moteur relie; sleep 1

# --- message : pastille, ignorée, compteur
W0=$(field w0 "d['width']")
moteur message; sleep 0.1
shot 07-pastille-en-cours; sleep 0.8
expect "message : pastille ambre" "$(field s06 "[d['view']['mode'], d['strong'], d['text'], 'landline-accent' in d['classes']]")" '["message", "Camille", "On se retrouve à 19h ?", true]'
expect "la pastille a grossi" "$(field s06 "d['width'] > $W0 + 100")" true
shot 08-pastille-message
sleep 6.5
expect "ignorée 7 s : rétractée, compteur à 1" "$(field s07 "[d['view']['mode'], d['count'], d['width'] == $W0 or d['width'] < $W0 + 20]")" '["idle", "1", true]'
shot 09-non-lus

# --- message : carte, réponse au clavier
moteur message Tu viens ce soir ?; sleep 1.2
pilote click; sleep 0.8
expect "clic sur la pastille : carte de réponse, champ actif" "$(field s08 "[d['view']['card']['kind'], d['focusInEntry'], len(d['controls'])]")" '["reply", true, 5]'
shot 10-carte-reponse
sleep 7
expect "carte ouverte : la pastille ne se rétracte pas" "$(field s09 "[d['menuOpen'], d['view']['mode']]")" '[true, "message"]'
pilote type Oui, parfait; sleep 0.3
shot 11-carte-reponse-texte
pilote key Return; sleep 0.25
expect "Entrée envoie" "$(recu 'bt_send_message.*Oui, parfait')" 1
sleep 0.7
expect "envoyé : pastille Sent, carte fermée" "$(field s10 "[d['view']['mode'], d['strong'], d['menuOpen']]")" '["sent", "Sent", false]'
shot 12-envoye
sleep 2.5
expect "puis retour au point, compteur remis à zéro" "$(field s11 "[d['view']['mode'], d['dot'], d['count']]")" '["idle", true, ""]'

# --- message : réponse toute faite, bouton envoyer
moteur message-leo Tu as les clés ?; sleep 1.2
pilote click; sleep 0.8
pilote press chips 1; sleep 0.4
expect "réponse toute faite envoyée" "$(recu 'bt_send_message.*tel:+41790000002.*On my way')" 1
sleep 3.5
moteur message Encore une question; sleep 1.2
pilote click; sleep 0.8
pilote type Je regarde; sleep 0.2
pilote press send; sleep 0.4
expect "bouton envoyer" "$(recu 'bt_send_message.*Je regarde')" 1
sleep 3.5

# --- envoi refusé
moteur refuse-envoi
moteur message Tu es là ?; sleep 1.2
pilote click; sleep 0.8
pilote type Oui; pilote key Return; sleep 1.2
expect "envoi refusé : dit sur la pastille" "$(field s12 "d['strong']")" '"Not sent"'
shot 13-envoi-refuse
pilote click; sleep 0.8
expect "envoi refusé : la carte revient avec le texte" "$(field s13 "[d['entryText'], d['view']['card']['isError']]")" '["Oui", true]'
shot 14-carte-envoi-refuse
pilote press close; sleep 0.6
expect "bouton fermer" "$(field s14 "d['menuOpen']")" false

# --- appel entrant : carte sans prise du clavier, vrai clic sur Answer
moteur appel; sleep 1.5
expect "appel : pastille, carte montrée sans rendre le menu modal, deux façons de répondre" "$(field s15 "[d['strong'], d['passiveOpen'], d['menuOpen'], d['controls']]")" '["Camille is calling", true, false, ["decline", "answer", "answerHere", "answerPhone"]]'
shot 15-appel-entrant
pilote press answerPhone; sleep 1.2
expect "Answer on iPhone : commande partie, avec le son sur l'iPhone" "$(( $(recu 'bt_call_action.*answer') - ANS0 )), $(recu 'bt_call_action.*answer.*audio.*phone')" "1, 1"
expect "en appel : pastille verte, carte fermée" "$(field s16 "[d['view']['mode'], 'landline-call' in d['classes'], d['passiveOpen'], d['timer']]")" '["call", true, false, true]'
sleep 2
shot 16-en-appel
expect "le chrono tourne" "$(field s17 "d['strong'] >= 'Camille · 00:02'")" true
pilote click; sleep 0.8
expect "clic : carte d'appel, avec un mot sur l'appel silencieux" "$(field s18 "[d['view']['card']['kind'], d['view']['card']['audio'], 'Tap Audio' in d['view']['card']['hint']]")" '["call", "phone", true]'
shot 17-carte-appel
pilote press toComputer; sleep 1
expect "son sur l'ordinateur, pour cet appel seulement" "$(field s19 "[d['view']['card']['audio'], d['menuOpen'], d['view']['card']['hint']]"), $(recu 'bt_set_call_audio.*here.*persist.*false')" '["computer", true, ""], 1'
shot 18-carte-appel-son-ici
# Rendre le son à l'iPhone en cours d'appel : jugé sur ce que dit le moteur.
moteur son-coince
pilote press toPhone; sleep 1
expect "iPhone demandé, rien n'a encore bougé : montré en cours, pas comme fait" "$(field s19a "[d['view']['card']['audio'], d['view']['card']['moving'], d['view']['card']['isError'], 'moving to iPhone' in d['view']['card']['subtitle']]"), $(recu 'bt_set_call_audio.*phone.*persist.*false')" '["phone", true, false, true], 1'
shot e3-carte-appel-bascule-en-cours
sleep 3
expect "3 s plus tard le son est toujours ici : dit, avec quoi faire, et l'état vrai" "$(field s19b "[d['view']['card']['audio'], d['view']['card']['moving'], d['view']['card']['isError'], d['view']['card']['subtitle'], 'tap Audio' in d['view']['card']['hint']]")" '["computer", false, true, "Couldn’t move the call to the iPhone", true]'
shot d1-son-reste-ici
moteur son-refuse
pilote press toPhone; sleep 1
expect "le moteur dit lui-même que le lien voix est resté : dit sans attendre" "$(field s19c "[d['view']['card']['audio'], d['view']['card']['moving'], d['view']['card']['subtitle']]")" '["computer", false, "Couldn’t move the call to the iPhone"]'
moteur son-libre
pilote press toPhone; sleep 1.2
expect "le son repart vers l'iPhone, l'appel continue" "$(field s19d "[d['view']['mode'], d['view']['card']['audio'], d['view']['card']['moving'], d['view']['card']['isError'], 'sound on iPhone' in d['view']['card']['subtitle']]")" '["call", "phone", false, false, true]'
shot e4-carte-appel-rendu-iphone
pilote press toComputer; sleep 1.2
expect "et revient ici, toujours en appel" "$(field s19e "[d['view']['mode'], d['view']['card']['audio'], d['view']['card']['isError']]")" '["call", "computer", false]'
pilote press hangUp; sleep 1.2
expect "Hang Up : appel terminé, carte fermée" "$(field s20 "[d['view']['mode'], d['menuOpen'], d['timer']]")" '["idle", false, false]'

# --- appel entrant refusé ; carte modale ouverte par clic
moteur appel; sleep 1.5
pilote click; sleep 0.8
expect "clic pendant la sonnerie : la carte passe dans le menu" "$(field s21 "[d['menuOpen'], d['passiveOpen'], d['view']['card']['kind']]")" '[true, false, "ring"]'
pilote press decline; sleep 1.2
expect "Decline : commande partie, retour au repos" "$(field s22 "[d['view']['mode'], d['menuOpen']]")" '["idle", false]'
expect "Decline : raccroché côté moteur" "$(( $(recu 'bt_call_action.*hangup') - HUP0 ))" 2

# --- batterie faible
moteur batterie-faible; sleep 1.2
expect "batterie faible : une notification discrète, rien dans la barre" "$(field s23 "[d['strong'], d['banner'], d['trayTitles']]")" '["", "iPhone battery at 12 %", ["iPhone: iPhone battery at 12 %"]]'
shot 19-batterie-faible
moteur batterie 11; sleep 0.8
expect "un point de moins : pas de seconde notification" "$(field s23b "d['trayTitles']")" '["iPhone: iPhone battery at 12 %"]'
sleep 5; pilote clearall; moteur relie; sleep 0.6

# --- notification d'une autre appli
moteur notification; sleep 1.5
expect "notification : source au nom de l'appli" "$(field s24 "d['trayTitles']")" '["Agenda: Dentiste"]'
shot 20-notification-bulle
sleep 5
pilote tray; sleep 1
shot 21-notification-liste
pilote dismiss Agenda; sleep 0.8
expect "effacée ici : effacement relayé" "$(recu 'bt_notification_action.*negative')" 1
pilote closetray; sleep 0.6
moteur notification Rappel; sleep 1
moteur retire-notification; sleep 0.8
expect "retirée sur l'iPhone : disparaît ici, sans relais" "$(field s25 "d['notifications']"), $(recu 'bt_notification_action')" "0, 1"
expect "les messages ne font pas de bulle" "$(field s26 "d['sources']")" '[]'

# --- appairage
moteur appairage; sleep 1.2
pilote click; sleep 0.8
expect "appairage : code affiché" "$(field s27 "[d['strong'], d['view']['card']['code']]")" '["Confirm link", "703405"]'
shot 22-appairage
pilote press match; sleep 1
expect "They Match : réponse partie" "$(recu 'bt_pair_confirm.*true.*essai-1')" 1
shot 23-appairage-fait
sleep 7.5

# --- états d'erreur
pilote nohelper
moteur permission; sleep 1.2
expect "sans assistant système, permission manquante : libellé" "$(field s28 "d['strong']")" '"Permission needed"'
shot 24-permission
pilote click; sleep 0.8
shot 25-carte-permission
pilote key Escape; sleep 0.5

# --- assistant présent mais pas installé chez root : aucun mot de passe demandé
# d'ici, la commande à lancer soi-même, et la suite toute seule une fois faite
rm -f "$FAUX_SETUP_STATE"
PK0=$(cat "$OUT/pkexec.log" 2>/dev/null | wc -l)
pilote helper user; sleep 0.8
pilote click; sleep 1.2
expect "sans copie de root : l'étape montre la commande, Copy Commands par défaut" "$(field a01 "[d['tourDialog'], d['view']['tour']['step'], d['view']['tour']['permissionManual'], d['tourButtons'], d['focusLabel'], any(t.startswith('sudo ') and t.endswith(' apply') for t in d['tourTexts']), any('in a terminal' in t for t in d['tourTexts'])]")" '[true, "permission", true, ["Not Now", "Copy Commands"], "Copy Commands", true, true]'
shot t17-autorisation-a-la-main
pilote tpress copy; sleep 0.8
expect "Copy Commands : copié, dit ; rien n'est lancé avec des droits" "$(field a02 "[d['tourDialog'], d['banner']]"), $(( $(cat "$OUT/pkexec.log" 2>/dev/null | wc -l) - PK0 ))" '[true, "Commands copied"], 0'
echo '{"experimental": true, "device_class": true, "ertm": true, "persistent": true}' > "$FAUX_SETUP_STATE"
sleep 3.5
expect "réglages faits à la main : vu sans clic, le parcours a quitté l'autorisation" "$(field a03 "[d['tourDialog'], d['view']['tour']['step'] != 'permission', d['view']['tour']['permission']]")" '[true, true, "done"]'
pilote tpress cancel; sleep 1
expect "parcours refermé" "$(field a04 "d['tourDialog']")" 'false'
moteur permission-ok
pilote helper; sleep 0.6
moteur a-relier; sleep 1.2
expect "à relier : libellé" "$(field s29 "d['strong']")" '"Link again"'
shot 26-a-relier
# Le temps de cliquer sur Cancel avant que l'iPhone soit trouvé (la boîte se redessine alors).
moteur delai balayage 5
pilote click; sleep 0.8
expect "à relier : le clic ouvre le parcours, à la recherche" "$(field s29b "[d['tourDialog'], d['view']['tour']['step']]")" '[true, "find"]'
pilote tpress cancel; sleep 0.8
moteur delai balayage 1
moteur sans-iphone; sleep 1.2
expect "aucun iPhone : libellé" "$(field s30 "d['strong']")" '"Link iPhone"'
shot 27-sans-iphone

# --- Bluetooth de l'ordinateur éteint
moteur bluetooth-eteint; sleep 1.2
expect "Bluetooth éteint : dit tel quel, pas une affaire de permission" "$(field b01 "[d['view']['link'], d['strong']]")" '["bluetooth", "Bluetooth is off"]'
pilote click; sleep 0.8
shot b1-bluetooth-eteint
pilote key Escape; sleep 0.5
moteur bluetooth-allume; moteur relie; sleep 1.2

# --- relié, mais des réglages qui ne tiendraient pas au redémarrage
echo '{"experimental": true, "ertm": true, "persistent": {"experimental": true, "ertm": false}}' > "$FAUX_SETUP_STATE"
pilote helper; sleep 0.8
pilote click; sleep 0.8
expect "réglages non persistants : relié, et le menu propose de finir" "$(field b02 "[d['view']['link'], 'permission' in d['controls']]")" '["linked", true]'
shot p4-permission-depuis-menu
pilote press permission; sleep 1.2
expect "l'entrée du menu ouvre le parcours, à l'autorisation, en deux étapes" "$(field b03 "[d['tourDialog'], d['menuOpen'], d['tourStepLabel']]")" '[true, false, "Step 1 of 2 — Permission"]'
expect "un seul réglage manque : un seul point, sans redémarrage du Bluetooth annoncé" "$(field b03 "[d['view']['tour']['permissionNeeds'], d['view']['tour']['restartsBluetooth'], any('One Bluetooth setting' in t for t in d['tourTexts']), any('hands-free' in t.lower() for t in d['tourTexts'])]")" '[["ertm"], false, true, false]'
shot p6-permission-un-point
A0=$(grep -c "landline-setup apply" "$OUT/pkexec.log")
pilote key Return; sleep 2.6
expect "Continue : réglages rendus persistants, et la fin directement" "$(field b04 "[d['view']['tour']['step'], d['tourStepLabel']]"), $(( $(grep -c "landline-setup apply" "$OUT/pkexec.log") - A0 ))" '["done", "Step 2 of 2 — Done"], 1'
shot p5-permission-puis-fin
pilote key Return; sleep 1
pilote click; sleep 0.8
expect "plus rien à proposer dans le menu" "$(field b05 "['permission' in d['controls'], d['tourDialog']]")" '[false, false]'
pilote key Escape; sleep 0.5
moteur sans-iphone; sleep 1

# --- moteur absent, puis de retour
STARTS0=$(grep -c -- "--user start landline-engine.service" "$OUT/systemctl.log" 2>/dev/null)
exec 3>&-; kill "$MOTEUR"; sleep 2
expect "moteur arrêté : démarrage tenté une fois, puis dit en clair" "$(field s31 "[d['engine'], d['strong']]"), $(( $(grep -c -- "--user start landline-engine.service" "$OUT/systemctl.log") - ${STARTS0:-0} ))" '["absent", "Engine not running"], 1'
pilote click; sleep 0.8
shot 28-moteur-absent
start_moteur; sleep 1.5
pilote press action; sleep 1.5
expect "Try Again : reconnecté" "$(field s32 "[d['engine'], d['view']['link']]")" '["connected", "linked"]'
pilote key Escape; sleep 0.5

# --- couleur d'accent du système
pilote accent on
moteur message; sleep 1.2
shot 29-accent-systeme
pilote click; sleep 0.8
shot 30-accent-systeme-carte
pilote key Escape; sleep 0.5
pilote accent off

# --- Phone Settings : la fenêtre des réglages
pilote click; sleep 0.8
pilote press settings; sleep 4
expect "Phone Settings : fenêtre ouverte, menu fermé" "$(field s35 "[d['menuOpen'], d['windows']]")" '[false, 1]'
shot 32-reglages
pilote scroll 40; sleep 1.5
shot 33-reglages-oubli

# --- extinction propre
moteur appel; sleep 1.2
gnome-extensions disable "$UUID"; sleep 1.5
expect "désactivée : plus rien dans la barre" "$(field s33 "[d['enabled'], d['inPanel']]")" '[false, false]'
shot 31-desactivee
gnome-extensions enable "$UUID"; sleep 2
expect "réactivée : état relu, appel en cours montré" "$(field s34 "[d['engine'], d['strong']]")" '["connected", "Camille is calling"]'
gnome-extensions disable "$UUID"; sleep 1

fi

[ -e "$OUT/error" ] && { echo "FAIL erreurs du pilote :"; cat "$OUT/error"; failed=1; }
echo "=== shell.log : erreurs JS, CRITICAL et lignes de l'extension"
# Un CRITICAL de Clutter, de St ou de GJS n'est pas une erreur JS, mais c'est
# un défaut de l'extension tout autant (allocation NaN, objet déjà libéré).
ERREURS="JS ERROR|TypeError|ReferenceError|Clutter-CRITICAL|Gjs-CRITICAL|St-CRITICAL|GLib-GObject-CRITICAL|GLib-CRITICAL|has been already disposed|Source ID .* was not found"
if grep -i -E "$ERREURS|landline|pilote|Extension .* error" "$OUT/shell.log" | grep -v "^$" | tail -30 | grep .; then
    if grep -q -i -E "$ERREURS" "$OUT/shell.log"; then
        echo "FAIL erreurs ou CRITICAL dans le journal du Shell : $(grep -c -i -E "$ERREURS" "$OUT/shell.log") ligne(s)"
        failed=1
    fi
else
    echo "(rien)"
fi
exit $failed
