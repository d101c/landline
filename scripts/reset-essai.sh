#!/usr/bin/env bash
# Remet l'essai à zéro côté ordinateur, sans droits administrateur :
# arrête le moteur, oublie l'iPhone, efface les données du moteur, relance le moteur.
# Les réglages Bluetooth du système ne sont pas touchés (voir « revert » de
# l'assistant système).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$repo/scripts/noms.sh"

systemctl --user stop "$ENGINE_UNIT" 2>/dev/null || true

bash "$repo/engine/scripts/tether-reset.sh" all -y || true
rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/$NAME_PREFIX" \
       "${XDG_DATA_HOME:-$HOME/.local/share}/$NAME_PREFIX" \
       "${XDG_STATE_HOME:-$HOME/.local/state}/$NAME_PREFIX"

systemctl --user start "$ENGINE_UNIT"
echo "Essai remis à zéro : aucun iPhone relié, moteur relancé."
