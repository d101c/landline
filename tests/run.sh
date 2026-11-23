#!/usr/bin/env bash
# Tous les tests : tests/run.sh
set -u
cd "$(dirname "$0")/.."
status=0
run() {
    echo "== $1"
    out=$(gjs -m "$2" 2>&1) || status=1
    echo "$out" | grep -v '^ok'
}
run "modèle d'état (règles pures)" tests/model.test.js
run "applis officielles et liens tel: (règles pures)" tests/apps.test.js
# Les gestionnaires de liens tel: pour de vrai, dans un HOME et des dossiers XDG
# temporaires : ceux de l'utilisateur ne sont jamais touchés.
liens=$(mktemp -d /tmp/landline-liens.XXXX)
echo "== liens tel: dans un dossier temporaire"
out=$(env HOME="$liens" XDG_DATA_HOME="$liens/data" XDG_CONFIG_HOME="$liens/config" XDG_CACHE_HOME="$liens/cache" \
    XDG_DATA_DIRS="$liens/sys" gjs -m tests/phoneLinks.test.js 2>&1) || status=1
echo "$out" | grep -v '^ok'
rm -rf "$liens"
run "client du moteur contre le faux moteur" tests/client.test.js
run "icônes des applis contre un faux App Store local" tests/appIcons.test.js
run "assistant système vu de l'extension" tests/setupHelper.test.js
run "installation du moteur (règles pures)" tests/engineInstall.test.js
run "installation du moteur dans un dossier temporaire" tests/engineInstaller.test.js
run "vraie archive du moteur (dist/ ou \$LANDLINE_DIST), dossier temporaire" tests/engineRelease.test.js
echo "== fiche"
# La description publiée est celle de metadata.json : elle doit rester celle
# qui est rédigée et relue dans docs/ego/description.txt.
if python3 -c 'import json,sys; sys.exit(json.load(open("extension/metadata.json"))["description"] != open("docs/ego/description.txt").read().strip())'; then
    echo "description de metadata.json identique à docs/ego/description.txt"
else
    echo "ÉCHEC : la description de metadata.json diffère de docs/ego/description.txt"
    status=1
fi
exit $status
