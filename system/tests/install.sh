#!/usr/bin/env bash
# Essais de scripts/install-local.sh et uninstall-local.sh dans un HOME
# factice : rien n'est posé dans le vrai dossier de l'utilisateur, rien n'est
# démarré ni activé (les scripts le refusent d'eux-mêmes quand HOME est factice).
#
#   system/tests/install.sh
#
# Demande un moteur compilé : BUILD_DIR (défaut : engine/build/local).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/engine/build/local}"
# shellcheck source=scripts/noms.sh
. "$REPO/scripts/noms.sh"
P="$NAME_PREFIX"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
ko() { fail=$((fail + 1)); printf '  ÉCHEC %s\n' "$1"; }
check()     { local label="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$label"; else ko "$label"; fi; }
check_not() { local label="$1"; shift; if "$@" >/dev/null 2>&1; then ko "$label"; else ok "$label"; fi; }
finish() {
    echo
    echo "$pass réussis, $fail en échec"
    [ "$fail" -eq 0 ]
}

echo "== Syntaxe"
for script in "$REPO/scripts/install-local.sh" "$REPO/scripts/uninstall-local.sh" "$REPO/scripts/noms.sh" \
              "$REPO/scripts/pack-extension.sh" "$REPO/scripts/reset-essai.sh" "${BASH_SOURCE[0]}"; do
    check "bash -n $(basename "$script")" bash -n "$script"
    if command -v shellcheck >/dev/null 2>&1; then
        check "shellcheck $(basename "$script")" shellcheck "$script"
    fi
done
command -v shellcheck >/dev/null 2>&1 || echo "  --   shellcheck absent : analyse statique NON faite"

if command -v systemd-analyze >/dev/null 2>&1; then
    v="$WORK/unit-home"
    mkdir -p "$v/.local/lib/$P/engine"
    printf '#!/bin/sh\n' > "$v/.local/lib/$P/engine/$ENGINE_DAEMON"
    chmod +x "$v/.local/lib/$P/engine/$ENGINE_DAEMON"
    msg="$(HOME="$v" systemd-analyze verify --user "$REPO/system/$ENGINE_UNIT" 2>&1 | grep "$P-engine" || true)"
    if [ -z "$msg" ]; then
        ok "systemd-analyze verify --user $ENGINE_UNIT"
    else
        ko "systemd-analyze verify --user $ENGINE_UNIT : $msg"
    fi
    check "l'unité lance le démon installé" grep -qx "ExecStart=%h/.local/lib/$P/engine/$ENGINE_DAEMON" "$REPO/system/$ENGINE_UNIT"
fi

if [ ! -x "$BUILD_DIR/$ENGINE_DAEMON" ]; then
    echo "  --   pas de moteur compilé dans $BUILD_DIR : installation NON essayée"
    finish
    exit
fi

# Extension factice : seul compte ce que le script en fait.
EXT="$WORK/extension"
UUID="essai@exemple.test"
mkdir -p "$EXT/lib"
printf '{"uuid": "%s", "name": "Essai"}\n' "$UUID" > "$EXT/metadata.json"
echo '// essai' > "$EXT/extension.js"

H="$WORK/home"
mkdir -p "$H"
inst()    { env -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME HOME="$H" BUILD_DIR="$BUILD_DIR" EXTENSION_DIR="$EXT" "$REPO/scripts/install-local.sh" "$@"; }
uninst()  { env -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME HOME="$H" EXTENSION_DIR="$EXT" "$REPO/scripts/uninstall-local.sh" "$@"; }
listing() { (cd "$H" && find . | LC_ALL=C sort); }
E="$H/.local/lib/$P/engine"
X="$H/.local/share/gnome-shell/extensions/$UUID"

echo "== install --dry-run"
out="$(inst --dry-run)"
check_not "rien n'est écrit" test -e "$H/.local"
check "annonce la copie du moteur" grep -q "ferait : install -m 0755 $BUILD_DIR/$ENGINE_DAEMON $E/$ENGINE_DAEMON" <<< "$out"
check "HOME factice : activation refusée" grep -q "rien ne sera démarré ni activé" <<< "$out"
check_not "aucun systemctl annoncé comme exécuté" grep -Eq '^(fait|ferait) : systemctl' <<< "$out"
check "rien à migrer" grep -q "aucune installation sous les anciens noms" <<< "$out"

echo "== install"
inst >/dev/null
check "démon installé" test -x "$E/$ENGINE_DAEMON"
check "outil en ligne de commande installé" test -x "$E/$ENGINE_CLI"
check "licence du moteur à côté des binaires" cmp -s "$REPO/engine/LICENSE" "$E/LICENSE"
check_not "aucune interface d'origine installée" sh -c "ls '$E' | grep -Eq 'gtk|tui|dialog|native-host'"
check "unité utilisateur installée" cmp -s "$REPO/system/$ENGINE_UNIT" "$H/.config/systemd/user/$ENGINE_UNIT"
check_not "unité non activée (HOME factice)" test -e "$H/.config/systemd/user/graphical-session.target.wants"
check "extension copiée sous son uuid" test -f "$X/extension.js"
check "assistant hors de l'extension, dans ~/.local/lib/$P" cmp -s "$REPO/system/$SETUP_NAME" "$H/.local/lib/$P/$SETUP_NAME"
check "assistant exécutable" test -x "$H/.local/lib/$P/$SETUP_NAME"
check_not "rien d'exécutable dans le dossier de l'extension" test -e "$X/helper"
for bin in "$ENGINE_DAEMON" "$ENGINE_CLI"; do
    stray="$(LD_LIBRARY_PATH="$E/lib" ldd "$E/$bin" | awk '$2 == "=>" { print $3 }' |
        grep -Ev "^(/lib/|/lib64/|/usr/lib/|/usr/lib64/|$E/lib/)" || true)"
    if [ -z "$stray" ]; then
        ok "$bin : système et engine/lib seulement"
    else
        ko "$bin dépend encore de : $stray"
    fi
done
# Aucun binaire du moteur n'est lancé ici : ldd suffit à vérifier les copies.
after="$(listing)"
inst >/dev/null
if [ "$after" = "$(listing)" ]; then ok "2e install : mêmes fichiers"; else ko "2e install : mêmes fichiers"; fi

echo "== uninstall"
mkdir -p "$H/.config/$P" "$H/.local/share/$P" "$H/.local/state/$P"
echo x > "$H/.config/$P/bluetooth.json"
uninst --dry-run >/dev/null
check "dry-run : rien n'est retiré" test -x "$E/$ENGINE_DAEMON"
uninst >/dev/null
check_not "moteur retiré" test -e "$H/.local/lib/$P"
check_not "unité retirée" test -e "$H/.config/systemd/user/$ENGINE_UNIT"
check_not "extension retirée" test -e "$X"
check "données gardées sans --purge" test -f "$H/.config/$P/bluetooth.json"
uninst --purge >/dev/null
check_not "--purge : ~/.config/$P retiré" test -e "$H/.config/$P"
check_not "--purge : ~/.local/share/$P retiré" test -e "$H/.local/share/$P"
check_not "--purge : ~/.local/state/$P retiré" test -e "$H/.local/state/$P"

# --- migration ----------------------------------------------------------------

# Une installation sous les noms d'avant : service et extension « iphone-link »,
# données du moteur dans des dossiers « tether ».
old_home() {
    H="$WORK/$1"
    E="$H/.local/lib/$P/engine"
    X="$H/.local/share/gnome-shell/extensions/$UUID"
    mkdir -p "$H/.local/lib/iphone-link/engine" "$H/.config/systemd/user/graphical-session.target.wants" \
             "$H/.local/share/gnome-shell/extensions/iphone-link@d101c.github.io" \
             "$H/.config/tether" "$H/.local/share/tether" "$H/.local/state/tether" \
             "$H/.config/wireplumber/wireplumber.conf.d"
    echo binaire > "$H/.local/lib/iphone-link/engine/tetherd"
    echo assistant > "$H/.local/lib/iphone-link/iphone-link-setup"
    echo '[Unit]' > "$H/.config/systemd/user/iphone-link-engine.service"
    ln -s ../iphone-link-engine.service "$H/.config/systemd/user/graphical-session.target.wants/iphone-link-engine.service"
    echo '{}' > "$H/.local/share/gnome-shell/extensions/iphone-link@d101c.github.io/metadata.json"
    echo '{"device_address":"00:11:22:33:44:55"}' > "$H/.config/tether/bluetooth.json"
    echo cle > "$H/.config/tether/store.key"
    echo messages > "$H/.local/share/tether/messages.ndjson.enc"
    echo journal > "$H/.local/state/tether/tetherd.log"
    echo regle > "$H/.config/wireplumber/wireplumber.conf.d/zz-tether-phone-audio.conf"
}

echo "== migration --dry-run"
old_home ancien
before="$(listing)"
out="$(inst --dry-run)"
if [ "$before" = "$(listing)" ]; then ok "rien n'est touché"; else ko "rien n'est touché"; fi
check "annonce le retrait de l'ancienne unité" grep -q "ferait : rm -f $H/.config/systemd/user/iphone-link-engine.service" <<< "$out"
check "annonce le retrait de l'ancienne extension" grep -q "ferait : rm -rf $H/.local/share/gnome-shell/extensions/iphone-link@d101c.github.io" <<< "$out"
check "annonce le déplacement des réglages" grep -q "ferait : mv $H/.config/tether $H/.config/$P" <<< "$out"
check "annonce le déplacement des données" grep -q "ferait : mv $H/.local/share/tether $H/.local/share/$P" <<< "$out"
check "annonce le déplacement de l'état" grep -q "ferait : mv $H/.local/state/tether $H/.local/state/$P" <<< "$out"
check "annonce le nouveau nom du journal" grep -q "ferait : mv $H/.local/state/$P/tetherd.log $H/.local/state/$P/$ENGINE_LOG" <<< "$out"
check "annonce le nouveau nom de la règle du son" grep -q "ferait : mv .*zz-tether-phone-audio.conf .*/$AUDIO_RULE" <<< "$out"
check "annonce le retrait de l'ancien dossier" grep -q "ferait : rm -rf $H/.local/lib/iphone-link" <<< "$out"
check "dit ce qu'il advient de la clé du trousseau" grep -q "clé du trousseau" <<< "$out"
check_not "HOME factice : ni systemctl ni gnome-extensions" grep -Eq '^(fait|ferait) : (systemctl|gnome-extensions|dconf)' <<< "$out"

echo "== migration"
inst >/dev/null
check_not "ancienne unité retirée" test -e "$H/.config/systemd/user/iphone-link-engine.service"
check_not "ancien lien d'activation retiré" test -L "$H/.config/systemd/user/graphical-session.target.wants/iphone-link-engine.service"
check_not "ancienne extension retirée" test -e "$H/.local/share/gnome-shell/extensions/iphone-link@d101c.github.io"
check_not "ancien dossier d'installation retiré" test -e "$H/.local/lib/iphone-link"
check_not "~/.config/tether n'existe plus" test -e "$H/.config/tether"
check_not "~/.local/share/tether n'existe plus" test -e "$H/.local/share/tether"
check_not "~/.local/state/tether n'existe plus" test -e "$H/.local/state/tether"
check "appareil choisi gardé" grep -q '00:11:22:33:44:55' "$H/.config/$P/bluetooth.json"
check "clé en fichier gardée" grep -qx cle "$H/.config/$P/store.key"
check "messages gardés" grep -qx messages "$H/.local/share/$P/messages.ndjson.enc"
check "journal gardé sous son nouveau nom" grep -qx journal "$H/.local/state/$P/$ENGINE_LOG"
check_not "plus de journal sous l'ancien nom" test -e "$H/.local/state/$P/tetherd.log"
check "règle du son renommée" grep -qx regle "$H/.config/wireplumber/wireplumber.conf.d/$AUDIO_RULE"
check "nouveau moteur installé" test -x "$E/$ENGINE_DAEMON"
check "nouvelle unité installée" test -f "$H/.config/systemd/user/$ENGINE_UNIT"
after="$(listing)"
out="$(inst)"
if [ "$after" = "$(listing)" ]; then ok "2e passage : mêmes fichiers"; else ko "2e passage : mêmes fichiers"; fi
check "2e passage : plus rien à migrer" grep -q "aucune installation sous les anciens noms" <<< "$out"

echo "== migration : données déjà présentes sous le nouveau nom"
old_home ancien-et-nouveau
mkdir -p "$H/.config/$P"
echo nouveau > "$H/.config/$P/bluetooth.json"
out="$(inst)"
check "le dossier existant n'est pas écrasé" grep -qx nouveau "$H/.config/$P/bluetooth.json"
check "l'ancien est laissé en place" test -f "$H/.config/tether/bluetooth.json"
check "et c'est dit" grep -q "existe déjà" <<< "$out"

echo "== uninstall d'une installation restée sous les anciens noms"
old_home ancien-desinstalle
uninst >/dev/null
check_not "ancienne unité retirée" test -e "$H/.config/systemd/user/iphone-link-engine.service"
check_not "ancienne extension retirée" test -e "$H/.local/share/gnome-shell/extensions/iphone-link@d101c.github.io"
check_not "ancien dossier d'installation retiré" test -e "$H/.local/lib/iphone-link"
check "données gardées sans --purge" test -f "$H/.config/tether/bluetooth.json"
uninst --purge >/dev/null
check_not "--purge : anciennes données retirées" test -e "$H/.config/tether"

finish
