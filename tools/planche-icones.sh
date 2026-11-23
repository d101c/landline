#!/usr/bin/env bash
# Planche des icônes en PNG : tools/planche-icones.sh DOSSIER
# Une ligne par schéma et par style ; chaque état à 16 px réels (agrandis ×4
# au plus proche voisin pour voir les vrais pixels), puis en grand.
set -eu
D="$1"; R="$(cd "$(dirname "$0")/.." && pwd)"
states=$(gjs -m "$R/tools/planche-icones.js" "$D")
for f in "$D"/*.svg; do rsvg-convert "$f" -o "${f%.svg}.png"; done
for scheme in sombre clair; do
    for style in phone handset; do
        magick "$D/$scheme-$style-"*-16.png -filter point -resize 400% +append "$D/ligne-$scheme-$style-16.png"
        magick "$D/$scheme-$style-"*-96.png +append "$D/ligne-$scheme-$style-96.png"
    done
done
magick "$D"/ligne-*-16.png -background '#888' -splice 0x4 -append "$D/planche-16px.png"
magick "$D"/ligne-*-96.png -background '#888' -splice 0x4 -append "$D/planche-grand.png"
echo "$states"
echo "$D/planche-16px.png $D/planche-grand.png"
