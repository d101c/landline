#!/usr/bin/env bash
# L'extension dans un vrai GNOME Shell, sans écran, branchée sur le faux moteur.
#
#   tools/shell-test/run.sh            joue tous les états, vérifie, capture
#   SCALE=2 tools/shell-test/run.sh    captures deux fois plus fines
#   SCHEME=prefer-light SHOTS=/tmp/clair tools/shell-test/run.sh
#                                      style clair, captures ailleurs
#   INNER=tools/shell-test/proto-notifs.sh tools/shell-test/run.sh
#                                      un autre déroulé que inner.sh (chemin absolu ou relatif au dépôt)
#   FAUX_APPSTORE=/chemin/icones tools/shell-test/run.sh
#                                      icônes des applis : <identifiant>.png de ce
#                                      dossier (hors dépôt) ; sans lui, aucune.
#                                      L'App Store n'est jamais joint.
#   SECTIONS="parcours cartes" tools/shell-test/run.sh
#                                      seulement ces parties (voir inner.sh)
#
# HOME, dossiers XDG, dossier d'exécution et bus D-Bus sont propres à l'essai :
# ni la session de l'utilisateur, ni ses extensions, ni le vrai moteur ne sont
# touchés. Le faux moteur écoute à l'endroit par défaut du moteur, mais dans le
# dossier d'exécution de l'essai ($RT/landline/landlined.sock) : l'extension n'a
# besoin d'aucune variable pour le trouver. pkexec et systemctl sont remplacés
# par les faux de tools/shell-test/bin : aucun mot de passe n'est demandé, aucun
# service n'est démarré, aucun réglage n'est changé (« systemctl restart » ne
# relance que le faux moteur). L'extension copiée est celle qui serait publiée,
# sans rien pour les essais : c'est le pilote (pilote@test, activé avant elle)
# qui remplace l'assistant système, NetworkManager et le partage d'écran par
# des doublures, et qui retrouve les boutons par leur libellé.
#
# Sorties : tools/shell-test/out (*.json, shell.log, moteur.log, session.log) et les
# captures dans docs/captures.
set -u
S="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$S/../.." && pwd)"
UUID="landline@d101c.github.io"
OUT="${OUT:-$S/out}"; SHOTS="${SHOTS:-$REPO/docs/captures}"

rm -rf "$OUT"; mkdir -p "$OUT" "$SHOTS"
# Dans /tmp : le chemin d'un socket Unix ne dépasse pas 108 octets.
RT=$(mktemp -d /tmp/ipl.XXXX); chmod 700 "$RT"
T="$RT/home"
mkdir -p "$T/config" "$T/data/gnome-shell/extensions/$UUID" "$T/cache" "$RT/landline"
cp -r "$REPO/extension/." "$T/data/gnome-shell/extensions/$UUID/"
glib-compile-schemas "$T/data/gnome-shell/extensions/$UUID/schemas"
cp -r "$S/pilote@test" "$T/data/gnome-shell/extensions/"

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_ID \
  HOME="$T" XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" \
  XDG_RUNTIME_DIR="$RT" GSETTINGS_BACKEND=dconf \
  FAUX_SOCKET="$RT/landline/landlined.sock" PILOTE_DIR="$OUT" SHOTS="$SHOTS" REPO="$REPO" \
  PATH="$S/bin:$PATH" PILOTE_SETUP="$REPO/tools/faux-landline-setup" \
  FAUX_SETUP_STATE="$RT/reglages.json" FAUX_PKEXEC_REFUSE="$RT/refuse" FAUX_PKEXEC_HOLD="$RT/attente" \
  SECTIONS="${SECTIONS:-}" HEURE="${HEURE:-}" FAUX_APPSTORE="${FAUX_APPSTORE:-}" \
  FAUX_PKEXEC_LOG="$OUT/pkexec.log" FAUX_SYSTEMCTL_LOG="$OUT/systemctl.log" \
  FAUX_SYSTEMCTL_REFUSE="$RT/refuse-relance" \
  SCALE="${SCALE:-1}" SCHEME="${SCHEME:-prefer-dark}" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 LANGUAGE= \
  dbus-run-session -- "${INNER:-$S/inner.sh}" 2> "$OUT/session.log"
status=$?
# Les services de la session privée s'arrêtent avec son bus ; le portail de
# documents laisse parfois son montage un instant.
sleep 1; fusermount3 -u -z "$RT/doc" 2>/dev/null
rm -rf "$RT" 2>/dev/null
exit $status
