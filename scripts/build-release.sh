#!/usr/bin/env bash
# Construit l'archive portable du moteur, celle que l'installateur de
# l'extension télécharge : une seule compilation qui tourne sur tout bureau
# GNOME récent (Ubuntu, Debian, Fedora, Arch), x86_64.
#
#   scripts/build-release.sh <version>              construit, teste, empaquette
#   scripts/build-release.sh <version> --no-test    sans la suite de tests
#
# La compilation se fait dans un conteneur Docker sur une base ancienne
# (ubuntu:24.04, glibc 2.39), pour que la glibc exigée reste basse. Rien n'est
# compilé ni installé sur la machine hôte. La source est prise dans le dépôt
# par « git archive HEAD » : seul ce qui est validé part dans l'archive (refus
# si engine/ ou system/ ont des modifications non validées, sauf ALLOW_DIRTY=1,
# qui copie alors l'arbre de travail).
#
# Produit, dans dist/ :
#   landline-engine-<version>-x86_64.tar.zst
#   landline-engine-<version>-x86_64.tar.zst.sha256
#   SHA256SUMS   l'archive, plus system/landline-setup et engine/landline-le-params
#                (chemins relatifs au dossier extrait), que l'installateur épingle
#
# Disposition de l'archive (voir docs/release.md) :
#   landline-engine-<version>/
#     engine/  landlined landlinectl landline-le-params lib/ locale/ LICENSE VERSION
#     system/  landline-setup io.github.d101c.landline.policy
#              landline-engine.service obex-landline.conf
#
# Variables :
#   BASE_IMAGE    image de base (défaut : ubuntu:24.04 ; à épingler par empreinte,
#                 ubuntu@sha256:…, pour une reconstruction à l'identique)
#   DOCKER        commande docker (défaut : docker ; DOCKER_HOST est respecté)
#   DIST_DIR      dossier de sortie (défaut : dist/ à la racine du dépôt)
#   ALLOW_DIRTY   1 : accepte un arbre de travail modifié (archive de test)
#
# Reproductibilité : horodatages fixés à la date du dernier commit touchant
# engine/ ou system/ (SOURCE_DATE_EPOCH), fichiers triés par nom, propriétaire
# 0:0, chemins de compilation constants (/src), binaires dépouillés. Restent variables : le
# contenu de l'image de base si elle n'est pas épinglée, et les versions de
# nlohmann/json et GoogleTest, fixées par étiquette dans engine/CMakeLists.txt.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/noms.sh
. "$REPO/scripts/noms.sh"

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; }
die() { printf 'erreur : %s\n' "$*" >&2; exit 1; }

version=""
run_tests=1
for arg in "$@"; do
    case "$arg" in
        --no-test) run_tests=0 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "option inconnue : $arg" ;;
        *) [ -z "$version" ] || die "une seule version attendue"; version="$arg" ;;
    esac
done
[ -n "$version" ] || { usage >&2; exit 2; }
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$ ]] || die "version invalide : $version (attendu : 0.2.0)"

BASE_IMAGE="${BASE_IMAGE:-ubuntu:24.04}"
DOCKER="${DOCKER:-docker}"
DIST_DIR="${DIST_DIR:-$REPO/dist}"
ARCH="x86_64"
NAME="$NAME_PREFIX-engine-$version"
ARCHIVE="$NAME-$ARCH.tar.zst"
BUILDER_TAG="$NAME_PREFIX-engine-builder:$(printf '%s' "$BASE_IMAGE" | tr -c 'A-Za-z0-9_.-' '_' | cut -c1-100)"

command -v "$DOCKER" >/dev/null || die "$DOCKER introuvable"
"$DOCKER" info >/dev/null 2>&1 || die "le démon Docker ne répond pas (DOCKER_HOST=${DOCKER_HOST:-défaut})"

# --- source -----------------------------------------------------------------

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"

if [ -n "$(git -C "$REPO" status --porcelain -- engine system scripts/noms.sh)" ]; then
    [ "${ALLOW_DIRTY:-0}" = 1 ] || die "engine/ ou system/ ont des modifications non validées (ALLOW_DIRTY=1 pour passer outre)"
    echo "attention : arbre de travail modifié, copié tel quel (archive de test, pas de publication)"
    # Fichiers suivis et nouveaux non ignorés, sans les dossiers de compilation.
    git -C "$REPO" ls-files -z --cached --others --exclude-standard -- engine system \
        | (cd "$REPO" && xargs -0 cp --parents -t "$work/src" 2>/dev/null || true)
    commit="$(git -C "$REPO" rev-parse HEAD)-dirty"
else
    git -C "$REPO" archive --format=tar HEAD engine system | tar -x -C "$work/src"
    commit="$(git -C "$REPO" rev-parse HEAD)"
fi
# Date du dernier commit qui touche ce qui part dans l'archive : un commit de
# documentation ne change pas l'archive.
epoch="$(git -C "$REPO" log -1 --format=%ct HEAD -- engine system)"

# --- image de construction --------------------------------------------------

# Pas de « docker build » : l'image est faite par « docker run » puis
# « docker commit », avec les mêmes options qu'à la compilation. seccomp est
# levé parce qu'un Docker plus ancien que l'image refuse ses appels système
# récents (apt-get échoue alors sur Ubuntu 24.04) ; le conteneur ne sert qu'à
# compiler une source connue.
DOCKER_RUN_OPTS=(--security-opt seccomp=unconfined)
packages="g++ cmake ninja-build pkg-config git ca-certificates file
libglib2.0-dev libsecret-1-dev libnotify-dev libssl-dev libavahi-client-dev
gettext patchelf zstd binutils"
echo "== Image de construction ($BASE_IMAGE → $BUILDER_TAG)"
if ! "$DOCKER" image inspect "$BUILDER_TAG" >/dev/null 2>&1; then
    "$DOCKER" pull -q "$BASE_IMAGE" >/dev/null
    cid="$("$DOCKER" create "${DOCKER_RUN_OPTS[@]}" -e DEBIAN_FRONTEND=noninteractive "$BASE_IMAGE" \
        sh -c "apt-get update -qq && apt-get install -y -qq --no-install-recommends $(echo $packages) >/dev/null && rm -rf /var/lib/apt/lists/*")"
    "$DOCKER" start -a "$cid" || { "$DOCKER" rm -f "$cid" >/dev/null; die "installation des outils de compilation échouée"; }
    "$DOCKER" commit "$cid" "$BUILDER_TAG" >/dev/null
    "$DOCKER" rm "$cid" >/dev/null
fi
base_digest="$("$DOCKER" image inspect --format '{{index .RepoDigests 0}}' "$BASE_IMAGE" 2>/dev/null || echo "$BASE_IMAGE")"
echo "base : $base_digest"

# --- compilation et empaquetage, dans le conteneur ----------------------------

cat > "$work/inside.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
: "${VERSION:?}" "${NAME:?}" "${ARCHIVE:?}" "${SOURCE_DATE_EPOCH:?}" "${RUN_TESTS:?}" "${PREFIX:?}"
export TZ=UTC

cmake -S /src/engine -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DTETHER_VERSION="$VERSION" \
      -DLANDLINE_PORTABLE=ON \
      -DTETHER_BUILD_EXTENSIONS=OFF \
      -DTETHER_CAMERA=OFF \
      -DBUILD_TESTING="$([ "$RUN_TESTS" = 1 ] && echo ON || echo OFF)" >/dev/null
cmake --build /build

if [ "$RUN_TESTS" = 1 ]; then
    sandbox="$(mktemp -d)"
    env HOME="$sandbox" XDG_CONFIG_HOME="$sandbox/config" XDG_STATE_HOME="$sandbox/state" \
        XDG_DATA_HOME="$sandbox/data" XDG_CACHE_HOME="$sandbox/cache" XDG_RUNTIME_DIR="$sandbox/run" \
        LANDLINE_NO_AUTOSTART=1 /build/test/tether_test --gtest_brief=1
fi

stage="/stage/$NAME"
eng="$stage/engine"
mkdir -p "$eng/lib" "$stage/system"
for bin in "${PREFIX}d" "${PREFIX}ctl" "$PREFIX-le-params"; do
    install -m 0755 "/build/$bin" "$eng/$bin"
    strip --strip-unneeded "$eng/$bin"
done
cp -r /build/locale "$eng/locale"
install -m 0644 /src/engine/LICENSE "$eng/LICENSE"
printf '%s\n' "$VERSION" > "$eng/VERSION"
for f in "$PREFIX-setup" io.github.d101c.landline.policy "$PREFIX-engine.service" "obex-$PREFIX.conf"; do
    mode=0644; [ "$f" = "$PREFIX-setup" ] && mode=0755
    install -m "$mode" "/src/system/$f" "$stage/system/$f"
done

# Bibliothèques. Restent prises au système : la glibc et ce que tout bureau
# GNOME porte (GLib/GIO, libsecret, libnotify, D-Bus, systemd). Le reste
# (aujourd'hui : Avahi, pour la partie réseau facultative) est copié dans
# engine/lib/, trouvé par le RPATH $ORIGIN/lib des binaires. Seules les
# dépendances directes sont regardées : celles des bibliothèques du système
# sont l'affaire de la distribution.
universal='^(linux-vdso|ld-linux-x86-64|libc|libm|libpthread|libdl|librt|libglib-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0|libsecret-1|libnotify|libdbus-1|libsystemd)\.so'
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
resolve() { ldconfig -p | awk -v n="$1" '$1 == n && /x86-64/ { print $NF; exit }'; }
queue=("$eng/${PREFIX}d" "$eng/${PREFIX}ctl" "$eng/$PREFIX-le-params")
declare -A seen=()
while [ "${#queue[@]}" -gt 0 ]; do
    file="${queue[0]}"; queue=("${queue[@]:1}")
    for lib in $(needed "$file"); do
        case "$lib" in libstdc++*|libgcc_s*|libssl*|libcrypto*) echo "erreur : $file dépend encore de $lib (devrait être lié statiquement)" >&2; exit 1 ;; esac
        [[ "$lib" =~ $universal ]] && continue
        [ -n "${seen[$lib]:-}" ] && continue
        seen[$lib]=1
        src="$(resolve "$lib")"
        [ -n "$src" ] || { echo "erreur : $lib introuvable dans le conteneur" >&2; exit 1; }
        cp -L "$src" "$eng/lib/$lib"
        chmod 0644 "$eng/lib/$lib"
        strip --strip-unneeded "$eng/lib/$lib"
        patchelf --set-rpath '$ORIGIN' "$eng/lib/$lib"
        echo "embarquée : $lib ($(dpkg -S "$(readlink -f "$src")" 2>/dev/null | cut -d: -f1 | head -n 1))"
        queue+=("$eng/lib/$lib")
    done
done

echo "== glibc exigée"
for f in "$eng"/* "$eng"/lib/*; do
    [ -f "$f" ] && file -b "$f" | grep -q ELF || continue
    printf '%s : ' "${f#$stage/}"
    objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1
done

# Archive reproductible : ordre, dates, propriétaires et permissions fixés.
find "$stage" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
cd /stage
tar --sort=name --format=gnu --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    --mode='u+rwX,go+rX,go-w' -cf - "$NAME" | zstd -q -19 -T1 > "/out/$ARCHIVE"
EOF

echo "== Compilation (conteneur)"
mkdir -p "$DIST_DIR"
out="$work/out"
mkdir -p "$out"
"$DOCKER" run --rm "${DOCKER_RUN_OPTS[@]}" \
    -v "$work/src:/src:ro" -v "$work/inside.sh:/inside.sh:ro" -v "$out:/out" \
    -e VERSION="$version" -e NAME="$NAME" -e ARCHIVE="$ARCHIVE" -e PREFIX="$NAME_PREFIX" \
    -e SOURCE_DATE_EPOCH="$epoch" -e RUN_TESTS="$run_tests" \
    "$BUILDER_TAG" bash /inside.sh

# --- sommes ------------------------------------------------------------------

echo "== Sommes SHA-256"
install -m 0644 "$out/$ARCHIVE" "$DIST_DIR/$ARCHIVE"
(cd "$DIST_DIR" && sha256sum "$ARCHIVE" > "$ARCHIVE.sha256")
extract="$work/extract"
mkdir -p "$extract"
zstd -dc "$DIST_DIR/$ARCHIVE" | tar -x -C "$extract"
{
    cat "$DIST_DIR/$ARCHIVE.sha256"
    (cd "$extract" && sha256sum "$NAME/system/$SETUP_NAME" "$NAME/engine/$NAME_PREFIX-le-params")
} > "$DIST_DIR/SHA256SUMS"

echo
echo "commit : $commit"
echo "base   : $base_digest"
echo "taille : $(du -h "$DIST_DIR/$ARCHIVE" | cut -f1) ($(stat -c %s "$DIST_DIR/$ARCHIVE") octets)"
cat "$DIST_DIR/SHA256SUMS"
