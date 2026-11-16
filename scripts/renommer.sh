#!/usr/bin/env bash
# Change le nom du produit partout dans le dépôt, puis lance les tests.
#
#   scripts/renommer.sh "<Nom affiché>" <identifiant-long> <préfixe-court> [options]
#
#   Nom affiché       le nom de l'extension, tel que l'utilisateur le lit
#   identifiant-long  uuid (<id>@d101c.github.io), schéma GSettings, domaine
#                     gettext, nom du dépôt GitHub      [a-z][a-z0-9-]*
#   préfixe-court     service, binaires (<p>d, <p>ctl), dossiers, socket,
#                     assistant (<p>-setup), fiche polkit, variables (<P>_…),
#                     classes de style, icônes          [a-z][a-z0-9]*
#
# Options :
#   --sans-tests    ne lance aucun test
#   --sans-moteur   ne recompile pas le moteur (tests du modèle, du client et
#                   de l'assistant système seulement)
#   --force         accepte un arbre de travail qui a des modifications
#
# Le préfixe court ne doit figurer nulle part dans le dépôt, même au milieu
# d'un mot : le script le vérifie et refuse sinon.
#
# Les noms courants sont lus dans scripts/noms.sh, que le script met à jour.
# Les noms d'avant reconnus pour la migration (« iphone-link », « tether ») ne
# sont jamais touchés.
#
# Ce qui est fait, dans l'ordre :
#   1. le nom affiché est remplacé partout ;
#   2. l'identifiant long est remplacé : partout s'il diffère du préfixe
#      courant, sinon seulement là où le contexte le désigne (uuid, schéma,
#      chemin dconf, dépôt GitHub, domaine gettext, préfixe du journal JS) ;
#   3. le préfixe court est remplacé partout, sous ses trois écritures
#      (préfixe, Préfixe, PRÉFIXE) ;
#   4. les fichiers dont le nom porte l'identifiant ou le préfixe sont renommés ;
#   5. les tests sont lancés (modèle, client, assistant système, puis
#      compilation et tests du moteur, puis scripts d'installation).
#
# Portée dans engine/ : seulement ce que nous avons renommé (src/core,
# src/daemon, src/cli, test, po, les deux CMakeLists de tête, l'unité modèle
# et le script de remise à zéro). La documentation et le reste du code de
# Tether ne sont pas touchés.
#
# Ce que le script ne fait pas : refaire les captures (les images portent le
# nom : tools/shell-test/run.sh), renommer le dossier du dépôt, migrer une
# installation faite sous le nom courant (install-local.sh ne connaît que les
# noms « iphone-link » et « tether »).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
# shellcheck source=scripts/noms.sh
. scripts/noms.sh

tests=1
engine=1
force=0
args=()
for arg in "$@"; do
    case "$arg" in
        --sans-tests)  tests=0 ;;
        --sans-moteur) engine=0 ;;
        --force)       force=1 ;;
        -h|--help)     awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*)            echo "option inconnue : $arg" >&2; exit 2 ;;
        *)             args+=("$arg") ;;
    esac
done
[ "${#args[@]}" -eq 3 ] || { echo "usage : scripts/renommer.sh \"<Nom affiché>\" <identifiant-long> <préfixe-court> [--sans-tests|--sans-moteur|--force]" >&2; exit 2; }

NEW_DISPLAY="${args[0]}"
NEW_ID="${args[1]}"
NEW_PREFIX="${args[2]}"

die() { printf 'erreur : %s\n' "$*" >&2; exit 1; }
[[ "$NEW_ID" =~ ^[a-z][a-z0-9-]*$ ]]   || die "identifiant long invalide : « $NEW_ID »"
[[ "$NEW_PREFIX" =~ ^[a-z][a-z0-9]*$ ]] || die "préfixe court invalide : « $NEW_PREFIX » (lettres minuscules et chiffres)"
[ -n "$NEW_DISPLAY" ] || die "nom affiché vide"
case "$NEW_DISPLAY" in *[\"\'\\/\&\|]*) die "nom affiché : caractères \" ' \\ / & | refusés" ;; esac
for reserved in tether iphone-link; do
    case "$NEW_PREFIX" in "$reserved"*) die "« $reserved » est un nom d'avant, reconnu par la migration : il ne peut pas resservir" ;; esac
    case "$NEW_ID" in "$reserved"*) die "« $reserved » est un nom d'avant, reconnu par la migration : il ne peut pas resservir" ;; esac
done
if [ "$force" = 0 ] && [ -n "$(git status --porcelain)" ]; then
    die "l'arbre de travail a des modifications : les valider d'abord, ou --force"
fi
if [ "$NEW_DISPLAY" = "$NAME_DISPLAY" ] && [ "$NEW_ID" = "$NAME_ID" ] && [ "$NEW_PREFIX" = "$NAME_PREFIX" ]; then
    echo "Ce sont déjà les noms courants."
    exit 0
fi

# Un préfixe déjà présent dans le dépôt (même au milieu d'un mot : « truc »
# dans « constructor ») ne pourrait plus être distingué au renommage suivant,
# et ce renommage-ci toucherait du texte qui n'a rien à voir.
if [ "$NEW_PREFIX" != "$NAME_PREFIX" ] && [[ "$NAME_PREFIX" != *"$NEW_PREFIX"* ]] &&
   git grep -q -i -F -e "$NEW_PREFIX" -- . ':!engine/README.md' ':!engine/docs' ':!engine/apple' ':!engine/extension'; then
    git grep -n -i -F -e "$NEW_PREFIX" -- . ':!engine/README.md' ':!engine/docs' ':!engine/apple' ':!engine/extension' | head -n 5 | cut -c1-160 >&2
    die "« $NEW_PREFIX » apparaît déjà dans le dépôt (ci-dessus) : choisir un préfixe qui n'y figure nulle part"
fi

echo "Nom affiché      : $NAME_DISPLAY -> $NEW_DISPLAY"
echo "Identifiant long : $NAME_ID -> $NEW_ID"
echo "Préfixe court    : $NAME_PREFIX -> $NEW_PREFIX"

# Les fichiers suivis, hors images, et dans engine/ la seule partie renommée.
mapfile -t files < <(git ls-files | grep -v -E '\.(png|jpg|svgz|zip)$' | grep -v '^engine/' ; \
    git ls-files engine/src/core engine/src/daemon engine/src/cli engine/test engine/po \
                 engine/CMakeLists.txt engine/src/CMakeLists.txt \
                 engine/packaging/systemd/tetherd.service.in engine/scripts/tether-reset.sh)

OLD_DISPLAY="$NAME_DISPLAY" OLD_ID="$NAME_ID" OLD_PREFIX="$NAME_PREFIX" \
NEW_DISPLAY="$NEW_DISPLAY" NEW_ID="$NEW_ID" NEW_PREFIX="$NEW_PREFIX" \
python3 - "${files[@]}" <<'PY'
import os, re, sys

old_display, old_id, old_prefix = (os.environ[k] for k in ('OLD_DISPLAY', 'OLD_ID', 'OLD_PREFIX'))
new_display, new_id, new_prefix = (os.environ[k] for k in ('NEW_DISPLAY', 'NEW_ID', 'NEW_PREFIX'))

D, I = '\x01D\x01', '\x01I\x01'

def id_in_context(text):
    """The long identifier where the context names it, when it cannot be told
    from the short prefix by its spelling."""
    e = re.escape(old_id)
    patterns = [
        r'(?<![\w-])%s(?=@d101c\.github\.io)' % e,
        r'(?<=org\.gnome\.shell\.extensions\.)%s(?![\w-])' % e,
        r'(?<=/org/gnome/shell/extensions/)%s(?=/)' % e,
        r'(?<=github\.com/d101c/)%s(?![\w-])' % e,
        r'(?<=gettext-domain": ")%s(?=")' % e,
        r'(?<=gettext-domain=")%s(?=")' % e,
        r'(?<=NAME_ID=")%s(?=")' % e,
        r"(?<=LOG_PREFIX = ')%s(?=')" % e,
    ]
    for pattern in patterns:
        text = re.sub(pattern, I, text)
    return text

def rename(text):
    text = text.replace(old_display, D)
    if old_id != old_prefix:
        text = text.replace(old_id, I)
    else:
        text = id_in_context(text)
    text = text.replace(old_prefix, new_prefix)
    text = text.replace(old_prefix.capitalize(), new_prefix.capitalize())
    text = text.replace(old_prefix.upper(), new_prefix.upper())
    return text.replace(D, new_display).replace(I, new_id)

changed = 0
for path in sys.argv[1:]:
    if os.path.islink(path) or not os.path.isfile(path):
        continue
    try:
        with open(path, encoding='utf-8') as handle:
            before = handle.read()
    except UnicodeDecodeError:
        continue
    after = rename(before)
    if after != before:
        with open(path, 'w', encoding='utf-8') as handle:
            handle.write(after)
        changed += 1

# File names carry the names too.
moved = 0
for path in sys.argv[1:]:
    directory, name = os.path.split(path)
    if directory.startswith('engine'):
        continue
    renamed = rename(name)
    if renamed != name and os.path.lexists(path):
        os.rename(path, os.path.join(directory, renamed))
        moved += 1
print(f'{changed} fichiers modifiés, {moved} fichiers renommés')
PY

git add -A >/dev/null 2>&1 || true

echo
echo "== Restes de l'ancien nom (à relire à la main s'il y en a)"
if git grep -n -i -e "$NAME_PREFIX" -e "$NAME_DISPLAY" -- . ':!engine/README.md' ':!engine/docs' ':!engine/apple' \
        ':!engine/extension' ':!engine/src/gtk' ':!engine/src/tui' ':!engine/packaging' | cut -c1-200 | head -40 | grep .; then
    echo "(ci-dessus : occurrences restantes)"
else
    echo "aucun"
fi
if [ "$NAME_ID" = "$NAME_PREFIX" ] && [ "$NEW_ID" != "$NEW_PREFIX" ]; then
    echo "L'ancien identifiant et l'ancien préfixe étaient identiques : hors uuid, schéma, chemin dconf,"
    echo "dépôt, domaine gettext et préfixe du journal, chaque mention a pris le préfixe « $NEW_PREFIX »."
    echo "Relire les textes (docs/, README.md) pour les mentions qui désignaient l'identifiant."
fi

if [ "$tests" = 0 ]; then
    echo
    echo "Tests non lancés (--sans-tests)."
    exit 0
fi

status=0
step() {
    echo
    echo "== $1"
    shift
    if ! "$@"; then status=1; echo "ÉCHEC : $*"; fi
}
step "Modèle et client" tests/run.sh
step "Assistant système" bash -c 'system/tests/run.sh | tail -n 1'
if [ "$engine" = 1 ]; then
    step "Moteur (compilation et tests unitaires)" bash -c 'scripts/build-engine.sh --clean > "$0" 2>&1; rc=$?; tail -n 3 "$0"; exit $rc' "$(mktemp)"
    step "Scripts d'installation" bash -c 'system/tests/install.sh | tail -n 1'
else
    echo
    echo "Moteur non recompilé (--sans-moteur) : les binaires de engine/build/local portent encore l'ancien nom."
fi
echo
if [ "$status" = 0 ]; then
    echo "Renommage terminé, tests réussis. Rien n'est validé : relire « git diff --cached --stat », refaire les"
    echo "captures (tools/shell-test/run.sh) et valider."
else
    echo "Renommage fait, mais des tests échouent."
fi
exit $status
