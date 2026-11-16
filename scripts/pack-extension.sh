#!/usr/bin/env bash
# Fabrique l'archive de l'extension, telle qu'elle serait envoyée.
#
#   scripts/pack-extension.sh [dossier de sortie]     (défaut : ./dist)
#
# L'archive ne contient que extension/ : ni l'assistant système, ni rien des
# essais (tools/, tests/). Rien n'est installé.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/dist}"
mkdir -p "$OUT"

[ ! -e "$REPO/extension/helper" ] || { echo "extension/helper ne doit pas exister" >&2; exit 1; }

gnome-extensions pack "$REPO/extension" \
    --extra-source=lib --extra-source=icons --extra-source=illustrations \
    --extra-source=stylesheet-light.css --extra-source=LICENSE \
    --force --out-dir "$OUT"
ls -l "$OUT"/*.shell-extension.zip
