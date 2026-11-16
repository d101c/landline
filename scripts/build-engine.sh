#!/usr/bin/env bash
# Compile le moteur (engine/) et lance la suite de tests unitaires.
# N'installe rien : réutilise la chaîne locale du banc d'essai
# (cmake + ninja dans un venv, en-têtes de développement extraits dans un sysroot).
#
#   scripts/build-engine.sh            compile puis lance les tests unitaires
#   scripts/build-engine.sh --no-test  compile seulement
#   scripts/build-engine.sh --clean    efface engine/build/local avant de compiler
#
# Variables :
#   BENCH       dossier du banc (défaut : ~/DEVELOPEMENT/iphone-link-study/bench)
#   BUILD_DIR   dossier de compilation (défaut : engine/build/local)
#   BUILD_TYPE  Release (défaut) ou Debug
#
# Le test « headless » (tether_headless_test) n'est JAMAIS lancé ici : il démarre
# un vrai démon, qui touche au Bluetooth.
#
# Attention : landlined ne lit pas ses arguments. « landlined --help » ou
# « landlined --version » démarrent le démon. Pour vérifier un binaire sans rien
# lancer : « landlinectl --version », ou ldd.
#
# Les interfaces d'origine de Tether (GTK, terminal, fenêtre d'appairage) ne
# sont pas compilées (LANDLINE_UPSTREAM_FRONTENDS=OFF), ni la caméra, qui ne
# sert qu'avec l'application iOS de Tether (TETHER_CAMERA=OFF : pas de
# dépendance à libavcodec ni à libpipewire). Les cibles CMake et le binaire de
# tests gardent leur nom d'origine (tether_test).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${BENCH:-$HOME/DEVELOPEMENT/iphone-link-study/bench}"
BUILD_DIR="${BUILD_DIR:-$ROOT/engine/build/local}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
TOOLS="$BENCH/_tools/venv/bin"
LIBS="$BENCH/_sysroot/root/usr/lib/x86_64-linux-gnu"

run_tests=1
for arg in "$@"; do
    case "$arg" in
        --no-test) run_tests=0 ;;
        --clean)   rm -rf "$BUILD_DIR" ;;
        -h|--help) sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "option inconnue : $arg" >&2; exit 2 ;;
    esac
done

# Chaîne locale si elle existe, sinon outils du système.
if [ -x "$TOOLS/cmake" ]; then
    export PATH="$TOOLS:$PATH"
fi
command -v cmake >/dev/null || { echo "cmake introuvable (ni dans $TOOLS ni dans le PATH)" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja introuvable (ni dans $TOOLS ni dans le PATH)" >&2; exit 1; }

extra=()
if [ -d "$LIBS/pkgconfig" ]; then
    export PKG_CONFIG_PATH="$LIBS/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LIBRARY_PATH="$LIBS${LIBRARY_PATH:+:$LIBRARY_PATH}"
    extra+=("-DCMAKE_BUILD_RPATH=$LIBS")
fi

cmake -S "$ROOT/engine" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DTETHER_BUILD_EXTENSIONS=OFF \
      -DTETHER_CAMERA=OFF \
      "${extra[@]}"
cmake --build "$BUILD_DIR"

echo
echo "Binaires : $BUILD_DIR"

if [ "$run_tests" = 1 ]; then
    # HOME et répertoires XDG isolés : les tests ne touchent ni à la configuration
    # ni à l'état réels de l'utilisateur. LANDLINE_NO_AUTOSTART interdit au client de
    # test de lancer un démon s'il ne trouve pas de socket.
    sandbox="$(mktemp -d)"
    trap 'rm -rf "$sandbox"' EXIT
    echo "Tests unitaires (HOME isolé : $sandbox)"
    env HOME="$sandbox" \
        XDG_CONFIG_HOME="$sandbox/config" XDG_STATE_HOME="$sandbox/state" \
        XDG_DATA_HOME="$sandbox/data" XDG_CACHE_HOME="$sandbox/cache" \
        XDG_RUNTIME_DIR="$sandbox/run" \
        LANDLINE_NO_AUTOSTART=1 \
        "$BUILD_DIR/test/tether_test"
fi
