// SPDX-License-Identifier: GPL-2.0-or-later
// Les applis officielles de l'ordinateur (#49) et les liens tel: (#50), règles
// pures, sans Shell : gjs -m tests/apps.test.js
import GLib from 'gi://GLib';

import {AppFinder, OFFICIAL_APPS, appRoute, conversationLink, desktopIds, mediaKind, schemes} from '../extension/lib/officialApps.js';
import {
    DESKTOP_ID, INTERFACE, MAX_LINK, OBJECT_PATH, PHONE_LINK_BURST, PHONE_LINK_WINDOW_MS, PhoneLinkGate, desktopEntry,
    phoneLinkNumber,
} from '../extension/lib/telLinks.js';

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

// --- what a notification stands for
for (const [body, kind] of [
    ['🎤 Voice message', 'voice'], ['Voice message', 'voice'], ['🎤 Voice message (0:12)', 'voice'],
    ['Message vocal', 'voice'], ['🎤 Message vocal', 'voice'], ['Audio', 'voice'], ['🎙 0:07', 'voice'],
    ['📷 Photo', 'photo'], ['Photo', 'photo'], ['📷 Regarde ça', 'photo'], ['Image', 'photo'],
    ['🎥 Video', 'video'], ['Vidéo', 'video'], ['Video message', 'video'], ['Message vidéo', 'video'],
    ['📄 facture.pdf', 'file'], ['Attachment', 'file'], ['Pièce jointe', 'file'], ['Fichier', 'file'],
    ['Sticker', 'sticker'], ['GIF', 'sticker'], ['Autocollant', 'sticker'],
    ['Camille: 🎤 Voice message', 'voice'], ['Léo: Photo', 'photo'],
    ['Camille sent you a voice message.', 'voice'], ['Léo a envoyé une photo', 'photo'],
    ['Photo de vacances demain ?', null], ['Audio du concert ce soir', null], ['Salut', null], ['', null],
    ['Video call at 3?', null], ['File it under taxes', null],
]) check(`media: ${JSON.stringify(body)}`, mediaKind(body), kind);

// --- routes
const none = {apps: {}, handlers: {https: 'browser.desktop'}, web: true};
check('every service has a name and its iOS bundles', Object.values(OFFICIAL_APPS).every(e => e.name && e.bundles.length > 0), true);
check('schemes asked about', schemes(), ['sgnl', 'tg', 'msteams', 'discord', 'slack', 'https']);
check('Signal: nothing installed, no web client: missing, Signal Desktop suggested',
    [appRoute('org.whispersystems.signal', none).missing, appRoute('org.whispersystems.signal', none).get.name], [true, 'Signal Desktop']);
check('not a listed service: null', appRoute('com.apple.mobilemail', none), null);
check('Flatpak Signal found by its id, without a handler',
    appRoute('org.whispersystems.signal', {apps: {'org.signal.Signal.desktop': 'Signal'}, handlers: {}, web: true}),
    {service: 'Signal', key: 'signal', app: 'org.signal.Signal.desktop', links: false, web: false, home: null, webName: null,
        missing: false, get: null, via: 'Signal'});
// #54: never a third-party wrapper of WhatsApp, even installed and handling
// whatsapp: links: WhatsApp Web, on the conversation.
const wrappers = {apps: {'com.rtosta.zapzap.desktop': 'ZapZap', 'com.github.xeco23.WasIstLos.desktop': 'WasIstLos'},
    handlers: {whatsapp: 'com.rtosta.zapzap.desktop', https: 'b'}, web: true};
const zapRoute = appRoute('net.whatsapp.WhatsApp', wrappers);
check('ZapZap and WasIstLos installed: not offered, WhatsApp Web instead', [zapRoute.via, zapRoute.app, zapRoute.web],
    ['WhatsApp Web', null, true]);
check('...on the conversation in the browser', conversationLink('net.whatsapp.WhatsApp', zapRoute, '41790000001'),
    'https://web.whatsapp.com/send?phone=41790000001');
check('...the same for WhatsApp Business', appRoute('net.whatsapp.WhatsAppSMB', wrappers).via, 'WhatsApp Web');
check('no wrapper is even looked up', desktopIds().some(id => /zapzap|WasIstLos/i.test(id)), false);
const otherWhatsapp = appRoute('net.whatsapp.WhatsApp', {apps: {}, handlers: {whatsapp: 'org.example.Unofficial.desktop'}, web: true});
check('another program that opens whatsapp: links: not used, the web client instead', otherWhatsapp.via, 'WhatsApp Web');
check('no browser at all: WhatsApp missing', appRoute('net.whatsapp.WhatsApp', {apps: {}, handlers: {}, web: false}).missing, true);
check('a number that is not one: no conversation', conversationLink('org.whispersystems.signal',
    appRoute('org.whispersystems.signal', {apps: {}, handlers: {sgnl: 'signal-desktop.desktop'}, web: true}), 'toString'), null);

// --- the finder, with a fake backend
{
    const loop = new GLib.MainLoop(null, false);
    const installed = {'signal-desktop.desktop': 'Signal'};
    const defaults = {sgnl: 'signal-desktop.desktop', https: 'browser.desktop'};
    let asked = 0;
    let changes = 0;
    const finder = new AppFinder({
        lookupApp: id => installed[id] ?? null,
        defaultFor: scheme => {
            asked++;
            if (scheme === 'tg')
                return Promise.reject(new Error('Failed to find default application'));
            return Promise.resolve(defaults[scheme] ?? null);
        },
        isRunning: id => id === 'signal-desktop.desktop',
    }, () => changes++);
    check('before the first look: nothing known', finder.route('org.whispersystems.signal'), null);
    (async () => {
        await finder.refresh();
        check('after it: Signal, its links, running', [finder.route('org.whispersystems.signal').via,
            finder.route('org.whispersystems.signal').links, finder.running('org.whispersystems.signal')], ['Signal', true, true]);
        check('a scheme nobody opens is not an error', finder.route('ph.telegra.Telegraph').via, 'Telegram Web');
        check('a web client is never "running"', finder.running('ph.telegra.Telegraph'), false);
        check('one change told', changes, 1);
        await finder.refresh();
        check('the same answer again: no change told', changes, 1);
        installed['org.telegram.desktop.desktop'] = 'Telegram';
        defaults.tg = 'org.telegram.desktop.desktop';
        await finder.refresh();
        check('Telegram Desktop installed: told, and used', [changes, finder.route('ph.telegra.Telegraph').via], [2, 'Telegram']);
        // Two looks at once: the later one wins, the earlier one is dropped.
        delete installed['signal-desktop.desktop'];
        const first = finder.refresh();
        const second = finder.refresh();
        await Promise.all([first, second]);
        check('two looks at once: one change', changes, 3);
        finder.stop();
        await finder.refresh();
        check('stopped: nothing more told', changes, 3);
        check('every scheme asked each time', asked > 0, true);
        loop.quit();
    })().catch(e => {
        check(`finder: ${e.message}`, false, true);
        loop.quit();
    });
    loop.run();
}

// --- phone links
for (const [uri, number] of [
    ['tel:+41790000001', '+41790000001'],
    ['tel:+41-79-000-00-01', '+41790000001'],
    ['tel:+41 79 000 00 01', '+41790000001'],
    ['tel:+41%2079%20000%2000%2001', '+41790000001'],
    ['tel:+41 (0)79 000 00 01', '+41790000001'],
    ['tel:+1.555.010.9999', '+15550109999'],
    ['TEL:0790000001', '0790000001'],
    ['tel:0790000001;phone-context=+41', '0790000001'],
    ['tel:+41790000001;ext=12', '+41790000001'],
    ['callto:+41790000001', '+41790000001'],
    ['callto://+41790000001', '+41790000001'],
    ['tel:144', '144'],
    ['tel:12', ''],
    ['tel:*21*0790000001#', ''],
    ['tel:%2A%2306%23', ''],
    ['tel:1-800-FLOWERS', ''],
    ['callto:echo123', ''],
    ['callto://camille.martin?call', ''],
    ['sms:+41790000001', ''],
    ['tel:+4179000000100000001', ''],
    ['tel:%E0%A4%A', ''],
    [`tel:+41${'0'.repeat(300)}`, ''],
    ['', ''],
    [null, ''],
]) check(`phone link ${JSON.stringify(uri)?.slice(0, 40)}`, phoneLinkNumber(uri), number);

const entry = desktopEntry('Landline', 'Call phone numbers\nthrough your iPhone');
check('desktop file: one line per key, hidden from the app grid, both schemes', [
    entry.includes('Comment=Call phone numbers through your iPhone\n'),
    entry.includes('NoDisplay=true\n'),
    entry.includes('MimeType=x-scheme-handler/tel;x-scheme-handler/callto;\n'),
], [true, true, true]);
check('desktop file: gdbus on the object of the extension, the link as one argument',
    entry.split('\n').find(line => line.startsWith('Exec=')),
    `Exec=gdbus call --session --dest org.gnome.Shell --object-path ${OBJECT_PATH} --method ${INTERFACE}.Call %u`);
check('its id', DESKTOP_ID, 'landline-phone-links.desktop');

// --- #56: how often a link opens the card
{
    let now = 1000000;
    const gate = new PhoneLinkGate(() => now);
    check('a link: shown', gate.take('tel:+41790000001', false), 'show');
    now += 200;
    check('the same link again within a second, its card open: nothing more', gate.take('tel:+41790000001', true), 'same');
    check('...its card closed meanwhile: shown again', gate.take('tel:+41790000001', false), 'show');
    now += 200;
    check('another link within a second: shown, not dropped', gate.take('tel:+41790000002', true), 'show');
    check('a link too long: still shown (the card says it has no number)',
        gate.take(`tel:${'1'.repeat(MAX_LINK)}`, false), 'show');
    const after = [];
    for (let i = 0; i < 3; i++) {
        now += 1500;
        after.push(gate.take(`tel:+4179000001${i}`, false));
    }
    check(`more than ${PHONE_LINK_BURST} cards in a minute: held`, after, ['show', 'hold', 'hold']);
    now += PHONE_LINK_WINDOW_MS;
    check('a minute later: shown again', gate.take('tel:+41790000020', false), 'show');
}

print(`\n${count - failed}/${count} checks passed`);
if (failed > 0)
    imports.system.exit(1);
