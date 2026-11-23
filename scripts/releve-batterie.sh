#!/usr/bin/env bash
# Relevé léger, une ligne par passage, pour suivre sur plusieurs jours ce que la
# liaison coûte à la batterie de l'iPhone et à l'ordinateur.
# Ne parle qu'au moteur (état déjà en mémoire) et lit son journal : n'ouvre aucun
# lien Bluetooth et ne réveille pas le téléphone.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$repo/scripts/noms.sh"

out="${1:-$HOME/DEVELOPEMENT/reports/landline-batterie.csv}"
log="${XDG_STATE_HOME:-$HOME/.local/state}/$NAME_PREFIX/${NAME_PREFIX}d.log"
ctl="$HOME/.local/lib/$NAME_PREFIX/engine/${NAME_PREFIX}ctl"
day="$(date +%F)"

mkdir -p "$(dirname "$out")"
[ -f "$out" ] || echo "date,heure,iphone_batterie,le,classique,mode,ouvertures_classique_jour,ouvertes_par_iphone_jour,releves_boite_jour,coupures_le_jour,moteur_cpu_s,moteur_mem_mo,pc_batterie,pc_etat" > "$out"

state="$(LANDLINE_NO_AUTOSTART=1 timeout 10 "$ctl" bt connection 2>/dev/null || true)"
field() { printf '%s\n' "$state" | sed -n "s/^$1:[[:space:]]*\\([^ %]*\\).*/\\1/p" | head -1; }
battery="$(field Battery)"; le="$(field LE)"; classic="$(field 'BR\/EDR')"

count() { if [ -f "$log" ]; then grep "^\[$day" "$log" | grep -c -- "$1" || true; else echo 0; fi; }
mode="$(grep -o 'link mode [a-z]*' "$log" 2>/dev/null | tail -1 | awk '{print $3}')"

cpu_ns="$(systemctl --user show "$ENGINE_UNIT" -p CPUUsageNSec --value 2>/dev/null || echo 0)"
mem="$(systemctl --user show "$ENGINE_UNIT" -p MemoryCurrent --value 2>/dev/null || echo 0)"
case "$cpu_ns" in ''|*[!0-9]*) cpu_ns=0 ;; esac
case "$mem" in ''|*[!0-9]*) mem=0 ;; esac

bat="$(ls -d /sys/class/power_supply/BAT* 2>/dev/null | head -1)"
pc_pct="$( [ -n "$bat" ] && cat "$bat/capacity" 2>/dev/null || echo "")"
pc_state="$( [ -n "$bat" ] && cat "$bat/status" 2>/dev/null || echo "")"

echo "$day,$(date +%H:%M),${battery:-},${le:-},${classic:-},${mode:-},$(count 'classic link opening'),$(count 'opened by phone'),$(count 'inbox listed'),$(count 'Bearer.LE1 disconnected'),$((cpu_ns / 1000000000)),$((mem / 1048576)),$pc_pct,$pc_state" >> "$out"
