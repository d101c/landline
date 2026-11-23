// SPDX-License-Identifier: GPL-2.0-or-later
// Icônes des applis de l'iPhone : gjs -m tests/appIcons.test.js
// Un serveur HTTP local imite la recherche de l'App Store et son serveur
// d'images ; le cache est dans un dossier temporaire. Aucune requête ne sort
// de la machine.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Soup from 'gi://Soup?version=3.0';
import {
    AppIcons, MAX_BYTES, MAX_LOOKUP_BYTES, RETRY_MS, artworkUrl, countryFromLocale, desktopCandidates, fromAppStore, iconFileName,
} from '../extension/lib/appIcons.js';

let failed = 0;
let count = 0;
function check(label, got, want) {
    count++;
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) {
        print(`ok   ${label}`);
    } else {
        failed++;
        print(`FAIL ${label}\n     got  ${a}\n     want ${b}`);
    }
}
const sleep = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));
async function until(test, ms = 5000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (test())
            return true;
        await sleep(10);
    }
    return false;
}

// --- règles pures
check('pays : la région de la locale', countryFromLocale(['fr_CH.UTF-8', 'fr_CH', 'fr', 'C']), 'ch');
check('pays : la première liste qui en a une', countryFromLocale(['en', 'C'], ['de_DE@euro']), 'de');
check('pays : aucun, les États-Unis', countryFromLocale(['C', 'POSIX'], []), 'us');
check('nom de fichier : l\'identifiant', iconFileName('net.whatsapp.WhatsApp'), 'net.whatsapp.WhatsApp');
check('nom de fichier : rien de douteux',
    ['../x', 'a/b', '', 'unknown', '.cache', 'a b', null, 'x'.repeat(201)].map(iconFileName),
    [null, null, null, null, null, null, null, null]);
check('adresse de l\'image : 128 px en PNG',
    artworkUrl({artworkUrl512: 'https://is1-ssl.mzstatic.com/image/thumb/a/b/AppIcon.png/512x512bb.jpg'}),
    'https://is1-ssl.mzstatic.com/image/thumb/a/b/AppIcon.png/128x128bb.png');
check('adresse de l\'image : aucune', artworkUrl({}), null);
check('seulement le serveur d\'images d\'Apple, en HTTPS',
    ['https://is1-ssl.mzstatic.com/x.png', 'http://is1-ssl.mzstatic.com/x.png', 'https://evil.example/mzstatic.com.png',
        'https://mzstatic.com.evil.example/x.png'].map(fromAppStore),
    [true, false, false, false]);
check('applis du bureau connues', [desktopCandidates('org.whispersystems.signal')[0],
    desktopCandidates('ph.telegra.Telegraph')[0], desktopCandidates('com.microsoft.skype.teams')[0],
    desktopCandidates('com.hammerandchisel.discord')[0], desktopCandidates('x.y')],
['signal-desktop', 'org.telegram.desktop', 'com.microsoft.Teams', 'com.discordapp.Discord', []]);

// --- le faux App Store
const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 1, 2, 3]);
const requests = [];
let delay = 0;
let lookupStatus = 200;
const server = new Soup.Server();
server.add_handler(null, (srv, msg, path, query) => {
    requests.push(path + (query ? `?${Object.entries(query).map(([k, v]) => `${k}=${v}`).sort().join('&')}` : ''));
    msg.pause();
    const answer = () => {
        if (path === '/lookup') {
            const id = query?.bundleId;
            const known = ['net.whatsapp.WhatsApp', 'com.microsoft.skype.teams', 'com.exemple.PasUneImage',
                'com.exemple.Redirigee', 'com.exemple.TropGrande', 'com.exemple.SansLongueur', 'com.exemple.RechercheGeante'];
            const results = known.includes(id)
                ? [{bundleId: id, artworkUrl512: `${base}/image/${id}/512x512bb.jpg`}] : [];
            // Une réponse de recherche démesurée : du texte en plus, après le résultat.
            if (id === 'com.exemple.RechercheGeante')
                results[0].description = 'x'.repeat(MAX_LOOKUP_BYTES);
            msg.set_status(lookupStatus, null);
            msg.set_response('application/json', Soup.MemoryUse.COPY,
                new TextEncoder().encode(JSON.stringify({resultCount: results.length, results})));
        } else if (path.startsWith('/image/com.exemple.Redirigee/')) {
            // Renvoie vers un autre nom d'hôte, que la vérification n'a jamais vu.
            msg.set_redirect(302, `${other}/image/cible-redirection/128x128bb.png`);
        } else if (path.startsWith('/image/com.exemple.TropGrande/')) {
            const big = new Uint8Array(MAX_BYTES + 1);
            big.set(PNG);
            msg.set_status(200, null);
            msg.set_response('image/png', Soup.MemoryUse.COPY, big);
        } else if (path.startsWith('/image/com.exemple.SansLongueur/')) {
            // Sans Content-Length (par morceaux) : arrêté à la lecture.
            msg.set_status(200, null);
            msg.get_response_headers().set_content_type('image/png', null);
            msg.get_response_headers().set_encoding(Soup.Encoding.CHUNKED);
            const body = msg.get_response_body();
            const piece = new Uint8Array(64 * 1024);
            piece.set(PNG);
            for (let i = 0; i < 12; i++)
                body.append_bytes(new GLib.Bytes(piece));
            body.complete();
        } else if (path.startsWith('/image/com.exemple.PasUneImage/')) {
            msg.set_status(200, null);
            msg.set_response('text/html', Soup.MemoryUse.COPY, new TextEncoder().encode('<html>'));
        } else if (path.startsWith('/image/') && path.endsWith('/128x128bb.png')) {
            msg.set_status(200, null);
            msg.set_response('image/png', Soup.MemoryUse.COPY, PNG);
        } else {
            msg.set_status(404, null);
        }
        msg.unpause();
    };
    if (delay)
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, delay, () => {
            answer();
            return GLib.SOURCE_REMOVE;
        });
    else
        answer();
});
server.listen_local(0, Soup.ServerListenOptions.IPV4_ONLY);
const base = server.get_uris()[0].to_string().replace(/\/$/, '');
// Le même serveur sous un autre nom : « un autre domaine » pour la redirection.
const other = base.replace('127.0.0.1', 'localhost');

const tmp = GLib.dir_make_tmp('landline-icons-XXXXXX');
const dir = `${tmp}/cache/landline/app-icons`;
let clock = Date.now();
let changes = 0;
const make = (extra = {}) => new AppIcons({
    dir,
    country: 'ch',
    lookupUrl: `${base}/lookup`,
    artworkAllowed: url => url.startsWith(base),
    now: () => clock,
    onChange: () => changes++,
    ...extra,
});
const exists = name => GLib.file_test(`${dir}/${name}`, GLib.FileTest.EXISTS);
const lookups = id => requests.filter(r => r.startsWith('/lookup') && r.includes(`bundleId=${id}`)).length;

async function main() {
    // Réglage éteint : aucune requête, rien sur le disque.
    let icons = make({enabled: false});
    check('éteint : pas d\'icône', icons.icon('net.whatsapp.WhatsApp'), null);
    await sleep(200);
    check('éteint : aucune requête, aucun fichier', [requests.length, exists('')], [0, false]);
    icons.destroy();

    // Première fois : une seule requête par appli, même demandée trois fois.
    icons = make();
    delay = 150;
    const first = [icons.icon('net.whatsapp.WhatsApp'), icons.icon('net.whatsapp.WhatsApp'),
        icons.icon('net.whatsapp.WhatsApp')];
    check('pas encore là : rien, en attendant', first, [null, null, null]);
    await until(() => changes > 0);
    delay = 0;
    check('arrivée : onChange une fois', changes, 1);
    check('dédoublonnage : une recherche, une image', [lookups('net.whatsapp.WhatsApp'),
        requests.filter(r => r.startsWith('/image/net.whatsapp.WhatsApp/')).length], [1, 1]);
    check('la recherche porte le pays', requests[0], '/lookup?bundleId=net.whatsapp.WhatsApp&country=ch');
    check('l\'image demandée : 128 px en PNG', requests[1], '/image/net.whatsapp.WhatsApp/128x128bb.png');
    check('le fichier du cache', icons.icon('net.whatsapp.WhatsApp'), {file: `${dir}/net.whatsapp.WhatsApp.png`});
    const [, contents] = GLib.file_get_contents(`${dir}/net.whatsapp.WhatsApp.png`);
    check('son contenu', [...contents], [...PNG]);
    icons.destroy();

    // Une autre session : le cache suffit, aucune requête.
    let before = requests.length;
    icons = make();
    icons.icon('net.whatsapp.WhatsApp');
    await until(() => icons.icon('net.whatsapp.WhatsApp') !== null);
    check('du cache : sans requête', [icons.icon('net.whatsapp.WhatsApp'), requests.length - before],
        [{file: `${dir}/net.whatsapp.WhatsApp.png`}, 0]);

    // Une appli que l'App Store ne connaît pas : retenue, pas redemandée.
    icons.icon('ch.exemple.Inconnue');
    await until(() => exists('ch.exemple.Inconnue.none'));
    await sleep(50);
    check('inconnue : retenue sur le disque', [exists('ch.exemple.Inconnue.none'), exists('ch.exemple.Inconnue.png')], [true, false]);
    for (let i = 0; i < 5; i++)
        icons.icon('ch.exemple.Inconnue');
    await sleep(100);
    check('inconnue : pas redemandée dans la session', lookups('ch.exemple.Inconnue'), 1);
    icons.destroy();
    icons = make();
    icons.icon('ch.exemple.Inconnue');
    await sleep(150);
    check('inconnue : ni dans une autre session le même jour', lookups('ch.exemple.Inconnue'), 1);
    clock += RETRY_MS + 1000;
    icons.destroy();
    icons = make();
    icons.icon('ch.exemple.Inconnue');
    await until(() => lookups('ch.exemple.Inconnue') === 2);
    check('inconnue : redemandée un jour plus tard', lookups('ch.exemple.Inconnue'), 2);

    // Une réponse qui n'est pas une image PNG.
    icons.icon('com.exemple.PasUneImage');
    await until(() => exists('com.exemple.PasUneImage.none'));
    check('pas une image : rien gardé', [exists('com.exemple.PasUneImage.png'), exists('com.exemple.PasUneImage.none')], [false, true]);

    // Le serveur en panne : en mémoire seulement, pas avant un jour.
    lookupStatus = 503;
    icons.icon('com.microsoft.skype.teams');
    await sleep(200);
    icons.icon('com.microsoft.skype.teams');
    await sleep(100);
    check('serveur en panne : une requête, rien sur le disque', [lookups('com.microsoft.skype.teams'),
        exists('com.microsoft.skype.teams.none')], [1, false]);
    lookupStatus = 200;
    clock += RETRY_MS + 1000;
    changes = 0;
    icons.icon('com.microsoft.skype.teams');
    await until(() => changes > 0);
    check('un jour plus tard : réessayée, trouvée', [lookups('com.microsoft.skype.teams'),
        icons.icon('com.microsoft.skype.teams')], [2, {file: `${dir}/com.microsoft.skype.teams.png`}]);

    // Éteint en cours de route : plus rien de l'App Store, rien demandé.
    changes = 0;
    icons.setEnabled(false);
    check('éteint : onChange, plus l\'icône du cache', [changes, icons.icon('net.whatsapp.WhatsApp')], [1, null]);
    before = requests.length;
    icons.icon('org.whispersystems.signal');
    await sleep(100);
    check('éteint : aucune requête', requests.length - before, 0);

    // L'appli installée sur l'ordinateur, réglage éteint ou non.
    icons.destroy();
    const asked = [];
    icons = make({
        enabled: false,
        desktopIcon: id => {
            asked.push(id);
            return id === 'org.signal.Signal' ? new Gio.ThemedIcon({name: 'org.signal.Signal'}) : null;
        },
    });
    const signal = icons.icon('org.whispersystems.signal');
    check('repli : l\'icône de l\'appli du bureau', [signal?.gicon?.to_string(), asked],
        ['org.signal.Signal', ['signal-desktop', 'org.signal.Signal']]);
    icons.icon('org.whispersystems.signal');
    check('repli : cherché une fois', asked.length, 2);
    check('repli : rien pour une appli inconnue du tableau', icons.icon('ch.exemple.Agenda'), null);

    // Annulé à la destruction : rien n'arrive après.
    icons.destroy();
    icons = make();
    delay = 200;
    changes = 0;
    icons.icon('com.exemple.Tardive');
    await sleep(50);
    icons.destroy();
    await sleep(400);
    delay = 0;
    check('détruit en cours de route : pas de rappel, pas de fichier', [changes, exists('com.exemple.Tardive.none')], [0, false]);

    // #55 : une redirection n'est jamais suivie, une réponse trop grande jamais gardée.
    icons.destroy();
    icons = make();
    for (const id of ['com.exemple.Redirigee', 'com.exemple.TropGrande', 'com.exemple.SansLongueur',
        'com.exemple.RechercheGeante'])
        icons.icon(id);
    await until(() => ['com.exemple.Redirigee', 'com.exemple.TropGrande', 'com.exemple.SansLongueur',
        'com.exemple.RechercheGeante'].every(id => exists(`${id}.none`)));
    check('redirection vers un autre domaine : pas suivie, rien gardé',
        [requests.filter(r => r.startsWith('/image/cible-redirection/')).length, exists('com.exemple.Redirigee.png'),
            exists('com.exemple.Redirigee.none')], [0, false, true]);
    check('image plus grande que MAX_BYTES (Content-Length) : refusée',
        [exists('com.exemple.TropGrande.png'), exists('com.exemple.TropGrande.none')], [false, true]);
    check('image sans Content-Length, trop longue : refusée à la lecture',
        [exists('com.exemple.SansLongueur.png'), exists('com.exemple.SansLongueur.none')], [false, true]);
    check('réponse de recherche démesurée : refusée, l\'image jamais demandée',
        [exists('com.exemple.RechercheGeante.none'),
            requests.filter(r => r.startsWith('/image/com.exemple.RechercheGeante/')).length], [true, 0]);

    // #55 : une appli installée après coup prête son icône (AppInfoMonitor).
    icons.destroy();
    let installed = false;
    changes = 0;
    icons = make({
        enabled: false,
        desktopIcon: id => installed && id === 'signal-desktop' ? new Gio.ThemedIcon({name: 'signal-desktop'}) : null,
    });
    check('appli du bureau absente : les initiales', icons.icon('org.whispersystems.signal'), null);
    installed = true;
    check('...installée, pas encore signalée : le cache tient', icons.icon('org.whispersystems.signal'), null);
    icons.appsChanged();
    check('...signalée par le moniteur : onChange, puis son icône',
        [changes, icons.icon('org.whispersystems.signal')?.gicon?.to_string()], [1, 'signal-desktop']);
    icons.destroy();

    server.disconnect();
    GLib.spawn_command_line_sync(`rm -rf ${GLib.shell_quote(tmp)}`);
}

const loop = new GLib.MainLoop(null, false);
main().catch(e => {
    failed++;
    print(`FAIL ${e}\n${e.stack}`);
}).finally(() => loop.quit());
loop.run();
print(`${count - failed}/${count} checks passed`);
if (failed)
    imports.system.exit(1);
