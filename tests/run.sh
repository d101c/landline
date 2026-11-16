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
run "client du moteur contre le faux moteur" tests/client.test.js
run "assistant système vu de l'extension" tests/setupHelper.test.js
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
