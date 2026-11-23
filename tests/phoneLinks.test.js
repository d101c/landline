// SPDX-License-Identifier: GPL-2.0-or-later
// « Use Landline for phone links » (#50) pour de vrai, mais dans un HOME et
// des dossiers XDG temporaires : tests/run.sh le lance avec
//   HOME=… XDG_DATA_HOME=… XDG_CONFIG_HOME=… XDG_DATA_DIRS=…
// et il refuse de tourner ailleurs que dans /tmp (les gestionnaires par défaut
// de l'utilisateur ne sont jamais touchés).
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';

import {phoneLinksState, stopPhoneLinks, usePhoneLinks, handlerFile} from '../extension/lib/phoneLinks.js';
import {DESKTOP_ID} from '../extension/lib/telLinks.js';

for (const name of ['HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_DIRS']) {
    if (!(GLib.getenv(name) ?? '').startsWith('/tmp/')) {
        print(`FAIL ${name} n'est pas dans /tmp : essai refusé (lancer par tests/run.sh)`);
        imports.system.exit(1);
    }
}

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

// The keys of Phone Settings, in memory.
const keys = {'phone-links': false, 'phone-links-previous': []};
const settings = {
    get_boolean: k => keys[k],
    set_boolean: (k, v) => (keys[k] = v),
    get_strv: k => [...keys[k]],
    set_strv: (k, v) => (keys[k] = [...v]),
};

const apps = GLib.build_filenamev([GLib.get_user_data_dir(), 'applications']);
// An app of "the system" for callto:, one the user chose for tel:.
const system = GLib.build_filenamev([GLib.getenv('XDG_DATA_DIRS').split(':')[0], 'applications']);
GLib.mkdir_with_parents(apps, 0o755);
GLib.mkdir_with_parents(system, 0o755);
const app = (dir, id, types) => GLib.file_set_contents(GLib.build_filenamev([dir, id]),
    `[Desktop Entry]\nType=Application\nName=${id}\nExec=true %u\nMimeType=${types}\n`);
app(apps, 'org.example.Phone.desktop', 'x-scheme-handler/tel;');
app(system, 'org.example.Meetings.desktop', 'x-scheme-handler/callto;');
GLib.file_set_contents(GLib.build_filenamev([system, 'mimeapps.list']),
    '[Default Applications]\nx-scheme-handler/callto=org.example.Meetings.desktop\n');

const defaultOf = scheme => Gio.AppInfo.get_default_for_type(`x-scheme-handler/${scheme}`, false)?.get_id() ?? null;
const exists = () => handlerFile().query_exists(null);
const pause = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));

const loop = new GLib.MainLoop(null, false);
(async () => {
    await pause(300);
    GioUnix.DesktopAppInfo.new('org.example.Phone.desktop').set_as_default_for_type('x-scheme-handler/tel');
    let state = await phoneLinksState();
    check('before: tel: is the user\'s app, callto: the system\'s; Landline is neither',
        [state.ours, state.others.map(o => `${o.scheme}=${o.id}`)],
        [false, ['tel=org.example.Phone.desktop', 'callto=org.example.Meetings.desktop']]);

    // #56: callto: cannot be taken after tel: was: everything is undone.
    let thrown = null;
    try {
        await usePhoneLinks(settings, {name: 'Landline', comment: ''}, (info, contentType) => {
            if (contentType === 'x-scheme-handler/callto')
                throw new Error('refused for the test');
            info.set_as_default_for_type(contentType);
        });
    } catch (e) {
        thrown = e.message;
    }
    await pause(300);
    check('callto: refused after tel:: the error comes back', thrown, 'refused for the test');
    check('...no file, tel: given back to the user\'s app, callto: as it was',
        [exists(), defaultOf('tel'), defaultOf('callto')], [false, 'org.example.Phone.desktop', 'org.example.Meetings.desktop']);
    check('...the option off, nothing remembered', [keys['phone-links'], keys['phone-links-previous']], [false, []]);

    await usePhoneLinks(settings, {name: 'Landline', comment: 'Call phone numbers through your iPhone'});
    state = await phoneLinksState();
    check('on: the file is written, default for both', [exists(), state.ours, defaultOf('tel'), defaultOf('callto')],
        [true, true, DESKTOP_ID, DESKTOP_ID]);
    check('...what was there is remembered, the extension told', [keys['phone-links-previous'], keys['phone-links']],
        [['tel=org.example.Phone.desktop', 'callto=org.example.Meetings.desktop'], true]);
    const [, bytes] = GLib.file_get_contents(handlerFile().get_path());
    check('...the file says it is Landline\'s', new TextDecoder().decode(bytes).includes('X-Landline-Phone-Links=true'), true);

    await usePhoneLinks(settings, {name: 'Landline', comment: 'again'});
    check('turned on twice: the first defaults are still the ones to give back', keys['phone-links-previous'],
        ['tel=org.example.Phone.desktop', 'callto=org.example.Meetings.desktop']);

    await stopPhoneLinks(settings);
    await pause(300);
    check('off: the file is gone, each default given back', [exists(), defaultOf('tel'), defaultOf('callto')],
        [false, 'org.example.Phone.desktop', 'org.example.Meetings.desktop']);
    const [, list] = GLib.file_get_contents(GLib.build_filenamev([GLib.get_user_config_dir(), 'mimeapps.list']));
    const text = new TextDecoder().decode(list);
    check('...the default of the system is not written as the user\'s, Landline is nowhere',
        [text.includes('callto'), text.includes(DESKTOP_ID)], [false, false]);
    check('...the keys emptied', [keys['phone-links'], keys['phone-links-previous']], [false, []]);

    // The user picks another app for tel: while Landline has it.
    await usePhoneLinks(settings, {name: 'Landline', comment: ''});
    app(apps, 'org.example.Other.desktop', 'x-scheme-handler/tel;');
    await pause(300);
    GioUnix.DesktopAppInfo.new('org.example.Other.desktop').set_as_default_for_type('x-scheme-handler/tel');
    state = await phoneLinksState();
    check('another app chosen for tel: since: the switch shows off', state.ours, false);
    await stopPhoneLinks(settings);
    await pause(300);
    check('...turning off leaves that choice alone, gives callto: back', [exists(), defaultOf('tel'), defaultOf('callto')],
        [false, 'org.example.Other.desktop', 'org.example.Meetings.desktop']);

    // Nothing opened phone links before.
    GLib.unlink(GLib.build_filenamev([apps, 'org.example.Other.desktop']));
    GLib.unlink(GLib.build_filenamev([apps, 'org.example.Phone.desktop']));
    for (const scheme of ['tel', 'callto'])
        Gio.AppInfo.reset_type_associations(`x-scheme-handler/${scheme}`);
    await pause(300);
    await usePhoneLinks(settings, {name: 'Landline', comment: ''});
    check('no app before for tel:: only callto: remembered', keys['phone-links-previous'],
        ['callto=org.example.Meetings.desktop']);
    await stopPhoneLinks(settings);
    await pause(300);
    check('off: tel: without a default again', [exists(), defaultOf('tel')], [false, null]);
    loop.quit();
})().catch(e => {
    check(`phone links: ${e.message}`, false, true);
    loop.quit();
});
loop.run();

print(`\n${count - failed}/${count} checks passed`);
if (failed > 0)
    imports.system.exit(1);
