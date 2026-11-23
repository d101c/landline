# Les noms du produit, lus (« source ») par les scripts de ce dossier et par
# system/tests/install.sh. scripts/renommer.sh réécrit ces trois lignes.
#
#   NAME_DISPLAY  le nom affiché de l'extension
#   NAME_ID       l'identifiant long : uuid (<id>@d101c.github.io), schéma
#                 GSettings, domaine gettext, nom du dépôt
#   NAME_PREFIX   le préfixe court : service, binaires, dossiers, variables
# shellcheck shell=bash disable=SC2034
NAME_DISPLAY="Landline"
NAME_ID="landline"
NAME_PREFIX="landline"

# Tout le reste en dérive.
EXT_UUID="$NAME_ID@d101c.github.io"
EXT_SCHEMA_PATH="/org/gnome/shell/extensions/$NAME_ID/"
ENGINE_UNIT="$NAME_PREFIX-engine.service"
ENGINE_DAEMON="${NAME_PREFIX}d"
ENGINE_CLI="${NAME_PREFIX}ctl"
ENGINE_LOG="${NAME_PREFIX}d.log"
SETUP_NAME="$NAME_PREFIX-setup"
SYSTEM_HELPER="/usr/local/libexec/$NAME_PREFIX/$SETUP_NAME"
AUDIO_RULE="zz-$NAME_PREFIX-phone-audio.conf"
# Complément de l'unité obex.service (relance d'obexd) : source dans system/,
# posé sous ~/.config/systemd/user/$OBEX_DROPIN_DIR/$OBEX_DROPIN.
OBEX_DROPIN_SRC="obex-$NAME_PREFIX.conf"
OBEX_DROPIN_DIR="obex.service.d"
OBEX_DROPIN="$NAME_PREFIX.conf"

# Les noms d'avant, reconnus pour la migration d'une installation existante :
# l'extension et le service s'appelaient « iphone-link », le moteur gardait
# les noms de Tether, dont il est dérivé.
OLD_EXT_UUID="iphone-link@d101c.github.io"
OLD_EXT_SCHEMA_PATH="/org/gnome/shell/extensions/iphone-link/"
OLD_ENGINE_UNIT="iphone-link-engine.service"
OLD_INSTALL_NAME="iphone-link"
OLD_DATA_NAME="tether"
OLD_ENGINE_DAEMON="tetherd"
OLD_ENGINE_LOG="tetherd.log"
OLD_AUDIO_RULE="zz-tether-phone-audio.conf"
