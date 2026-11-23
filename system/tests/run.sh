#!/usr/bin/env bash
# Essais de system/landline-setup dans une racine factice : rien n'est écrit
# ni exécuté sur le vrai système. Des doublures de systemctl, btmgmt, udevadm
# et bluetoothctl imitent bluetoothd, sysfsutils et la version de BlueZ.
#
#   system/tests/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETUP="$HERE/../landline-setup"
POLICY_SRC="$HERE/../io.github.d101c.landline.policy"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
R=""
ok()  { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
ko()  { fail=$((fail + 1)); printf '  ÉCHEC %s\n' "$1"; [ -z "${2:-}" ] || printf '        %s\n' "$2"; }
check() { # check <libellé> <commande…>
    local label="$1"; shift
    if "$@" >/dev/null 2>&1; then ok "$label"; else ko "$label" "commande : $*"; fi
}
check_not() {
    local label="$1"; shift
    if "$@" >/dev/null 2>&1; then ko "$label" "commande : $*"; else ok "$label"; fi
}
check_eq() { # check_eq <libellé> <attendu> <obtenu>
    if [ "$2" = "$3" ]; then
        ok "$1"
    elif [[ "$2$3" == *$'\n'* ]]; then
        ko "$1" "différences (attendu < / obtenu >) :"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/        /' || true
    else
        ko "$1" "attendu : $2 / obtenu : $3"
    fi
}

# --- racine factice ---------------------------------------------------------

# new_root <nom> : une machine vierge — bluetoothd sans --experimental, classe
# par défaut, ERTM coupé par /etc/sysfs.conf (cas de la manette Xbox).
new_root() {
    R="$WORK/$1"
    mkdir -p "$R"/{.stubs,.state,etc/bluetooth,etc/systemd/system,etc/udev/rules.d} \
             "$R"/usr/lib/systemd/system "$R"/usr/libexec/bluetooth "$R"/usr/share/polkit-1/actions \
             "$R"/sys/module/bluetooth/parameters "$R"/sys/class/bluetooth/hci0 \
             "$R"/sys/class/bluetooth/hci0:256 "$R"/proc/4242 "$R"/usr/local
    cat > "$R/usr/lib/systemd/system/bluetooth.service" <<'UNIT'
[Unit]
Description=Bluetooth service

[Service]
Type=dbus
BusName=org.bluez
ExecStart=/usr/libexec/bluetooth/bluetoothd
UNIT
    : > "$R/usr/libexec/bluetooth/bluetoothd"
    chmod +x "$R/usr/libexec/bluetooth/bluetoothd"
    printf '[General]\n#Experimental = false\n' > "$R/etc/bluetooth/main.conf"
    printf '# exemple\n/module/bluetooth/parameters/disable_ertm=1\n' > "$R/etc/sysfs.conf"
    echo Y > "$R/sys/module/bluetooth/parameters/disable_ertm"
    printf '/usr/libexec/bluetooth/bluetoothd\0' > "$R/proc/4242/cmdline"
    echo 4242 > "$R/.state/pid"
    echo 7c010c > "$R/.state/class"
    echo yes > "$R/.state/powered"
    echo 5.87 > "$R/.state/bluez-version"

    # bluetoothctl : ne sert qu'à dire la version de BlueZ.
    cat > "$R/.stubs/bluetoothctl" <<'STUB'
#!/bin/bash
[ "$1" = --version ] || exit 1
v="$(cat "$LANDLINE_ROOT/.state/bluez-version")"
[ -n "$v" ] || exit 1
printf 'bluetoothctl: %s\n' "$v"
STUB

    # btmgmt : lit la classe (publiée pour information) ; toute écriture de
    # classe est notée, pour vérifier que plus personne n'en fait.
    cat > "$R/.stubs/btmgmt" <<'STUB'
#!/bin/bash
S="$LANDLINE_ROOT/.state"
cat >/dev/null   # comme le vrai : lit son entrée standard
case "$*" in
    *" info")
        printf 'hci0:\tPrimary controller\n\taddr 00:11:22:33:44:55 version 10 manufacturer 2 class 0x%s\n' "$(cat "$S/class")"
        printf '\tsupported settings: powered connectable\n'
        if [ "$(cat "$S/powered")" = yes ]; then printf '\tcurrent settings: powered connectable\n'
        else printf '\tcurrent settings: connectable\n'; fi
        ;;
    *" class 4 8")
        echo x >> "$S/class-writes"
        if [ "$(cat "$S/powered")" = yes ]; then echo 7c0408 > "$S/class"; fi
        ;;
esac
STUB

    # systemctl : imite systemd pour les seules demandes de l'assistant.
    cat > "$R/.stubs/systemctl" <<'STUB'
#!/bin/bash
R="$LANDLINE_ROOT"; S="$R/.state"
effective() { # dernière ligne ExecStart non vide, unité puis fichiers d'appoint
    cat "$R/usr/lib/systemd/system/bluetooth.service" \
        "$R"/etc/systemd/system/bluetooth.service.d/*.conf 2>/dev/null |
        sed -n 's/^ExecStart=-\{0,1\}\(..*\)$/\1/p' | tail -n 1
}
run_ertm() {
    [ -f "$R/etc/systemd/system/landline-ertm.service" ] || return 1
    echo N > "$R/sys/module/bluetooth/parameters/disable_ertm"
}
case "$1" in
    show) cat "$S/pid" ;;
    daemon-reload|stop) ;;
    restart|start)
        verb="$1"; shift
        for unit in "$@"; do
            case "$unit" in
                bluetooth.service)
                    effective | tr ' \n' '\0\0' > "$R/proc/4242/cmdline"
                    if [ "$(cat "$S/powered")" = yes ]; then
                        echo 7c010c > "$S/class"      # bluetoothd réécrit sa classe
                    fi ;;
                landline-ertm.service) run_ertm || exit 1 ;;
            esac
        done ;;
esac
STUB
    printf '#!/bin/bash\nexit 0\n' > "$R/.stubs/udevadm"

    # setcap et getcap : le droit posé sur un fichier, noté dans l'état.
    # « setcap-fails » fait échouer setcap.
    cat > "$R/.stubs/setcap" <<'STUB'
#!/bin/bash
S="$LANDLINE_ROOT/.state"
[ -e "$S/setcap-fails" ] && exit 1
[ "$1" = cap_net_raw+ep ] && [ -f "$LANDLINE_ROOT$2" ] || exit 1
echo yes > "$S/caps"
STUB
    cat > "$R/.stubs/getcap" <<'STUB'
#!/bin/bash
S="$LANDLINE_ROOT/.state"
[ -f "$1" ] && [ "$(cat "$S/caps" 2>/dev/null)" = yes ] && printf '%s cap_net_raw=ep\n' "$1"
exit 0
STUB
    chmod +x "$R"/.stubs/*
    : > "$R/.commands.log"
}

setup()  { LANDLINE_ROOT="$R" "$SETUP" "$@"; }
status() { setup status; }

# Empreinte de la racine, hors doublures, état simulé et pseudo-systèmes.
snapshot() {
    (cd "$R" && find . -path ./.stubs -prune -o -path ./.state -prune -o -path ./proc -prune \
        -o -path ./sys -prune -o -name .commands.log -prune \
        -o \( -type f -o -type l -o -type d \) -print | LC_ALL=C sort |
        while IFS= read -r f; do
            if [ -L "$f" ]; then printf '%s -> %s\n' "$f" "$(readlink "$f")"
            elif [ -f "$f" ]; then printf '%s %s\n' "$f" "$(sha256sum < "$f")"
            else printf '%s/\n' "$f"; fi
        done)
}

# Redémarrage de la machine : sysfsutils, nos unités si activées, bluetoothd.
reboot_machine() {
    echo Y > "$R/sys/module/bluetooth/parameters/disable_ertm"
    if [ -L "$R/etc/systemd/system/multi-user.target.wants/landline-ertm.service" ]; then
        LANDLINE_ROOT="$R" "$R/.stubs/systemctl" start landline-ertm.service
    fi
    LANDLINE_ROOT="$R" "$R/.stubs/systemctl" restart bluetooth.service
}
ertm_value() { cat "$R/sys/module/bluetooth/parameters/disable_ertm"; }

# La classe d'appareil reste à faux partout : elle n'est plus posée, et
# « ready » n'en dépend plus.
TAIL='"required":{"experimental":true,"ertm":true},"bluez_version":"5.87","le_params":false}'
ALL_FALSE='{"experimental":false,"device_class":false,"ertm":false,"ready":false,"persistent":{"experimental":false,"device_class":false,"ertm":false},'"$TAIL"
ALL_TRUE='{"experimental":true,"device_class":false,"ertm":true,"ready":true,"persistent":{"experimental":true,"device_class":false,"ertm":true},'"$TAIL"

# Ce qu'une version précédente posait pour la classe d'appareil.
legacy_class() {
    local marker='# Fichier géré par iphone-link (iphone-link-setup).'
    mkdir -p "$R/etc/systemd/system/bluetooth.service.wants" "$R/usr/local/libexec/iphone-link"
    printf '%s\n[Service]\nExecStart=/usr/local/libexec/iphone-link/iphone-link-btclass\n' "$marker" \
        > "$R/etc/systemd/system/iphone-link-btclass.service"
    ln -sfn /etc/systemd/system/iphone-link-btclass.service \
        "$R/etc/systemd/system/bluetooth.service.wants/iphone-link-btclass.service"
    printf '%s\nACTION=="add", SUBSYSTEM=="bluetooth"\n' "$marker" > "$R/etc/udev/rules.d/iphone-link-btclass.rules"
    printf '#!/bin/bash\n%s\nexit 0\n' "$marker" > "$R/usr/local/libexec/iphone-link/iphone-link-btclass"
    chmod +x "$R/usr/local/libexec/iphone-link/iphone-link-btclass"
}
LEGACY_FILES="etc/systemd/system/iphone-link-btclass.service
etc/systemd/system/bluetooth.service.wants/iphone-link-btclass.service
etc/udev/rules.d/iphone-link-btclass.rules
usr/local/libexec/iphone-link/iphone-link-btclass"

# --- 1. syntaxe et analyse statique ----------------------------------------

echo "== Syntaxe"
check "bash -n landline-setup" bash -n "$SETUP"
check "bash -n tests/run.sh" bash -n "${BASH_SOURCE[0]}"
if command -v shellcheck >/dev/null 2>&1; then
    check "shellcheck landline-setup" shellcheck "$SETUP"
    check "shellcheck tests/run.sh" shellcheck "${BASH_SOURCE[0]}"
else
    echo "  --   shellcheck absent : analyse statique NON faite"
fi

# --- 2. machine vierge ------------------------------------------------------

echo "== Machine vierge : status"
new_root vierge
out="$(status)"
check_eq "status : tout à faux" "$ALL_FALSE" "$out"
check_eq "status : une seule ligne" 1 "$(printf '%s\n' "$out" | wc -l)"
check "status : JSON valide" python3 -c 'import json,sys; json.loads(sys.argv[1])' "$out"
before="$(snapshot)"

echo "== Droits et usage"
if [ "$(id -u)" -ne 0 ]; then
    set +e
    msg="$("$SETUP" apply 2>&1)"; rc=$?
    set -e
    check_eq "apply sans root : code 1" 1 "$rc"
    check "apply sans root : message clair" grep -q 'droits administrateur' <<< "$msg"
    set +e
    "$SETUP" revert >/dev/null 2>&1; rc=$?
    set -e
    check_eq "revert sans root : code 1" 1 "$rc"
else
    echo "  --   lancé en root : contrôle des droits non fait"
fi
set +e
setup frobnicate >/dev/null 2>&1; rc=$?
set -e
check_eq "action inconnue : code 64" 64 "$rc"

echo "== Machine vierge : apply --dry-run"
dry="$(setup apply --dry-run)"
check_eq "dry-run : rien n'est écrit" "$before" "$(snapshot)"
check_eq "dry-run : aucune commande lancée" "" "$(cat "$R/.commands.log")"
check "dry-run : annonce le fichier d'appoint" grep -q 'écrirait : /etc/systemd/system/bluetooth.service.d/landline-experimental.conf' <<< "$dry"
check "dry-run : montre le contenu" grep -q '| ExecStart=/usr/libexec/bluetooth/bluetoothd --experimental' <<< "$dry"
check "dry-run : annonce le redémarrage" grep -q 'exécuterait : systemctl restart bluetooth.service' <<< "$dry"
check "dry-run : prévient de la coupure" grep -q 'REDÉMARRAGE DU BLUETOOTH' <<< "$dry"

echo "== Machine vierge : apply"
out="$(setup apply)"
check_eq "apply : dernière ligne = état, tout à vrai" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
check_eq "apply : status confirme" "$ALL_TRUE" "$(status)"
for f in etc/systemd/system/bluetooth.service.d/landline-experimental.conf \
         etc/systemd/system/landline-ertm.service usr/local/libexec/landline/landline-setup \
         usr/share/polkit-1/actions/io.github.d101c.landline.policy; do
    check "apply : $f porte l'en-tête" grep -q 'Managed by landline-setup' "$R/$f"
done
while IFS= read -r f; do
    check_not "apply : $f n'est plus installé" test -e "$R/$f"
done <<< "$LEGACY_FILES"
check_not "apply : aucune écriture de classe" test -s "$R/.state/class-writes"
check_not "apply : l'unité de classe n'est jamais lancée" grep -q 'iphone-link-btclass' "$R/.commands.log"
check_eq "apply : la classe d'origine est gardée" 7c010c "$(cat "$R/.state/class")"
check "apply : unité ERTM activée" test -L "$R/etc/systemd/system/multi-user.target.wants/landline-ertm.service"
check "apply : copie de l'assistant identique" cmp -s "$SETUP" "$R/usr/local/libexec/landline/landline-setup"
check "apply : /etc/sysfs.conf intact" grep -qx '/module/bluetooth/parameters/disable_ertm=1' "$R/etc/sysfs.conf"
check_eq "apply : ERTM réactivé" N "$(ertm_value)"
check_eq "apply : un seul redémarrage de bluetoothd" 1 "$(grep -c '^systemctl restart bluetooth.service$' "$R/.commands.log")"
check "apply : le redémarrage est annoncé" grep -q 'REDÉMARRAGE DU BLUETOOTH' <<< "$out"
check "apply : fiche polkit = fichier du dépôt" cmp -s "$POLICY_SRC" "$R/usr/share/polkit-1/actions/io.github.d101c.landline.policy"
check "fiche polkit : XML valide, message et chemin annoté" python3 -c '
import sys, xml.etree.ElementTree as ET
action = ET.parse(sys.argv[1]).getroot().find("action")
assert action.get("id") == "io.github.d101c.landline.setup"
assert action.find("message").text == "Change Bluetooth settings so this computer can link to an iPhone"
note = action.find("annotate")
assert note.get("key") == "org.freedesktop.policykit.exec.path"
assert note.text == "/usr/local/libexec/landline/landline-setup"
' "$POLICY_SRC"

echo "== Idempotence"
after_apply="$(snapshot)"
: > "$R/.commands.log"
out="$(setup apply)"
check_eq "2e apply : arborescence identique" "$after_apply" "$(snapshot)"
check_not "2e apply : rien d'écrit" grep -Eq '^(écrit|copié|lien créé) ' <<< "$out"
check_not "2e apply : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
check_not "2e apply : pas de daemon-reload" grep -q 'daemon-reload' "$R/.commands.log"
check_eq "2e apply : toujours prêt" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
LANDLINE_ROOT="$R" "$R/usr/local/libexec/landline/landline-setup" apply >/dev/null
check_eq "apply depuis la copie installée : arborescence identique" "$after_apply" "$(snapshot)"

echo "== Redémarrage de la machine"
reboot_machine
check_eq "après redémarrage : toujours prêt" "$ALL_TRUE" "$(status)"

echo "== revert --dry-run puis revert"
dry="$(setup revert --dry-run)"
check_eq "revert --dry-run : rien ne change" "$after_apply" "$(snapshot)"
check "revert --dry-run : annonce le retrait" grep -q 'retirerait : /etc/systemd/system/landline-ertm.service' <<< "$dry"
: > "$R/.commands.log"
out="$(setup revert)"
check_eq "revert : arborescence d'origine" "$before" "$(snapshot)"
check_eq "revert : ERTM remis à la valeur de sysfs.conf" 1 "$(ertm_value)"
check "revert : bluetoothd redémarré sans --experimental" grep -q '^systemctl restart bluetooth.service$' "$R/.commands.log"
check_eq "revert : status tout à faux" "$ALL_FALSE" "$(tail -n 1 <<< "$out")"
setup revert >/dev/null
check_eq "2e revert : arborescence d'origine" "$before" "$(snapshot)"
reboot_machine
check_eq "après revert et redémarrage : tout à faux" "$ALL_FALSE" "$(status)"

echo "== revert depuis la copie installée (elle se retire elle-même)"
setup apply >/dev/null
LANDLINE_ROOT="$R" "$R/usr/local/libexec/landline/landline-setup" revert >/dev/null
check_eq "arborescence d'origine" "$before" "$(snapshot)"

# --- 3. cette machine -------------------------------------------------------

echo "== Machine déjà réglée à la main (fichier d'appoint de l'utilisateur)"
new_root manuel
mkdir -p "$R/etc/systemd/system/bluetooth.service.d"
printf '[Service]\nExecStart=\nExecStart=/usr/libexec/bluetooth/bluetoothd --experimental\n' \
    > "$R/etc/systemd/system/bluetooth.service.d/experimental.conf"
printf '/usr/libexec/bluetooth/bluetoothd\0--experimental\0' > "$R/proc/4242/cmdline"
echo N > "$R/sys/module/bluetooth/parameters/disable_ertm"
mkdir -p "$R/usr/local/libexec"          # dossier parent déjà présent
before="$(snapshot)"
check_eq "status : prêt avec la classe d'origine, mais l'ERTM ne tiendra pas" \
    '{"experimental":true,"device_class":false,"ertm":true,"ready":true,"persistent":{"experimental":true,"device_class":false,"ertm":false},'"$TAIL" \
    "$(status)"
out="$(setup apply)"
check_eq "apply : tout à vrai" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
check_not "apply : pas de fichier d'appoint en double" test -e "$R/etc/systemd/system/bluetooth.service.d/landline-experimental.conf"
check_not "apply : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
check "apply : le dit" grep -q 'pas de redémarrage du Bluetooth' <<< "$out"
reboot_machine
check_eq "après redémarrage : prêt" "$ALL_TRUE" "$(status)"
: > "$R/.commands.log"
setup revert >/dev/null
check_eq "revert : arborescence d'origine (fichier de l'utilisateur et /usr/local/libexec gardés)" "$before" "$(snapshot)"
check_not "revert : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
check_eq "revert : ERTM remis à 1" 1 "$(ertm_value)"

# --- 4. cas particuliers ----------------------------------------------------

echo "== Cas particuliers"
new_root mainconf
printf '[General]\nExperimental = true\n' > "$R/etc/bluetooth/main.conf"
check "main.conf Experimental = true : persistant" grep -q '"persistent":{"experimental":true' <<< "$(status)"
setup apply >/dev/null
check_not "main.conf : pas de fichier d'appoint" test -e "$R/etc/systemd/system/bluetooth.service.d/landline-experimental.conf"

new_root options
sed -i 's|^ExecStart=.*|ExecStart=-/usr/libexec/bluetooth/bluetoothd --noplugin=sap|' "$R/usr/lib/systemd/system/bluetooth.service"
setup apply >/dev/null
check "options existantes de bluetoothd gardées, préfixe retiré" grep -qx 'ExecStart=/usr/libexec/bluetooth/bluetoothd --noplugin=sap --experimental' \
    "$R/etc/systemd/system/bluetooth.service.d/landline-experimental.conf"

new_root arch
sed -i 's|^ExecStart=.*|ExecStart=/usr/lib/bluetooth/bluetoothd|' "$R/usr/lib/systemd/system/bluetooth.service"
setup apply >/dev/null
check "chemin de bluetoothd lu dans l'unité (Arch)" grep -qx 'ExecStart=/usr/lib/bluetooth/bluetoothd --experimental' \
    "$R/etc/systemd/system/bluetooth.service.d/landline-experimental.conf"

new_root surcharge
mkdir -p "$R/etc/systemd/system/bluetooth.service.d"
printf '[Service]\nExecStart=\nExecStart=/usr/libexec/bluetooth/bluetoothd -d\n' \
    > "$R/etc/systemd/system/bluetooth.service.d/zz-debug.conf"
set +e
msg="$(setup apply 2>&1)"; rc=$?
set -e
check_eq "fichier d'appoint tiers qui écrase ExecStart : code 1" 1 "$rc"
check "… et message explicite" grep -q 'redéfinit ExecStart' <<< "$msg"

new_root etranger
setup apply >/dev/null
printf '[Unit]\nDescription=à moi\n' > "$R/etc/systemd/system/landline-ertm.service"
set +e
setup revert >/dev/null 2>&1; rc=$?
set -e
check "revert : fichier sans en-tête laissé en place" test -f "$R/etc/systemd/system/landline-ertm.service"
check_eq "revert : le signale (code 2)" 2 "$rc"

new_root sans-sysfs
rm "$R/etc/sysfs.conf"
echo N > "$R/sys/module/bluetooth/parameters/disable_ertm"
check "sans sysfs.conf : ERTM persistant d'office" grep -q '"ertm":true},"required"' <<< "$(status)"
mkdir -p "$R/etc/modprobe.d"
echo 'options bluetooth disable_ertm=Y' > "$R/etc/modprobe.d/99-xpadneo-bluetooth.conf"
check "modprobe.d disable_ertm=Y : ERTM non persistant" grep -q '"ertm":false},"required"' <<< "$(status)"
setup apply >/dev/null
setup revert >/dev/null
check_eq "revert : ERTM remis à la valeur de modprobe.d" Y "$(ertm_value)"

new_root eteint
echo no > "$R/.state/powered"
echo 000000 > "$R/.state/class"
set +e
out="$(setup apply)"; rc=$?
set -e
check_eq "Bluetooth éteint : apply réussit" 0 "$rc"
check "Bluetooth éteint : prêt quand même, la classe ne compte plus" \
    grep -q '"device_class":false,"ertm":true,"ready":true' <<< "$(tail -n 1 <<< "$out")"

# --- 5. classe d'appareil : plus exigée, restes d'une version précédente ----

echo "== Classe d'appareil : pour information seulement"
new_root classe-info
echo 7c0408 > "$R/.state/class"
check "classe mains-libres déjà là : publiée, sans effet sur ready" \
    grep -q '"device_class":true,"ertm":false,"ready":false' <<< "$(status)"
echo 7c0000 > "$R/.state/class"
setup apply >/dev/null
check "classe d'origine 0x7c0000 : prêt" grep -q '"device_class":false,"ertm":true,"ready":true' <<< "$(status)"

echo "== Restes d'une version précédente"
new_root ancien
before="$(snapshot)"
setup apply >/dev/null
after_apply="$(snapshot)"
legacy_class
check "status : l'ancienne unité est vue (information)" grep -q '"persistent":{"experimental":true,"device_class":true' <<< "$(status)"
dry="$(setup apply --dry-run)"
check "apply --dry-run : annonce le retrait de l'ancienne unité" grep -q 'retirerait : /etc/systemd/system/iphone-link-btclass.service' <<< "$dry"
: > "$R/.commands.log"
out="$(setup apply)"
while IFS= read -r f; do
    check_not "apply : retire $f" test -e "$R/$f"
done <<< "$LEGACY_FILES"
check_eq "apply : arborescence d'une installation neuve" "$after_apply" "$(snapshot)"
check "apply : arrête l'ancienne unité" grep -q '^systemctl stop iphone-link-btclass.service$' "$R/.commands.log"
check "apply : recharge systemd et udev" grep -q 'daemon-reload' "$R/.commands.log"
check "apply : recharge udev" grep -q '^udevadm control --reload$' "$R/.commands.log"
check_not "apply : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
legacy_class
setup revert >/dev/null
check_eq "revert : retire aussi les restes, arborescence d'origine" "$before" "$(snapshot)"
new_root ancien-seul
before="$(snapshot)"
legacy_class
rmdir "$R/usr/local/libexec/iphone-link" 2>/dev/null || true
setup revert >/dev/null
while IFS= read -r f; do
    check_not "revert sans apply : retire $f" test -e "$R/$f"
done <<< "$LEGACY_FILES"
new_root ancien-etranger
legacy_class
printf '[Unit]\nDescription=à moi\n' > "$R/etc/systemd/system/iphone-link-btclass.service"
set +e
setup apply >/dev/null 2>&1; rc=$?
set -e
check "apply : ancienne unité sans en-tête laissée en place" test -f "$R/etc/systemd/system/iphone-link-btclass.service"
check_eq "apply : le signale (code 2)" 2 "$rc"

# --- 5 ter. installation sous l'ancien nom ----------------------------------

# Ce qu'un « apply » de l'assistant d'avant (iphone-link-setup) laissait sur une
# machine vierge : unité ERTM, fichier d'appoint, copie de l'assistant, fiche.
old_install() {
    local marker='# Fichier géré par iphone-link (iphone-link-setup).'
    mkdir -p "$R/etc/systemd/system/bluetooth.service.d" "$R/etc/systemd/system/multi-user.target.wants" \
             "$R/usr/local/libexec/iphone-link"
    printf '%s\n[Service]\nExecStart=\nExecStart=/usr/libexec/bluetooth/bluetoothd --experimental\n' "$marker" \
        > "$R/etc/systemd/system/bluetooth.service.d/iphone-link-experimental.conf"
    printf '%s\n[Service]\nType=oneshot\nExecStart=/bin/true\n' "$marker" \
        > "$R/etc/systemd/system/iphone-link-ertm.service"
    ln -sfn /etc/systemd/system/iphone-link-ertm.service \
        "$R/etc/systemd/system/multi-user.target.wants/iphone-link-ertm.service"
    printf '#!/bin/bash\n%s\nexit 0\n' "$marker" > "$R/usr/local/libexec/iphone-link/iphone-link-setup"
    chmod +x "$R/usr/local/libexec/iphone-link/iphone-link-setup"
    printf '# Fichier géré par iphone-link : /usr/local/libexec a été créé par iphone-link-setup.\n' \
        > "$R/usr/local/libexec/iphone-link/.parent-created"
    printf '<!-- Fichier géré par iphone-link (iphone-link-setup). -->\n<policyconfig/>\n' \
        > "$R/usr/share/polkit-1/actions/io.github.d101c.iphone-link.policy"
    # Le bluetoothd en service a déjà l'option, l'ERTM est déjà actif.
    printf '/usr/libexec/bluetooth/bluetoothd\0--experimental\0' > "$R/proc/4242/cmdline"
    echo N > "$R/sys/module/bluetooth/parameters/disable_ertm"
}
OLD_FILES="etc/systemd/system/bluetooth.service.d/iphone-link-experimental.conf
etc/systemd/system/iphone-link-ertm.service
etc/systemd/system/multi-user.target.wants/iphone-link-ertm.service
usr/local/libexec/iphone-link/iphone-link-setup
usr/local/libexec/iphone-link/.parent-created
usr/local/libexec/iphone-link
usr/share/polkit-1/actions/io.github.d101c.iphone-link.policy"

echo "== Installation sous l'ancien nom : apply la reprend"
new_root neuf
setup apply >/dev/null
fresh="$(snapshot)"
new_root ancien-nom
before="$(snapshot)"
old_install
check_eq "status : les anciens réglages comptent tant qu'ils sont là" "$ALL_TRUE" "$(status)"
dry="$(setup apply --dry-run)"
check "apply --dry-run : annonce le retrait de l'ancienne unité ERTM" grep -q 'retirerait : /etc/systemd/system/iphone-link-ertm.service' <<< "$dry"
check "apply --dry-run : annonce le retrait de l'ancien fichier d'appoint" grep -q 'retirerait : /etc/systemd/system/bluetooth.service.d/iphone-link-experimental.conf' <<< "$dry"
check "apply --dry-run : annonce le nouveau fichier d'appoint" grep -q 'écrirait : /etc/systemd/system/bluetooth.service.d/landline-experimental.conf' <<< "$dry"
check "apply --dry-run : annonce le retrait de l'ancienne fiche" grep -q 'retirerait : /usr/share/polkit-1/actions/io.github.d101c.iphone-link.policy' <<< "$dry"
check_not "apply --dry-run : pas de redémarrage de bluetoothd" grep -q 'exécuterait : systemctl restart bluetooth.service' <<< "$dry"
: > "$R/.commands.log"
out="$(setup apply)"
while IFS= read -r f; do
    check_not "apply : retire $f" test -e "$R/$f"
done <<< "$OLD_FILES"
check_eq "apply : même arborescence qu'une installation neuve" "$fresh" "$(snapshot)"
check_eq "apply : toujours prêt" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
check_not "apply : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
check "apply : systemd relit ses unités" grep -q 'daemon-reload' "$R/.commands.log"
reboot_machine
check_eq "après redémarrage : toujours prêt" "$ALL_TRUE" "$(status)"
setup revert >/dev/null
check_eq "revert : arborescence d'origine" "$before" "$(snapshot)"

echo "== Installation sous l'ancien nom : revert seul la retire"
new_root ancien-nom-revert
before="$(snapshot)"
old_install
dry="$(setup revert --dry-run)"
check "revert --dry-run : annonce le retrait de l'ancienne copie" grep -q 'retirerait : /usr/local/libexec/iphone-link/iphone-link-setup' <<< "$dry"
: > "$R/.commands.log"
setup revert >/dev/null
check_eq "revert : arborescence d'origine" "$before" "$(snapshot)"
check_eq "revert : ERTM remis à la valeur de sysfs.conf" 1 "$(ertm_value)"
check "revert : bluetoothd redémarré sans --experimental" grep -q '^systemctl restart bluetooth.service$' "$R/.commands.log"

echo "== Installation sous l'ancien nom : fichier étranger laissé en place"
new_root ancien-nom-etranger
old_install
printf '[Unit]\nDescription=à moi\n' > "$R/etc/systemd/system/iphone-link-ertm.service"
set +e
setup apply >/dev/null 2>&1; rc=$?
set -e
check "apply : unité sans en-tête laissée en place" test -f "$R/etc/systemd/system/iphone-link-ertm.service"
check_eq "apply : le signale (code 2)" 2 "$rc"

# --- 5 quater. tolérance du lien LE (auxiliaire landline-le-params) ---------

echo "== Tolérance du lien LE : souhaitée, jamais bloquante"
HELPER=usr/local/libexec/landline/landline-le-params
SRC="$WORK/landline-le-params"
printf '\177ELF\002\001\001\000fake helper\n' > "$SRC"
chmod 0755 "$SRC"
LE_TRUE="${ALL_TRUE/\"le_params\":false/\"le_params\":true}"

new_root le-sans-source
set +e
out="$(setup apply 2>"$WORK/err")"; rc=$?
set -e
check_eq "sans source : apply réussit quand même" 0 "$rc"
check_eq "sans source : prêt, le_params à faux" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
check "sans source : le dit" grep -q 'auxiliaire landline-le-params introuvable' "$WORK/err"
check_not "sans source : rien posé" test -e "$R/$HELPER"
check_not "sans source : pas de setcap" grep -q setcap "$R/.commands.log"

new_root le-pose
before="$(snapshot)"
dry="$(LANDLINE_LE_PARAMS_SRC="$SRC" setup apply --dry-run)"
check "dry-run : annonce la copie" grep -q "copierait : $SRC -> /$HELPER" <<< "$dry"
check "dry-run : annonce setcap" grep -q "exécuterait : setcap cap_net_raw+ep /$HELPER" <<< "$dry"
check_eq "dry-run : rien n'est écrit" "$before" "$(snapshot)"
out="$(LANDLINE_LE_PARAMS_SRC="$SRC" setup apply)"
check_eq "apply : prêt et le_params à vrai" "$LE_TRUE" "$(tail -n 1 <<< "$out")"
check_eq "status : le confirme" "$LE_TRUE" "$(status)"
check "apply : auxiliaire identique à sa source" cmp -s "$SRC" "$R/$HELPER"
check_eq "apply : mode 0755" 755 "$(stat -c %a "$R/$HELPER")"
check "apply : marque posée" grep -q 'Managed by landline-setup' "$R/$HELPER.managed"
check "apply : setcap sur la copie provisoire, avant sa mise en place" \
    grep -qx 'setcap cap_net_raw+ep /usr/local/libexec/landline/.landline-le-params.new' "$R/.commands.log"
check_not "apply : pas de copie provisoire laissée" test -e "$R/usr/local/libexec/landline/.landline-le-params.new"
check "apply : le dit" grep -q 'auxiliaire en place avec cap_net_raw' <<< "$out"
: > "$R/.commands.log"
out="$(LANDLINE_LE_PARAMS_SRC="$SRC" setup apply)"
check "2e apply : inchangé" grep -q 'inchangé : /usr/local/libexec/landline/landline-le-params' <<< "$out"
check_not "2e apply : pas de nouveau setcap" grep -q setcap "$R/.commands.log"
out="$(setup apply)"
check "apply sans source, déjà posé : gardé" grep -q 'auxiliaire déjà en place' <<< "$out"
check_eq "apply sans source, déjà posé : toujours vrai" "$LE_TRUE" "$(tail -n 1 <<< "$out")"
chmod 0775 "$R/$HELPER"
check "écrivable par le groupe : plus opérationnel" grep -q '"le_params":false' <<< "$(status)"
chmod 0755 "$R/$HELPER"
rm "$R/.state/caps"
check "sans le droit : plus opérationnel" grep -q '"le_params":false' <<< "$(status)"
echo yes > "$R/.state/caps"
dry="$(setup revert --dry-run)"
check "revert --dry-run : annonce le retrait" grep -q 'retirerait : /usr/local/libexec/landline/landline-le-params' <<< "$dry"
out="$(setup revert)"
check_not "revert : auxiliaire retiré" test -e "$R/$HELPER"
check_not "revert : marque retirée" test -e "$R/$HELPER.managed"
check "revert : le_params à faux" grep -q '"le_params":false' <<< "$(tail -n 1 <<< "$out")"
check_eq "revert : arborescence d'origine" "$before" "$(snapshot)"

new_root le-pas-elf
printf '#!/bin/sh\necho pirate\n' > "$WORK/script"
out="$(LANDLINE_LE_PARAMS_SRC="$WORK/script" setup apply 2>/dev/null)"
check_not "source qui n'est pas un ELF : refusée" test -e "$R/$HELPER"
check_eq "source qui n'est pas un ELF : prêt quand même" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"
ln -s "$SRC" "$WORK/lien"
LANDLINE_LE_PARAMS_SRC="$WORK/lien" setup apply >/dev/null 2>&1
check_not "source qui est un lien : refusée" test -e "$R/$HELPER"

new_root le-setcap-echoue
touch "$R/.state/setcap-fails"
set +e
out="$(LANDLINE_LE_PARAMS_SRC="$SRC" setup apply 2>"$WORK/err")"; rc=$?
set -e
check_eq "setcap échoue : apply réussit quand même" 0 "$rc"
check "setcap échoue : le dit" grep -q 'setcap a échoué' "$WORK/err"
check_not "setcap échoue : rien laissé" test -e "$R/$HELPER"
check_not "setcap échoue : pas de copie provisoire" test -e "$R/usr/local/libexec/landline/.landline-le-params.new"
check_eq "setcap échoue : prêt, le_params à faux" "$ALL_TRUE" "$(tail -n 1 <<< "$out")"

new_root le-etranger
mkdir -p "$R/usr/local/libexec/landline"
cp "$SRC" "$R/$HELPER"
set +e
setup revert >/dev/null 2>&1; rc=$?
set -e
check "revert : auxiliaire sans marque laissé en place" test -f "$R/$HELPER"
check_eq "revert : le signale (code 2)" 2 "$rc"

# La source de l'auxiliaire n'est prise qu'à côté de l'assistant lancé, jamais
# dans le dossier personnel de l'utilisateur qui a demandé les droits (#25).
# L'assistant est copié ailleurs pour chaque disposition ; getent est une
# doublure qui donne à l'uid 4321 un dossier personnel où un auxiliaire
# « piégé » (un ELF valide) attend.
echo "== Source de l'auxiliaire : à côté de l'assistant seulement"
PIRATE_HOME="$WORK/home-pirate"
mkdir -p "$PIRATE_HOME/.local/lib/landline/engine" "$WORK/fakebin"
printf '\177ELF\002\001\001\000piege du dossier personnel\n' > "$PIRATE_HOME/.local/lib/landline/engine/landline-le-params"
chmod 0755 "$PIRATE_HOME/.local/lib/landline/engine/landline-le-params"
cat > "$WORK/fakebin/getent" <<STUB
#!/bin/sh
[ "\$1" = passwd ] && [ "\$2" = 4321 ] && echo "pirate:x:4321:4321::$PIRATE_HOME:/bin/sh"
STUB
chmod +x "$WORK/fakebin/getent"
# copy_setup <dossier> : l'assistant posé dans <dossier>, son chemin en sortie.
copy_setup() { mkdir -p "$1"; cp "$SETUP" "$1/landline-setup"; chmod 0755 "$1/landline-setup"; printf '%s\n' "$1/landline-setup"; }

for who in PKEXEC_UID SUDO_UID; do
    new_root "le-home-$who"
    copy="$(copy_setup "$WORK/copie-de-root-$who")"
    set +e
    out="$(env PATH="$WORK/fakebin:$PATH" "$who=4321" LANDLINE_ROOT="$R" "$copy" apply 2>"$WORK/err")"; rc=$?
    set -e
    check_eq "copie de root, $who : apply réussit" 0 "$rc"
    check_not "copie de root, $who : l'auxiliaire du dossier personnel n'est pas pris" test -e "$R/$HELPER"
    check_not "copie de root, $who : pas de setcap" grep -q setcap "$R/.commands.log"
    check "copie de root, $who : dit qu'il n'y en a pas à côté" grep -q 'introuvable à côté de l' "$WORK/err"
done

new_root le-a-cote-root
copy="$(copy_setup "$WORK/libexec-root")"
printf '\177ELF\002\001\001\000auxiliaire de root\n' > "$WORK/libexec-root/landline-le-params"
env PATH="$WORK/fakebin:$PATH" PKEXEC_UID=4321 LANDLINE_ROOT="$R" "$copy" apply >/dev/null 2>&1
check "copie de root, auxiliaire à côté : c'est lui qui est posé" cmp -s "$WORK/libexec-root/landline-le-params" "$R/$HELPER"

new_root le-un-clic
copy="$(copy_setup "$WORK/un-clic/engine/system")"
printf '\177ELF\002\001\001\000moteur un clic\n' > "$WORK/un-clic/engine/landline-le-params"
env SUDO_UID=4321 PATH="$WORK/fakebin:$PATH" LANDLINE_ROOT="$R" "$copy" apply >/dev/null 2>&1
check "un clic (engine/system/landline-setup) : engine/landline-le-params posé" \
    cmp -s "$WORK/un-clic/engine/landline-le-params" "$R/$HELPER"

new_root le-local
copy="$(copy_setup "$WORK/local")"
mkdir -p "$WORK/local/engine"
printf '\177ELF\002\001\001\000install-local\n' > "$WORK/local/engine/landline-le-params"
env SUDO_UID=4321 PATH="$WORK/fakebin:$PATH" LANDLINE_ROOT="$R" "$copy" apply >/dev/null 2>&1
check "install-local (landline-setup + engine/) : engine/landline-le-params posé" \
    cmp -s "$WORK/local/engine/landline-le-params" "$R/$HELPER"

new_root le-pas-system
copy="$(copy_setup "$WORK/autre/engine/ailleurs")"
printf '\177ELF\002\001\001\000voisin\n' > "$WORK/autre/engine/landline-le-params"
LANDLINE_ROOT="$R" "$copy" apply >/dev/null 2>&1
check_not "dossier parent pris seulement quand l'assistant est dans system/" test -e "$R/$HELPER"

# --- 5 bis. version de BlueZ et mode expérimental ---------------------------

echo "== BlueZ 5.88 et après : mode expérimental non requis"
new_root bluez588
echo 5.88 > "$R/.state/bluez-version"
check_eq "5.88, ERTM coupé : pas prêt, expérimental non requis" \
    '{"experimental":false,"device_class":false,"ertm":false,"ready":false,"persistent":{"experimental":false,"device_class":false,"ertm":false},"required":{"experimental":false,"ertm":true},"bluez_version":"5.88","le_params":false}' \
    "$(status)"
out="$(setup apply)"
check_not "5.88 : pas de fichier d'appoint" test -e "$R/etc/systemd/system/bluetooth.service.d/landline-experimental.conf"
check_not "5.88 : pas de redémarrage de bluetoothd" grep -q 'restart bluetooth.service' "$R/.commands.log"
check "5.88 : le dit" grep -q 'non requis avec BlueZ 5.88' <<< "$out"
check "5.88 : prêt sans mode expérimental" grep -q '"experimental":false,"device_class":false,"ertm":true,"ready":true' <<< "$(tail -n 1 <<< "$out")"
reboot_machine
check "5.88 : prêt après redémarrage" grep -q '"ready":true,"persistent":{"experimental":false,"device_class":false,"ertm":true}' <<< "$(status)"

new_root rien-a-demander
echo 5.88 > "$R/.state/bluez-version"
rm "$R/etc/sysfs.conf"
echo N > "$R/sys/module/bluetooth/parameters/disable_ertm"
check_eq "ERTM actif et persistant, expérimental non requis : prêt sans rien demander" \
    '{"experimental":false,"device_class":false,"ertm":true,"ready":true,"persistent":{"experimental":false,"device_class":false,"ertm":true},"required":{"experimental":false,"ertm":true},"bluez_version":"5.88","le_params":false}' \
    "$(status)"

new_root versions
for pair in "5.87:true" "5.88:false" "5.100:false" "6.0:false" "5.9:true" "4.101:true" "5.66:true"; do
    echo "${pair%%:*}" > "$R/.state/bluez-version"
    check "BlueZ ${pair%%:*} : expérimental requis = ${pair##*:}" \
        grep -q "\"required\":{\"experimental\":${pair##*:}," <<< "$(status)"
done
: > "$R/.state/bluez-version"
check "version illisible : expérimental requis, version vide" \
    grep -q '"required":{"experimental":true,"ertm":true},"bluez_version":""' <<< "$(status)"
rm "$R/.stubs/bluetoothctl"
printf '#!/bin/bash\n[ "$1" = --version ] && echo 5.90\n' > "$R/.stubs/bluetoothd"
chmod +x "$R/.stubs/bluetoothd"
check "sans bluetoothctl : version lue auprès de bluetoothd" \
    grep -q '"required":{"experimental":false,"ertm":true},"bluez_version":"5.90"' <<< "$(status)"

# --- 6. unités générées -----------------------------------------------------

echo "== Unités et règle générées"
new_root verif
setup apply >/dev/null
if command -v systemd-analyze >/dev/null 2>&1; then
    v="$WORK/verify"
    mkdir -p "$v"
    cp "$R/etc/systemd/system/landline-ertm.service" "$v/"
    for unit in landline-ertm.service; do
        msg="$(systemd-analyze verify "$v/$unit" 2>&1 | grep "$unit" || true)"
        check_eq "systemd-analyze verify $unit" "" "$msg"
    done
else
    echo "  --   systemd-analyze absent : unités non vérifiées"
fi
echo
echo "$pass réussis, $fail en échec"
[ "$fail" -eq 0 ]
