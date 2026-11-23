// SPDX-License-Identifier: GPL-2.0-or-later
// Règles du modèle d'état, sans Shell : gjs -m tests/model.test.js
import {
    Model, initials, firstName, formatDuration, formatNumber, sender,
    MESSAGE_PILL_MS, SENT_PILL_MS, SEND_TIMEOUT_MS, TOUR_MIN_STEP_MS, TOUR_VERIFY_MS,
    OBEX_STUCK_MS, FIX_TIMEOUT_MS, RETRY_PROFILES_MS,
    LINK_SETTLE_MS, FLAP_WINDOW_MS, STEADY_MS, AUDIO_MOVE_MS, AUDIO_ANSWER_MS,
    LEASE_RENEW_MS, PROVISIONAL_MS, ANSWER_HERE_MS, DIAL_TIMEOUT_MS, ATTENTION_MS, PHONE_AUDIO_WAIT_MS,
    RECONNECT_HOLD_MS, RECONNECT_SETUP_MS, RECONNECT_MAX_MS, RECONNECT_AFTER_LINK_MS, RECONNECT_CALM_MS,
    reconnectReason, holdOffSeconds,
} from '../extension/lib/model.js';
import {phoneLanguage, bilingual, PHONE_LABELS} from '../extension/lib/phoneLabels.js';
import {replyLink, replySender, contactNumber} from '../extension/lib/replyLinks.js';
import {appRoute} from '../extension/lib/officialApps.js';
import {ICON_STATES, ICON_STYLES, MOTIONS, iconLayers, layerSvg, stateSvg} from '../extension/lib/iconArt.js';
import {DEVICE, Hotspot, failureKind} from '../extension/lib/hotspot.js';
import GLib from 'gi://GLib';

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

const T0 = 1794830000000;
function setup(steady = false) {
    const ctx = {now: T0, sent: [], shown: [], changes: 0};
    ctx.model = new Model({
        send: c => ctx.sent.push(c),
        now: () => ctx.now,
        quickReplies: ['Yes', 'On my way', 'Later'],
        // The steady indicator has its own tests; everywhere else a state
        // shows the instant it is true.
        steady,
    });
    ctx.model.onChange = () => ctx.changes++;
    ctx.model.onNotification = (kind, n) => ctx.shown.push([kind, n]);
    return ctx;
}
const status = (extra = {}) => ({
    command: 'bt_status', enabled: true, device_address: 'AA:BB:CC:00:00:01',
    capability: {mode: 'full', setup: []}, ...extra,
});
const conn = (extra = {}) => ({
    command: 'bt_connection_changed', device_present: true, device_paired: true,
    classic_connected: true, le_available: true, le_connected: true, map_open: true,
    map_error: 'none', pbap_open: true, ancs_ready: true, battery: 96, calls: {operator: 'Swisscom', audio: 'idle'}, ...extra,
});
const message = (extra = {}) => ({
    command: 'bt_message', handle: 'H1', thread: 'tel:+41790000001', address: '+41790000001',
    name: 'Camille Martin', body: 'On se retrouve à 19h ?', timestamp: T0 / 1000,
    outgoing: false, read: false, ...extra,
});
const call = (state, extra = {}) => ({
    command: 'bt_calls',
    calls: [{path: '/call1', number: '+41790000001', withheld: false, name: '', state, ...extra}],
});
function linked() {
    const ctx = setup();
    ctx.model.setEngineState('connected');
    ctx.model.handleEvent(status());
    ctx.model.handleEvent(conn());
    return ctx;
}

// --- helpers
check('initials of two words', initials('Camille Martin'), 'CM');
check('initials of one word', initials('Léo'), 'L');
check('initials of a number: none, it gets a pictogram', initials('+41790000001'), '');
check('first name', firstName('Camille Martin'), 'Camille');
check('first name of a number', firstName('+41 79 000 00 01'), '+41 79 000 00 01');
check('duration', formatDuration(161000), '02:41');
check('duration over an hour', formatDuration(3723000), '1:02:03');

// --- link states
{
    const c = setup();
    check('before the first answer: nothing claimed', c.model.view().link, 'starting');
    c.model.setEngineState('absent');
    let v = c.model.view();
    check('engine absent: said in words', [v.link, v.pill.strong, v.icon.state], ['engine', 'Engine not running', 'bare']);
    check('engine absent: card explains', [v.card.kind, v.card.problem, v.card.action.id], ['problem', 'engine', 'retry']);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    v = c.model.view();
    check('linked: quiet glyph and full dot', v.pill, {tone: 'quiet'});
    check('linked: menu header, the battery and nothing about the link',
        [v.card.kind, v.card.battery, 'state' in v.card, 'statusLine' in v.card], ['menu', 96, false, false]);
    check('linked, all well: the name and the battery; nothing under it',
        [v.card.name, v.card.notices, v.card.paused, v.icon.state], ['iPhone', [], null, 'linked']);
    check('the menu never offers to forget the iPhone', 'forgetAction' in v.card, false);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    v = c.model.view();
    check('out of range: hollow dot, dimmed', v.pill, {tone: 'quiet'});
    check('out of range: one line says so, no battery', [v.card.notices, v.card.battery], [['Out of range'], null]);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, device_paired: false}));
    check('forgotten by the iPhone: link again', [c.model.view().pill.strong, c.model.view().card],
        ['Link again', {kind: 'direct', action: 'link'}]);
    c.model.handleEvent(status({device_address: ''}));
    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    check('no iPhone chosen', c.model.view().pill.strong, 'Link iPhone');
    c.model.handleEvent(status({capability: {mode: 'blocked', setup: [
        {what: 'needs BlueZ experimental bearer API', command: 'sudo a'},
        {what: 'L2CAP ERTM is disabled', command: 'sudo b'}]}}));
    v = c.model.view();
    check('permission needed: label', [v.pill.tone, v.pill.strong], ['accent', 'Permission needed']);
    check('permission needed: plain steps and commands',
        [v.card.steps.length, /newer Bluetooth/.test(v.card.steps[0]), v.card.commands], [2, true, ['sudo a', 'sudo b']]);
    c.model.handleEvent(conn());
    check('linked wins over a missing permission', c.model.view().link, 'linked');
    check('...which is then a notice in the menu, and the mark of the icon',
        [c.model.view().card.notices.length, c.model.view().icon.state], [1, 'attention']);
}

// --- message: pill, ignored, unread count
{
    const c = linked();
    c.model.handleEvent(message());
    let v = c.model.view();
    check('message: amber pill', v.pill, {tone: 'accent', strong: 'Camille', text: 'On se retrouve à 19h ?', avatar: 'CM'});
    check('message: deadline', c.model.nextDeadline(), T0 + MESSAGE_PILL_MS);
    c.now = T0 + MESSAGE_PILL_MS;
    c.model.expire();
    v = c.model.view();
    check('ignored: back to rest with a count', v.pill, {tone: 'quiet'});
    check('ignored: the bubble moves until a minute after the message came',
        [v.icon, c.model.nextDeadline()], [{state: 'unread', moving: true}, T0 + ATTENTION_MS]);
    c.model.handleEvent(message());
    check('same message twice: counted once', c.model.view().unread, 1);
    c.model.handleEvent(message({handle: 'H2', body: 'Tu es là ?'}));
    c.model.handleEvent(message({handle: 'H3', body: 'Allo'}));
    check('a newer message displaces the pill', [c.model.view().pill.text, c.model.unreadCount], ['Allo', 2]);
    c.model.handleEvent({command: 'bt_message_read', handles: ['H1', 'H2'], read: true, success: true});
    check('read on the iPhone: count drops', c.model.unreadCount, 0);
}

// --- message history must not pop up
{
    const c = linked();
    c.model.handleEvent(message({handle: 'A', read: true}));
    c.model.handleEvent(message({handle: 'B', outgoing: true}));
    c.model.handleEvent(message({handle: 'C', timestamp: T0 / 1000 - 3600}));
    check('read, outgoing or old messages stay quiet', c.model.view().mode, 'idle');
    check('...but feed the conversation list', c.model.view().card.threads.length, 1);
}

// --- reply
{
    const c = linked();
    c.model.handleEvent(message());
    c.model.cardOpened();
    let v = c.model.view();
    check('card on a message pill: reply card', [v.card.kind, v.card.name, v.card.messages[0].body, v.card.quick.length],
        ['reply', 'Camille Martin', 'On se retrouve à 19h ?', 3]);
    check('opening marks as read on the iPhone', c.sent, [{command: 'bt_mark_read', handles: ['H1'], read: true}]);
    c.now = T0 + MESSAGE_PILL_MS + 1;
    check('open card: no timer left running', c.model.nextDeadline(), null);
    check('open card: pill stays', c.model.view().pill.strong, 'Camille');
    c.sent.length = 0;
    check('empty reply is not sent', c.model.reply('   '), false);
    c.model.reply(' Oui, parfait ');
    check('reply: command', c.sent, [{command: 'bt_send_message', thread: 'tel:+41790000001', body: 'Oui, parfait', operation_id: 'reply-1'}]);
    v = c.model.view();
    check('reply: card closes, pill waits', [v.cardOpen, v.mode, v.pill.strong], [false, 'sending', 'Sending…']);
    c.model.handleEvent({command: 'bt_send_result', thread: 'x', success: true, operation_id: 'other'});
    check('result of another operation is ignored', c.model.view().mode, 'sending');
    c.model.handleEvent({command: 'bt_send_result', thread: 'tel:+41790000001', success: true, operation_id: 'reply-1'});
    check('sent: confirmation', c.model.view().pill, {tone: 'neutral', check: true, strong: 'Sent'});
    c.now += SENT_PILL_MS;
    c.model.expire();
    check('then back to the dot', c.model.view().pill, {tone: 'quiet'});
}

// --- reply that fails, reply that never gets an answer
{
    const c = linked();
    c.model.handleEvent(message());
    c.model.cardOpened();
    c.model.reply('Oui');
    c.model.handleEvent({command: 'bt_send_result', thread: 'tel:+41790000001', success: false, message: 'Refused.', operation_id: 'reply-1'});
    check('failed: said on the pill', c.model.view().pill.strong, 'Not sent');
    c.model.cardOpened();
    const v = c.model.view();
    check('failed: card comes back with the text and the reason', [v.card.kind, v.card.draft, v.card.subtitle, v.card.isError],
        ['reply', 'Oui', 'Refused.', true]);

    const d = linked();
    d.model.handleEvent(message());
    d.model.cardOpened();
    d.model.reply('Oui');
    check('no answer: a deadline exists', d.model.nextDeadline(), T0 + SEND_TIMEOUT_MS);
    d.now = T0 + SEND_TIMEOUT_MS;
    d.model.expire();
    check('no answer: reported as not sent', d.model.view().pill.strong, 'Not sent');
}

// --- conversation opened from the menu, draft kept
{
    const c = linked();
    c.model.handleEvent({command: 'bt_threads', threads: [
        {thread: 't1', name: 'Léo', address: '+41790000002', preview: 'Bien reçu', timestamp: 10, repliable: true},
        {thread: 't2', name: 'Camille Martin', address: '+41790000001', preview: 'Salut', timestamp: 20, repliable: true}]});
    c.model.cardOpened();
    check('menu lists conversations, newest first', c.model.view().card.threads.map(t => t.name), ['Camille Martin', 'Léo']);
    c.model.openThread('t1');
    check('picked conversation: reply card', [c.model.view().card.kind, c.model.view().card.messages.map(m => m.body)], ['reply', ['Bien reçu']]);
    c.model.setDraft('je te rap');
    c.model.cardClosed();
    check('closed: back to rest', [c.model.view().mode, c.model.view().card.kind], ['idle', 'menu']);
    c.model.openThread('t1');
    check('draft is kept', c.model.view().card.draft, 'je te rap');
    c.model.handleEvent(message({handle: 'Z', thread: 't2'}));
    check('message for another conversation while typing: counted, not shown', [c.model.view().card.thread, c.model.unreadCount], ['t1', 1]);
}

// --- calls
{
    const c = linked();
    c.model.handleEvent({command: 'bt_threads', threads: [
        {thread: 'tel:+41790000001', name: 'Camille Martin', address: '+41790000001', preview: 'x', timestamp: 1}]});
    c.model.handleEvent(call('incoming'));
    let v = c.model.view();
    check('incoming call: pill', v.pill, {tone: 'accent', strong: 'Camille is calling'});
    check('incoming call: card', [v.card.kind, v.card.name, v.card.subtitle], ['ring', 'Camille Martin', 'Calling your iPhone']);
    check('name missing in the broadcast: list asked once', c.sent, [{command: 'bt_list_calls'}]);
    c.model.handleEvent(call('incoming', {name: 'Camille M.'}));
    c.model.handleEvent(call('incoming'));
    check('name from the list is kept, not asked again', [c.model.view().card.name, c.sent.length], ['Camille M.', 1]);
    c.sent.length = 0;
    c.model.handleEvent(message());
    check('message during a call: counted, pill untouched', [c.model.view().mode, c.model.unreadCount], ['ring', 1]);
    c.model.answer();
    check('answer: command', c.sent, [{command: 'bt_call_action', action: 'answer', path: '/call1'}]);
    c.model.handleEvent(call('active'));
    v = c.model.view();
    check('on a call: green pill with timer', v.pill, {tone: 'call', strong: 'Camille · 00:00'});
    check('on a call: next tick in one second', c.model.nextDeadline(), c.now + 1000);
    c.now += 161000;
    v = c.model.view();
    check('timer runs', [v.pill.strong, v.card.subtitle, v.card.audio], ['Camille · 02:41', '02:41 · sound on iPhone', 'phone']);
    c.sent.length = 0;
    c.model.setCallAudio('computer');
    check('sound here: command', c.sent, [{command: 'bt_call_action', action: 'audio_here'}]);
    check('sound here: shown at once', c.model.view().card.audio, 'computer');
    c.model.handleEvent({command: 'bt_call_result', action: 'audio_here', success: false, message: 'No call audio here.'});
    v = c.model.view();
    check('sound refused: back to iPhone, reason shown', [v.card.audio, v.card.subtitle, v.card.isError], ['phone', 'No call audio here.', true]);
    c.sent.length = 0;
    c.model.cardOpened();
    c.model.hangUp();
    check('hang up: command', c.sent, [{command: 'bt_call_action', action: 'hangup', path: '/call1'}]);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    v = c.model.view();
    check('call over: rest, card closed, missed message counted', [v.mode, v.cardOpen, v.unread], ['idle', false, 1]);

    c.model.handleEvent(call('incoming', {number: '', withheld: true}));
    check('hidden number', [c.model.view().pill.strong, c.model.view().card.name], ['Incoming call', 'Unknown caller']);
    c.sent.length = 0;
    c.model.decline();
    check('decline: command', c.sent, [{command: 'bt_call_action', action: 'hangup', path: '/call1'}]);
}

// --- battery
{
    const c = linked();
    c.model.handleEvent(conn({battery: 12}));
    check('battery low: the icon says so, no pill', [c.model.view().pill, c.model.view().icon.state], [{tone: 'quiet'}, 'battery']);
    check('...and the menu: the battery drawn red, no sentence', [c.model.view().card.batteryLow, c.model.view().card.notices], [true, []]);
    c.model.cardOpened();
    c.model.cardClosed();
    c.model.handleEvent(conn({battery: 11}));
    check('battery still low, menu seen: the icon is back at rest', [c.model.view().mode, c.model.view().icon.state], ['idle', 'linked']);
    check('battery in the menu', c.model.view().card.battery, 11);
}

// --- notifications of other apps: one pill, in order of importance
{
    const c = linked();
    const n = (extra = {}) => ({
        command: 'bt_notification', uid: 1, app_id: 'net.whatsapp.WhatsApp', app_name: 'WhatsApp',
        title: 'Camille', subtitle: '', body: 'On mange ensemble ?', category: 4, timestamp: T0 / 1000,
        silent: false, positive_action: true, negative_action: true, ...extra,
    });
    c.model.handleEvent(n());
    let v = c.model.view();
    check('a live notification grows the pill', [v.mode, v.pill], ['note',
        {tone: 'accent', strong: 'WhatsApp', text: 'Camille: On mange ensemble ?', apps: ['net.whatsapp.WhatsApp'], avatar: 'W'}]);
    c.now += 2000;
    c.model.handleEvent(n({uid: 2, body: 'Tu viens ?'}));
    c.model.handleEvent(n({uid: 3, app_id: 'com.apple.mobilemail', app_name: 'Mail', title: 'Facture'}));
    v = c.model.view();
    check('more within 5 s: one pill that counts them', [v.pill.strong, v.pill.text], ['3 notifications', 'WhatsApp, Mail']);
    check('...with the icons of their apps, once each', v.pill.apps, ['net.whatsapp.WhatsApp', 'com.apple.mobilemail']);
    c.model.handleEvent({command: 'bt_notification_removed', uid: 2});
    check('one removed on the iPhone: one less', c.model.view().pill.strong, '2 notifications');
    c.now += 6000;
    c.model.expire();
    check('ignored: the pill goes', c.model.view().mode, 'idle');
    c.model.handleEvent(n({uid: 4, body: 'Alors ?'}));
    c.model.cardOpened();
    v = c.model.view();
    check('clicked: its card', [v.card.kind, v.card.appName, v.card.title, v.card.body, v.card.canClear],
        ['note', 'WhatsApp', 'Camille', 'Alors ?', true]);
    check('the card knows the app, for its icon', [v.card.appId, v.pill.apps], ['net.whatsapp.WhatsApp', ['net.whatsapp.WhatsApp']]);
    c.model.handleEvent(n({uid: 5, body: 'Allô ?'}));
    check('another one while a card is open: no pill for it', c.model.view().card.uid, 4);
    c.sent.length = 0;
    c.model.clearNotification(4);
    check('Dismiss: cleared on the iPhone, gone here', [c.sent, c.model.view().card.kind],
        [[{command: 'bt_notification_action', uid: 4, action: 'negative'}], 'menu']);
    c.model.cardClosed();
    c.model.handleEvent(message());
    c.model.handleEvent(n({uid: 6, body: 'Encore'}));
    check('a message is in the top bar: the notification does not take its place', c.model.view().mode, 'message');
    c.now += 8000;
    c.model.expire();
    c.model.handleEvent(n({uid: 7, body: 'Plus tard'}));
    c.model.handleEvent(message({handle: 'H2'}));
    check('a message takes the place of a notification', c.model.view().mode, 'message');
    c.now += 8000;
    c.model.expire();
    c.model.setQuietApps(['whatsapp']);
    c.model.handleEvent(n({uid: 8, body: 'Chut'}));
    check('an app kept quiet: no pill', c.model.view().mode, 'idle');
    c.model.setQuietApps([]);
    c.model.setQuiet({dnd: true});
    c.model.handleEvent(n({uid: 9, body: 'Silence'}));
    c.model.setQuiet({dnd: false});
    check('Do Not Disturb: no pill, and none afterwards', c.model.view().mode, 'idle');
    c.model.handleEvent(n({uid: 10, app_id: 'com.apple.mobilephone', app_name: 'Phone', title: 'Camille', body: 'Missed Call', category: 2}));
    check('a missed call: the icon, no pill', [c.model.view().mode, c.model.view().icon.state], ['idle', 'missed']);

    // "On your iPhone": what waits there, in the menu.
    const m = linked();
    check('nothing waits: no section', m.model.view().card.phone, null);
    for (let i = 1; i <= 5; i++)
        m.model.handleEvent(n({uid: i, silent: true, body: `Ancien ${i}`, timestamp: T0 / 1000 - 600 * i}));
    m.model.handleEvent({command: 'bt_notifications', notifications: [1, 2, 3, 4, 5].map(i =>
        n({uid: i, silent: true, body: `Ancien ${i}`, timestamp: T0 / 1000 - 600 * i}))
        .concat([n({uid: 6, app_id: 'com.apple.mobilemail', app_name: 'Mail', title: 'CFF', body: 'Billet', timestamp: T0 / 1000 - 60, silent: true})])});
    m.model.handleEvent(n({uid: 7, app_id: 'ch.exemple.Agenda', app_name: 'Calendar', title: 'Dentiste', body: 'Demain'}));
    let phone = m.model.view().card.phone;
    check('already there at connection, and live: listed by app, the latest first, capped',
        [phone.total, phone.hidden, phone.groups.map(g => [g.appName, g.notes.map(x => x.uid)])],
        [7, 1, [['Calendar', [7]], ['Mail', [6]], ['WhatsApp', [1, 2, 3, 4]]]]);
    check('each app and each line knows its identifier, for the icon',
        phone.groups.map(g => [g.appId, g.notes[0].appId]),
        [['ch.exemple.Agenda', 'ch.exemple.Agenda'], ['com.apple.mobilemail', 'com.apple.mobilemail'],
            ['net.whatsapp.WhatsApp', 'net.whatsapp.WhatsApp']]);
    m.model.showAllNotes();
    check('Show all', m.model.view().card.phone.hidden, 0);
    m.model.handleEvent({command: 'bt_notification_removed', uid: 6});
    check('removed on the iPhone: gone', m.model.view().card.phone.groups.map(g => g.appName), ['Calendar', 'WhatsApp']);
    m.sent.length = 0;
    m.model.clearNotification(7);
    m.model.clearAllNotifications();
    check('Dismiss, Clear All: cleared on the iPhone, the section goes',
        [m.sent.map(x => [x.command, x.uid, x.action]), m.model.view().card.phone],
        [[7, 1, 2, 3, 4, 5].map(uid => ['bt_notification_action', uid, 'negative']), null]);

    // Back after an absence: one summary, once.
    const a = linked();
    const old = (uid, app, extra = {}) => n({uid, app_id: `x.${app}`, app_name: app, silent: true, body: `Texte ${uid}`, ...extra});
    [old(1, 'WhatsApp'), old(2, 'WhatsApp'), old(3, 'Mail'), old(4, 'Signal'), old(5, 'News'), old(6, 'Calendar')]
        .forEach(e => a.model.handleEvent(e));
    check('notifications were waiting: nothing while they still come', a.model.view().mode, 'idle');
    a.now = a.model.nextDeadline();
    a.model.expire();
    check('...then one summary, three apps at most',
        [a.model.view().mode, a.model.view().pill.strong, a.model.view().pill.text],
        ['summary', 'While you were away:', '2 WhatsApp, 1 Mail, 1 Signal and 2 more']);
    check('...with the icons of the three apps it names, in its order', a.model.view().pill.apps,
        ['x.WhatsApp', 'x.Mail', 'x.Signal']);
    a.model.cardOpened();
    check('clicked: the menu, with what waits', [a.model.view().card.kind, a.model.view().card.phone.total, a.model.view().mode], ['menu', 6, 'idle']);
    a.model.cardClosed();
    a.model.handleEvent(conn({le_connected: false, classic_connected: false}));
    a.model.handleEvent(conn());
    [old(11, 'WhatsApp', {body: 'Texte 1'}), old(12, 'WhatsApp', {body: 'Texte 2'}), old(13, 'Mail', {body: 'Texte 3'})].forEach(e => a.model.handleEvent(e));
    a.now += 3000;
    a.model.expire();
    check('the link came back with the same ones: no second summary', [a.model.view().mode, a.model.view().card.phone.total], ['idle', 3]);
    a.model.setQuiet({dnd: true});
    a.model.handleEvent(old(14, 'Mail'));
    a.now += 3000;
    a.model.expire();
    a.model.setQuiet({dnd: false});
    check('Do Not Disturb: no summary', a.model.view().mode, 'idle');

    // "Earlier": kept in memory only when asked for.
    const h = linked();
    h.model.handleEvent(n({uid: 1, body: 'Un'}));
    h.model.handleEvent({command: 'bt_notification_removed', uid: 1});
    check('history off: nothing is kept', [h.model.view().card.earlier, h.model._earlier.size], [null, 0]);
    h.model.setKeepHistory(true);
    h.model.handleEvent(n({uid: 2, body: 'Deux'}));
    h.model.handleEvent(n({uid: 3, body: 'Trois', timestamp: T0 / 1000 + 5}));
    check('history on, still on the iPhone: not earlier yet', h.model.view().card.earlier, []);
    h.model.handleEvent({command: 'bt_notification_removed', uid: 2});
    h.model.clearNotification(3);
    check('gone from the iPhone, or dismissed here: listed, the latest first',
        h.model.view().card.earlier.map(e => [e.appId, e.appName, e.title, e.body]),
        [['net.whatsapp.WhatsApp', 'WhatsApp', 'Camille', 'Trois'], ['net.whatsapp.WhatsApp', 'WhatsApp', 'Camille', 'Deux']]);
    h.now += 12 * 3600 * 1000 + 1;
    check('twelve hours later: gone', h.model.view().card.earlier, []);
    for (let i = 0; i < 250; i++)
        h.model.handleEvent(n({uid: 100 + i, body: `Lot ${i}`}));
    check('200 at most', h.model._earlier.size, 200);
    h.model.clearHistory();
    check('Clear history', h.model._earlier.size, 0);
    h.model.handleEvent(n({uid: 400, body: 'Encore'}));
    h.model.setKeepHistory(false);
    check('turned off: dropped at once', h.model._earlier.size, 0);
}

// --- pairing confirmation
{
    const c = linked();
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '703405', address: 'AA', timeout_seconds: 60, operation_id: 'p1'});
    let v = c.model.view();
    check('pairing: pill and card', [v.pill.strong, v.card.kind, v.card.code], ['Confirm link', 'pair', '703405']);
    c.model.confirmPairing(true);
    check('pairing: answer carries the operation', c.sent, [{command: 'bt_pair_confirm', accept: true, operation_id: 'p1'}]);
    check('pairing: card gone', c.model.view().mode, 'idle');
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '111111', timeout_seconds: 60});
    c.sent.length = 0;
    c.model.confirmPairing(false);
    check('pairing without operation: no identifier sent', c.sent, [{command: 'bt_pair_confirm', accept: false}]);
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '111111', timeout_seconds: 60});
    c.now += 60000;
    c.model.expire();
    check('pairing unanswered: card gone after the delay', c.model.view().mode, 'idle');
}

// --- notifications of other apps
{
    const c = linked();
    const n = (extra = {}) => ({command: 'bt_notification', uid: 7, app_id: 'ch.exemple.Agenda', app_name: 'Agenda',
        title: 'Dentiste', subtitle: '', body: 'Demain', category: 5, timestamp: 1, silent: false, negative_action: true, ...extra});
    c.model.handleEvent(n());
    check('other app: becomes a desktop notification',
        c.shown, [['show', {uid: 7, appId: 'ch.exemple.Agenda', appName: 'Agenda', title: 'Dentiste', body: 'Demain', timestamp: 1, silent: false, canClear: true}]]);
    c.shown.length = 0;
    c.model.handleEvent(n({uid: 8, app_id: 'com.apple.MobileSMS'}));
    c.model.handleEvent(n({uid: 9, app_id: 'com.apple.mobilephone', category: 1}));
    check('messages and calls stay with the pill', c.shown, []);
    c.model.handleEvent(n({uid: 10, app_id: 'com.apple.mobilephone', category: 2}));
    check('a missed call is shown', c.shown.length, 1);
    c.shown.length = 0;
    c.model.handleEvent(conn({map_open: false, map_error: 'forbidden'}));
    c.model.handleEvent(n({uid: 11, app_id: 'com.apple.MobileSMS'}));
    check('messages unreadable: their notification is not lost', c.shown.length, 1);
    c.shown.length = 0;
    c.model.handleEvent({command: 'bt_notification_removed', uid: 7});
    check('removed on the iPhone', c.shown, [['remove', {uid: 7}]]);
    c.sent.length = 0;
    c.model.clearNotification(7);
    c.model.clearNotification(11);
    check('dismissed here: cleared there, if it is still there', c.sent, [{command: 'bt_notification_action', uid: 11, action: 'negative'}]);
    c.shown.length = 0;
    c.model.handleEvent({command: 'bt_notifications', notifications: [n({uid: 12})]});
    check('list at start: what is gone is dropped, nothing in it is shown',
        c.shown, [['reset', {uids: [12]}]]);
    c.shown.length = 0;
    c.model.setEngineState('absent');
    check('engine lost: desktop notifications withdrawn', c.shown, [['reset', {uids: []}]]);
    check('engine lost: nothing stale shown', c.model.view().link, 'engine');
}

// --- permission, with the system helper
{
    const c = setup();
    c.model.setEngineState('connected');
    c.model.handleEvent(status({device_address: ''}));
    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    c.model.setSystemSetup({experimental: false, device_class: true, ertm: true, ready: false});
    let v = c.model.view();
    check('helper says not ready: permission needed, click goes to the dialog',
        [v.pill.strong, v.card], ['Permission needed', {kind: 'direct', action: 'link'}]);
    c.model.setSystemSetup({experimental: true, device_class: true, ertm: true, ready: true});
    v = c.model.view();
    check('helper says ready: next is linking', [v.pill.strong, v.card], ['Link iPhone', {kind: 'direct', action: 'link'}]);
    c.model.setSystemSetup({experimental: true, device_class: false, ertm: true, ready: true,
        persistent: {experimental: true, device_class: false, ertm: true}});
    check('the device class is not asked for any more: nothing needed', c.model.view().pill.strong, 'Link iPhone');
    c.model.setSystemSetup({experimental: true, device_class: false, ertm: true, ready: false,
        persistent: {experimental: true, device_class: false, ertm: true}});
    check('an older helper that is not ready because of the class alone: nothing asked',
        c.model.view().pill.strong, 'Link iPhone');
    c.model.setSystemSetup({experimental: false, ertm: true, ready: true,
        persistent: {experimental: false, ertm: true}, required: {experimental: false, ertm: true}});
    check('BlueZ where the newer features are not required: nothing asked', c.model.view().pill.strong, 'Link iPhone');
    c.model.setSystemSetup({experimental: true, ertm: true, ready: true, persistent: {experimental: true, device_class: false, ertm: false}});
    check('ready now but not after a restart: permission still needed', c.model.view().pill.strong, 'Permission needed');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    v = c.model.view();
    check('...and when linked, the link is shown and the menu offers to finish', [v.link, v.card.permissionAction, v.card.notices.length], ['linked', true, 1]);
    c.model.setSystemSetup({ready: true, persistent: {experimental: true, device_class: true, ertm: true}});
    check('all kept: nothing left to offer', c.model.view().card.permissionAction, false);
    c.model.handleEvent(status({device_address: '', capability: {mode: 'full', powered: false, setup: []}}));
    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    c.model.setSystemSetup({ready: false, device_class: false});
    v = c.model.view();
    check('Bluetooth off: said so, not a permission problem', [v.link, v.pill.strong, v.card.problem], ['bluetooth', 'Bluetooth is off', 'bluetooth']);
    c.model.setSystemSetup({experimental: true, device_class: true, ertm: true, ready: true});
    c.model.handleEvent(status({device_address: '', capability: {mode: 'blocked', setup: [{what: 'x', command: 'y'}]}}));
    check('the helper is believed over the engine', c.model.view().link, 'setup');
}

// --- permission, when nothing can ask for the password from here
{
    const c = setup();
    c.model.setEngineState('connected');
    c.model.handleEvent(status({device_address: ''}));
    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    const missing = {experimental: false, ertm: false, ready: false};
    c.model.setSystemSetup({...missing, elevation: true, command: ''});
    c.model.startTour();
    let tour = c.model.view().tour;
    check('a helper of root: the password step, no commands', [tour.step, tour.permissionManual, tour.permissionCommands],
        ['permission', false, []]);
    c.model.cancelTour();
    c.model.setSystemSetup({...missing, elevation: false, command: "sudo '/home/u/.local/lib/landline/landline-setup' apply"});
    c.model.startTour();
    tour = c.model.view().tour;
    check('no helper of root: the same step shows what to run by hand',
        [tour.step, tour.permissionManual, tour.permissionCommands, tour.permissionNeeds],
        ['permission', true, ["sudo '/home/u/.local/lib/landline/landline-setup' apply"], ['experimental', 'ertm']]);
    check('...nothing asked yet', c.model.view().tour.permission, 'idle');
    c.model.tourPermission('unavailable');
    check('asked to apply anyway: nothing pretended, still the commands', c.model.view().tour.permission, 'idle');
    c.model.setSystemSetup({...missing, ertm: true, elevation: false, command: 'sudo x apply'});
    tour = c.model.view().tour;
    check('one of the two done by hand: still on the step, one left', [tour.step, tour.permissionNeeds], ['permission', ['experimental']]);
    c.model.setSystemSetup({experimental: true, ertm: true, ready: true, elevation: false, command: 'sudo x apply'});
    tour = c.model.view().tour;
    check('both done by hand: noticed, the tour goes on by itself', [tour.permission, tour.plan?.[0] ?? 'permission', c.model.view().link],
        ['done', 'permission', 'setup']);
    // No command from the helper: the ones the engine lists.
    const d = setup();
    d.model.setEngineState('connected');
    d.model.handleEvent(status({device_address: '', capability: {mode: 'blocked', setup: [
        {what: 'experimental', command: 'sudo a'}, {what: 'ERTM', command: 'sudo b'}]}}));
    d.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    d.model.setSystemSetup({...missing, elevation: false});
    d.model.startTour();
    check('no command from the helper: the two the engine gives', d.model.view().tour.permissionCommands, ['sudo a', 'sudo b']);
}

// --- setup tour: from the first click to a working link
const phone = (extra = {}) => ({address: 'AA:01', name: 'iPhone de Camille', iphone: true, paired: false, ...extra});
const devices = list => ({command: 'bt_devices', devices: list});
const noPhone = {command: 'bt_connection_changed', device_present: false, device_paired: false};
const off = {ancs_ready: false, map_open: false, pbap_open: false, map_error: 'forbidden'};
function unlinked(extra = {}) {
    const c = setup();
    c.model = new Model({send: x => c.sent.push(x), now: () => c.now, hostName: 'atelier', steady: false, ...extra});
    c.model.setEngineState('connected');
    c.model.handleEvent(status({device_address: '', calls_enabled: false}));
    c.model.handleEvent(noPhone);
    return c;
}
// Let the step on screen have its moment, as the timer of the extension does.
function settle(c, ms = TOUR_MIN_STEP_MS) {
    c.now += ms;
    c.model.expire();
}
{
    const c = unlinked();
    c.model.startTour();
    let w = c.model.view().tour;
    check('tour: opens on the search, step 1 of 6', [w.open, w.step, w.index, w.total, c.sent.map(x => x.command)],
        [true, 'find', 1, 6, ['bt_scan', 'bt_list_devices']]);
    c.sent.length = 0;
    c.model.handleEvent(devices([{address: 'BB:02', name: 'Casque', iphone: false}]));
    check('no iPhone yet: nothing asked', c.sent, []);
    c.model.handleEvent({command: 'bt_scan_result', success: true});
    check('scan over, nothing found: scans again', c.sent, [{command: 'bt_scan'}, {command: 'bt_list_devices'}]);
    c.sent.length = 0;
    c.model.handleEvent(devices([phone()]));
    check('one iPhone: the computer calls it, by itself', c.sent, [{command: 'bt_pair', address: 'AA:01', operation_id: 'pair-1'}]);
    w = c.model.view().tour;
    check('...and says which one it found', [w.found, w.deviceName, w.canRetry], [true, 'iPhone de Camille', false]);
    c.model.handleEvent(devices([phone()]));
    c.model.startTour();
    check('one attempt at a time', c.sent.length, 1);

    c.model.handleEvent({command: 'bt_pair_progress', step: 'confirm', detail: '703405', operation_id: 'pair-1'});
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '703405', address: 'AA:01', timeout_seconds: 60, operation_id: 'pair-1'});
    check('the number came at once: "Found" still gets its moment', c.model.view().tour.step, 'find');
    check('...and the timer knows when to move on', c.model.nextDeadline(), c.now + 1000);
    settle(c);
    w = c.model.view().tour;
    check('step 2: the number and the time left', [w.step, w.index, w.code, w.secondsLeft, c.model.view().mode],
        ['code', 2, '703405', 59, 'idle']);
    c.now += 10900;
    check('countdown runs', [c.model.view().tour.secondsLeft, c.model.nextDeadline()], [48, c.now + 600]);
    c.sent.length = 0;
    c.model.confirmPairing(true);
    check('They Match: answer with the operation', c.sent, [{command: 'bt_pair_confirm', accept: true, operation_id: 'pair-1'}]);
    c.model.confirmPairing(true);
    check('answered once, and a step that says to wait', [c.sent.length, c.model.view().tour.step], [1, 'pairing']);

    c.model.handleEvent({command: 'bt_pair_result', success: true, status: 'paired', dual_bond: true, operation_id: 'pair-1'});
    c.model.handleEvent(status({calls_enabled: true}));
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, ...off}));
    settle(c, 5000);
    check('"paired" alone is not a link: still waiting', [c.model.view().tour.step, c.model.view().link], ['pairing', 'away']);
    c.model.handleEvent(conn({le_connected: false, ...off}));
    settle(c, 2000);
    check('one half up: still waiting', c.model.view().tour.step, 'pairing');
    c.model.handleEvent(conn(off));
    w = c.model.view().tour;
    check('both links up: the switches, with the name of this computer', [w.step, w.index, w.hostName, w.switches],
        ['switches', 4, 'atelier', {notifications: false, messages: false, contacts: false}]);
    c.model.handleEvent(conn({ancs_ready: true, map_open: false, pbap_open: false}));
    check('a switch ticks when the engine sees it', c.model.view().tour.switches.notifications, true);
    c.sent.length = 0;
    c.model.handleEvent(conn());
    check('all three on: counts asked, the last tick stays a moment',
        [c.model.view().tour.step, c.model.view().tour.allOn, c.sent.map(x => x.command)],
        ['switches', true, ['bt_list_threads', 'bt_list_contacts']]);
    settle(c);
    w = c.model.view().tour;
    check('then the try', [w.step, w.index, w.proof, w.canBack], ['try', 5, null, true]);
    c.model.handleEvent({command: 'bt_notification', uid: 7, app_id: 'com.apple.MobileSMS', app_name: 'Messages',
        title: 'Camille Martin', body: 'Test !', category: 4, timestamp: T0 / 1000, silent: false});
    check('a message arrives during the try: shown as the proof', c.model.view().tour.proof, {name: 'Camille Martin', body: 'Test !'});
    c.model.tourBack();
    w = c.model.view().tour;
    check('Back: the switches again, without running off', [w.step, w.reviewed], ['switches', true]);
    settle(c, 5000);
    check('...they stay until Continue', c.model.view().tour.step, 'switches');
    c.model.tourNext();
    c.model.tourNext();
    c.model.handleEvent({command: 'bt_contacts', query: '', contacts: new Array(214).fill({})});
    c.model.handleEvent({command: 'bt_threads', threads: [{thread: 'a', name: 'A', preview: 'x', timestamp: 1}]});
    w = c.model.view().tour;
    check('the end: real numbers', [w.step, w.index, w.total, w.battery, w.contacts, w.conversations], ['done', 6, 6, 96, 214, 1]);
    c.model.cancelTour();
    check('Done: tour gone, linked', [c.model.view().tour, c.model.view().link], [null, 'linked']);
}

// --- setup tour: call control follows Phone Settings
{
    const c = unlinked({callControl: true});
    check('not linked: the engine is left alone', c.sent.filter(x => x.command === 'bt_set_calls'), []);
    c.model.handleEvent(status({calls_enabled: false}));
    c.model.handleEvent(conn({calls: null}));
    c.model.handleEvent(conn({calls: null, battery: 95}));
    check('linked, calls off in the engine: turned on, once', c.sent.filter(x => x.command === 'bt_set_calls'),
        [{command: 'bt_set_calls', enabled: true}]);
    c.model.handleEvent(status({calls_enabled: true}));
    c.sent.length = 0;
    c.model.setCallControl(false);
    check('switched off in Phone Settings: turned off', c.sent, [{command: 'bt_set_calls', enabled: false}]);
}

// --- setup tour: the permission step
{
    const c = unlinked();
    c.model.setSystemSetup({experimental: false, device_class: false, ertm: false, ready: false, persistent: {}});
    check('settings missing: Permission needed, one click to the tour', [c.model.view().pill.strong, c.model.view().card],
        ['Permission needed', {kind: 'direct', action: 'link'}]);
    c.model.startTour();
    let w = c.model.view().tour;
    check('tour: starts with the permission, step 1 of 7', [w.step, w.index, w.total, w.permission, w.restartsBluetooth],
        ['permission', 1, 7, 'idle', true]);
    check('...which lists the two settings this computer lacks', w.permissionNeeds, ['experimental', 'ertm']);
    c.model.tourPermission('waiting');
    check('password window up: waiting, said so', c.model.view().tour.permission, 'waiting');
    c.model.closeTour();
    check('...and the tour is kept while it is up', c.model.view().tour !== null, true);
    c.model.startTour();
    c.model.tourPermission('refused');
    w = c.model.view().tour;
    check('password not given: stays on the step, gently', [w.step, w.permission], ['permission', 'refused']);
    c.model.tourPermission('waiting');
    c.sent.length = 0;
    c.model.setSystemSetup({experimental: true, device_class: true, ertm: true, ready: true,
        persistent: {experimental: true, device_class: true, ertm: true}});
    c.model.tourPermission('done');
    check('applied: the search starts by itself', c.sent.map(x => x.command), ['bt_scan', 'bt_list_devices']);
    check('...after the permission step had its moment', c.model.view().tour.step, 'permission');
    settle(c);
    w = c.model.view().tour;
    check('then step 2 of 7', [w.open, w.step, w.index, w.total], [true, 'find', 2, 7]);
    c.model.closeTour();
    let v = c.model.view();
    check('Escape while searching: steps aside, the indicator brings it back',
        [v.tour.open, v.pill.strong, v.card], [false, 'Continue setup', {kind: 'direct', action: 'link'}]);
    c.model.handleEvent(devices([phone()]));
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '703405', timeout_seconds: 60, operation_id: 'pair-1'});
    check('the number arrives meanwhile: the indicator says so', c.model.view().pill.strong, 'Confirm link');
    c.sent.length = 0;
    c.model.startTour();
    check('click: back where it was, nothing started twice', [c.model.view().tour.open, c.sent], [true, []]);

    const d = unlinked();
    d.model.setSystemSetup({experimental: true, device_class: false, ertm: true, ready: true,
        persistent: {experimental: true, device_class: false, ertm: false}});
    d.model.handleEvent(status());
    d.model.handleEvent(conn());
    d.model.startTour();
    w = d.model.view().tour;
    check('linked, settings that would not survive a restart: permission, then the end', [w.step, w.total], ['permission', 2]);
    check('...one point only, and no restart of Bluetooth', [w.permissionNeeds, w.restartsBluetooth], [['ertm'], false]);
    d.model.tourPermission('waiting');
    d.model.tourPermission('done');
    check('"done" from the helper is checked against its status', d.model.view().tour.permission, 'failed');
    d.model.setSystemSetup({ready: true, persistent: {experimental: true, device_class: true, ertm: true}});
    d.model.tourPermission('done');
    settle(d);
    check('...nothing else to do: the end', d.model.view().tour.step, 'done');

    // Nothing to ask: the password step is not shown at all.
    const f = unlinked();
    f.model.setSystemSetup({experimental: false, device_class: false, ertm: true, ready: true,
        persistent: {experimental: false, device_class: false, ertm: true}, required: {experimental: false, ertm: true}});
    f.model.startTour();
    w = f.model.view().tour;
    check('nothing to ask of this computer: the tour starts at the search, 6 steps',
        [w.step, w.total, w.permissionNeeds], ['find', 6, []]);
    const g = unlinked();
    g.model.setSystemSetup(null);
    g.model.handleEvent(status({device_address: '', capability: {mode: 'full', class_ok: false, setup: [
        {what: 'adapter presents itself as A/V Hands-Free (major 4, minor 8)', command: 'sudo btmgmt class 4 8'}]}}));
    check('an older engine that still asks for the device class: not a permission problem', g.model.view().link, 'setup');

    const e = unlinked();
    e.model.setSystemSetup({experimental: false, ertm: false, ready: false, persistent: {}});
    e.model.handleEvent(status());
    e.model.handleEvent(conn({classic_connected: false, le_connected: false}));
    e.model.handleEvent(devices([phone({paired: true})]));
    e.model.startTour();
    e.model.tourPermission('waiting');
    e.model.setSystemSetup({ready: true, persistent: {}});
    e.sent.length = 0;
    e.model.tourPermission('done');
    settle(e);
    w = e.model.view().tour;
    check('a pairing that is not known to be broken gets its chance after the permission',
        [w.step, w.checking, e.sent], ['pairing', true, []]);
    e.model.handleEvent(conn());
    settle(e);
    check('...it comes back by itself: straight to the end', e.model.view().tour.step, 'done');
}

// --- setup tour: several iPhones, expiry, refusal, not found
{
    const c = unlinked();
    c.model.startTour();
    c.sent.length = 0;
    c.model.handleEvent({command: 'bt_devices', devices: [
        {address: 'AA:01', name: 'iPhone de Camille', iphone: true}, {address: 'AA:02', name: 'iPhone de Léo', iphone: true}]});
    check('two iPhones: a short list, nothing called', [c.model.view().tour.candidates.map(x => x.name), c.sent],
        [['iPhone de Camille', 'iPhone de Léo'], []]);
    c.model.choosePairingDevice('AA:02');
    check('the chosen one is called', c.sent, [{command: 'bt_pair', address: 'AA:02', operation_id: 'pair-1'}]);

    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '111111', timeout_seconds: 60, operation_id: 'pair-1'});
    c.now += 60000;
    c.model.expire();
    let w = c.model.view().tour;
    check('number expired: said plainly, no second attempt while the first one runs',
        [w.step, w.at, w.error, w.canRetry], ['failed', 'code', 'The number was not confirmed in time.', false]);
    c.model.handleEvent({command: 'bt_pair_result', success: false, status: 'timeout', operation_id: 'pair-1'});
    w = c.model.view().tour;
    check('attempt over: Try Again offered, message kept', [w.canRetry, w.error], [true, 'The number was not confirmed in time.']);
    c.sent.length = 0;
    c.model.retryTour();
    check('Try Again: searches again', [c.model.view().tour.step, c.sent[0]], ['find', {command: 'bt_scan'}]);

    c.model.handleEvent({command: 'bt_devices', devices: [{address: 'AA:01', name: 'iPhone', iphone: true}]});
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '222222', timeout_seconds: 60, operation_id: 'pair-2'});
    settle(c);
    c.sent.length = 0;
    c.model.confirmPairing(false);
    check('They Differ: refusal sent', c.sent, [{command: 'bt_pair_confirm', accept: false, operation_id: 'pair-2'}]);
    c.model.handleEvent({command: 'bt_pair_result', success: false, status: 'rejected', operation_id: 'pair-2'});
    check('declined: plain words', /declined/.test(c.model.view().tour.error), true);

    c.model.retryTour();
    c.model.handleEvent({command: 'bt_devices', devices: [{address: 'AA:01', name: 'iPhone', iphone: true}]});
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '333333', timeout_seconds: 60, operation_id: 'pair-3'});
    c.sent.length = 0;
    c.model.cancelTour();
    check('Cancel on the number: refusal sent, tour gone',
        [c.sent, c.model.view().tour], [[{command: 'bt_pair_confirm', accept: false, operation_id: 'pair-3'}], null]);

    c.model.startTour();
    c.model.handleEvent({command: 'bt_devices', devices: [{address: 'AA:01', name: 'iPhone', iphone: true}]});
    c.model.cancelTour();
    c.sent.length = 0;
    c.model.handleEvent({command: 'bt_pair_confirm_request', code: '444444', timeout_seconds: 60, operation_id: 'pair-4'});
    c.model.handleEvent({command: 'bt_pair_result', success: false, status: 'rejected', operation_id: 'pair-4'});
    check('cancelled while the iPhone was being called: the late number is declined, quietly',
        [c.sent, c.model.view().mode], [[{command: 'bt_pair_confirm', accept: false, operation_id: 'pair-4'}], 'idle']);

    c.model.startTour();
    for (let i = 0; i < 6; i++)
        c.model.handleEvent({command: 'bt_scan_result', success: true});
    check('never found: gives up with advice', [c.model.view().tour.step, /not found/.test(c.model.view().tour.error)], ['failed', true]);
}

// --- setup tour: a pairing that exists but does not work
{
    // What the second real trial met: a bond made outside the tour, BR/EDR
    // only. The engine answers "already paired" and no link ever comes up.
    const c = unlinked();
    c.model.handleEvent(status({device_address: '', capability: {mode: 'full', setup: [],
        bonded_device_present: true, bond_has_le: false, bond_carries_ancs: false}}));
    c.model.handleEvent(devices([phone({paired: true, bonded: true, le_bonded: false})]));
    check('partial pairing, no iPhone chosen: Link again, not "linked"', [c.model.view().link, c.model.view().pill.strong],
        ['broken', 'Link again']);
    c.sent.length = 0;
    c.model.startTour();
    let w = c.model.view().tour;
    check('the computer already knows this iPhone: the tour starts with the repair, nothing sent yet',
        [w.step, w.index, w.total, w.repair, c.sent], ['repair', 1, 7, 'ask', []]);
    c.model.tourRepairDone();
    check('Done on My iPhone: this computer forgets it too', [c.model.view().tour.repair, c.sent],
        ['removing', [{command: 'bt_unpair', address: 'AA:01', operation_id: 'unpair-1'}]]);
    c.sent.length = 0;
    c.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', operation_id: 'unpair-1'});
    c.model.handleEvent(status({device_address: ''}));
    check('forgotten: the normal pairing starts over', c.sent.map(x => x.command), ['bt_scan', 'bt_list_devices']);
    settle(c);
    check('...on the search step', [c.model.view().tour.step, c.model.view().tour.index], ['find', 2]);

    // The same met halfway: the tour asked to pair and got "already paired".
    const d = unlinked();
    d.model.startTour();
    d.model.handleEvent(devices([phone({paired: true})]));
    check('found, but already paired here: no bt_pair that would skip the number',
        [d.sent.some(x => x.command === 'bt_pair'), d.model.view().tour.step], [false, 'find']);
    settle(d);
    check('...the repair instead', [d.model.view().tour.step, d.model.view().tour.repair], ['repair', 'ask']);

    const e = unlinked();
    e.model.startTour();
    e.model.handleEvent(devices([phone()]));
    e.model.handleEvent({command: 'bt_pair_progress', step: 'already_paired', detail: 'iPhone', operation_id: 'pair-1'});
    e.model.handleEvent({command: 'bt_pair_result', success: true, status: 'already_paired', dual_bond: true, operation_id: 'pair-1'});
    e.model.handleEvent(status());
    e.model.handleEvent(conn({classic_connected: false, le_connected: false}));
    settle(e);
    w = e.model.view().tour;
    check('"already paired": no success announced, the link is checked first', [w.step, w.checking, e.model.view().link],
        ['pairing', true, 'away']);
    e.now += 15000;
    e.model.expire();
    settle(e);
    check('no link after a while: the repair', e.model.view().tour.step, 'repair');
    {
        const slow = unlinked();
        slow.model.startTour();
        slow.model.handleEvent(devices([phone()]));
        slow.model.handleEvent({command: 'bt_pair_confirm_request', code: '1', timeout_seconds: 60, operation_id: 'pair-1'});
        settle(slow);
        slow.model.confirmPairing(true);
        slow.model.handleEvent({command: 'bt_pair_result', success: true, status: 'paired', dual_bond: true, operation_id: 'pair-1'});
        slow.model.handleEvent(status());
        slow.now += TOUR_VERIFY_MS;
        slow.model.expire();
        settle(slow);
        check('links slower than expected: the repair is offered, nothing is forgotten', [slow.model.view().tour.step,
            slow.sent.some(x => x.command === 'bt_unpair')], ['repair', false]);
        slow.model.handleEvent(conn(off));
        settle(slow);
        check('...they come up after all: on to the switches, no repair', slow.model.view().tour.step, 'switches');
    }
    e.model.tourRepairDone();
    e.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', operation_id: 'unpair-2'});
    e.model.handleEvent(devices([phone()]));
    e.model.handleEvent({command: 'bt_pair_confirm_request', code: '555555', timeout_seconds: 60, operation_id: 'pair-3'});
    settle(e);
    settle(e);
    e.model.confirmPairing(true);
    e.model.handleEvent({command: 'bt_pair_result', success: true, status: 'paired', dual_bond: false, operation_id: 'pair-3'});
    w = e.model.view().tour;
    check('still no LE half after a repair: said plainly, no endless loop', [w.step, /still does not come up/.test(w.error)],
        ['failed', true]);

    // The iPhone refuses the link again and again.
    const f = linked();
    f.model.handleEvent(conn({classic_connected: true, le_connected: false,
        link_reason: 'LE connect failed (org.bluez.Reason.Authentication).'}));
    let v = f.model.view();
    check('authentication failures: not shown as linked, a click repairs', [v.link, v.pill.strong, v.card],
        ['broken', 'Link again', {kind: 'direct', action: 'link'}]);
    // The engine of docs/contrat-appairage.md says it in so many words.
    f.model.handleEvent(conn({bond_state: 'broken', bond_detail: 'auth_failures', needs_repair: true}));
    check('needs_repair: never "linked", whatever the links say', f.model.view().link, 'broken');
    f.model.handleEvent(conn({le_available: false, bond_state: 'partial', bond_detail: 'bredr_only', needs_repair: false}));
    check('a partial pairing with no remedy is not sent to repair', f.model.view().link, 'linked');
    f.model.handleEvent(conn({classic_connected: false, le_connected: false, bond_state: 'broken', needs_repair: true}));
    f.model.startTour();
    check('...and the tour opens on the repair', f.model.view().tour.step, 'repair');
    f.model.tourRepairDone();
    f.model.handleEvent({command: 'bt_unpair_result', success: false, message: 'x', operation_id: 'unpair-1'});
    w = f.model.view().tour;
    check('this computer could not forget it: said, with what to do', [w.step, /could not forget/.test(w.error)], ['failed', true]);

    const h = unlinked();
    h.model.startTour();
    h.model.handleEvent(devices([phone({paired: true, bond_state: 'ok'})]));
    check('a pairing the engine vouches for is used', h.sent.at(-1), {command: 'bt_pair', address: 'AA:01', operation_id: 'pair-1'});
    h.model.handleEvent({command: 'bt_pair_progress', step: 'stale_bond', detail: 'bredr_only', operation_id: 'pair-1'});
    h.model.handleEvent({command: 'bt_pair_result', success: false, status: 'stale_bond', reason: 'stale_bond',
        needs_repair: true, operation_id: 'pair-1'});
    settle(h);
    check('"stale_bond": not a failure, the repair', [h.model.view().tour.step, h.model.view().tour.repair], ['repair', 'ask']);
    h.model.tourRepairDone();
    h.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', removed: true,
        selection_cleared: true, operation_id: 'unpair-2'});
    h.sent.length = 0;
    h.model.handleEvent(devices([phone()]));
    check('after a repair the pairing is asked afresh', h.sent,
        [{command: 'bt_pair', address: 'AA:01', operation_id: 'pair-3', fresh: true}]);

    const g = unlinked();
    g.model.handleEvent({command: 'bt_pair_result', success: true, status: 'already_paired'});
    check('a pairing result outside the tour announces nothing', [g.model.view().mode, g.model.view().pill.strong], ['idle', 'Link iPhone']);
}

// --- real data in the cards
{
    check('Swiss mobile number', formatNumber('+41790000001'), '+41 79 000 00 01');
    check('French number', formatNumber('+33612345678'), '+33 6 12 34 56 78');
    check('North American number', formatNumber('+12125550123'), '+1 212 555 0123');
    check('national number', formatNumber('0790000001'), '079 000 00 01');
    check('short code left alone', formatNumber('38200'), '38200');
    check('other country: readable groups', formatNumber('+4915123456789'), '+49 151 234 567 89');
    check('sender with a name', sender('Camille Martin', '+41790000001', 'tel:+41790000001'),
        {name: 'Camille Martin', short: 'Camille', initials: 'CM', shortCode: false});
    check('sender without a name: the number, readable, no initials', sender('', '+41765550142', 'tel:+41765550142'),
        {name: '+41 76 555 01 42', short: '+41 76 555 01 42', initials: '', shortCode: false});
    check('short code', sender('38200', '38200', 'tel:38200'), {name: '38200', short: '38200', initials: '', shortCode: true});
    check('iMessage address', sender('', 'lea.rochat@exemple.ch', 'lea.rochat@exemple.ch'),
        {name: 'lea.rochat@exemple.ch', short: 'lea.rochat', initials: 'LR', shortCode: false});

    const c = linked();
    c.model.handleEvent(message({handle: 'N1', thread: 'tel:+41765550142', address: '+41765550142', name: '', body: 'Bonjour'}));
    check('unknown number: pictogram in the pill, readable number', c.model.view().pill,
        {tone: 'accent', strong: '+41 76 555 01 42', text: 'Bonjour'});
    c.model.cardOpened();
    let card = c.model.view().card;
    check('...and in the card', [card.name, card.initials, card.messages.map(m => m.body), card.quick.length],
        ['+41 76 555 01 42', '', ['Bonjour'], 3]);
    c.model.cardClosed();

    c.model.handleEvent(message({handle: 'S1', body: 'Tu es où ?'}));
    c.model.handleEvent(message({handle: 'S2', body: 'Je suis devant'}));
    c.model.handleEvent(message({handle: 'S3', body: ''}));
    check('three in a row, the last one a photo: the pill says so', c.model.view().pill.text, 'Attachment');
    c.sent.length = 0;
    c.model.cardOpened();
    card = c.model.view().card;
    check('the card shows the run, oldest first', card.messages.map(m => m.body), ['Tu es où ?', 'Je suis devant', '']);
    check('...and marks all of it read', c.sent, [{command: 'bt_mark_read', handles: ['S1', 'S2', 'S3'], read: true}]);
    c.sent.length = 0;
    c.model.handleEvent(message({handle: 'S4', body: 'Ah, je te vois'}));
    check('one more while the card is open: joins it, read at once',
        [c.model.view().card.messages.map(m => m.body), c.sent, c.model.unreadCount],
        [['Je suis devant', '', 'Ah, je te vois'], [{command: 'bt_mark_read', handles: ['S4'], read: true}], 0]);
    c.model.cardClosed();

    c.model.handleEvent(message({handle: 'C1', thread: 'tel:38200', address: '38200', name: '38200',
        body: '482913 est votre code'}));
    c.model.cardOpened();
    check('verification code from a short code: no ready-made replies', c.model.view().card.quick, []);
    c.model.cardClosed();
    c.model.handleEvent({command: 'bt_threads', threads: [
        {thread: 'tel:+41765550142', name: '', address: '+41765550142', preview: '', timestamp: 5}]});
    const row = c.model.view().card.threads[0];
    check('menu: number readable, pictogram, empty text named', [row.name, row.initials, row.preview],
        ['+41 76 555 01 42', '', 'Attachment']);
    c.model.handleEvent(call('incoming', {number: '+41225550199'}));
    card = c.model.view().card;
    check('call from an unknown number: readable, pictogram', [card.name, card.initials, c.model.view().pill.strong],
        ['+41 22 555 01 99', '', '+41 22 555 01 99']);
}

// --- notifications already on the iPhone at reconnection
{
    const c = linked();
    for (let i = 0; i < 40; i++) {
        c.model.handleEvent({command: 'bt_notification', uid: 100 + i, app_id: 'a.b', app_name: 'B', title: 't', body: 'b',
            category: 0, timestamp: 1, silent: true, negative_action: true});
    }
    check('burst of old notifications: none becomes a desktop notification', c.shown, []);

    // What the user saw for real: every reconnection brought the old pile back.
    const note = (uid, extra = {}) => ({command: 'bt_notification', uid, app_id: 'ch.exemple.Agenda', app_name: 'Agenda',
        title: 'Dentiste', subtitle: '', body: 'Demain', category: 5, timestamp: 1000 + uid, silent: false,
        negative_action: true, ...extra});
    const shows = () => c.shown.filter(([kind]) => kind === 'show').map(([, n]) => n.uid);
    c.model.handleEvent(note(1));
    check('a new one, received live: shown', shows(), [1]);
    c.model.handleEvent(note(1, {app_name: 'Calendar'}));
    check('the same one sent again with its app name corrected: an update', c.shown.at(-1)[1].appName, 'Calendar');
    c.shown.length = 0;
    // The link drops and comes back: the engine sends the pile again.
    for (let i = 0; i < 31; i++)
        c.model.handleEvent(note(200 + i, {silent: true}));
    c.model.handleEvent(note(1, {silent: true}));
    check('reconnection, 31 old ones marked silent: nothing', c.shown, []);
    c.model.handleEvent(note(400, {timestamp: 1200}));
    check('one of the old ones sent again without "silent": still nothing', c.shown, []);
    c.model.handleEvent(note(300));
    check('...then one new: one notification', shows(), [300]);
    c.shown.length = 0;
    c.model.handleEvent(note(1));
    check('already shown and still there, sent again without "silent": no second one',
        c.shown.filter(([, n]) => n.title !== 'Dentiste'), []);
    c.shown.length = 0;
    c.model.handleEvent(note(77, {timestamp: 1300}));
    check('the same notification under another number: not shown again', c.shown, []);
    c.model.handleEvent({command: 'bt_notification_removed', uid: 300});
    check('removed on the iPhone: ours goes', c.shown, [['remove', {uid: 300}]]);
    c.shown.length = 0;
    c.model.handleEvent(note(300));
    check('...and does not come back if the engine sends it again', c.shown, []);
    // The engine restarts: numbers start over, memory does not.
    c.model.setEngineState('absent');
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    c.shown.length = 0;
    c.model.handleEvent(note(5, {timestamp: 1001}));
    check('after an engine restart: what was shown before stays quiet', c.shown, []);
    c.model.handleEvent(note(6, {timestamp: 5000, title: 'Nouveau'}));
    check('...a really new one shows', shows(), [6]);

    // Messages: seen once, never a pill again.
    const d = linked();
    d.model.handleEvent(message());
    d.now = T0 + MESSAGE_PILL_MS;
    d.model.expire();
    d.model.setEngineState('absent');
    d.model.setEngineState('connected');
    d.model.handleEvent(status());
    d.model.handleEvent(conn());
    d.model.handleEvent(message());
    check('a message already seen, replayed after the engine came back: no pill', d.model.view().mode, 'idle');
}

// --- messages and contacts not on offer (the system OBEX service out of reach)
{
    const down = {map_open: false, pbap_open: false, map_error: 'other', pbap_error: 'other'};
    const off = {map_open: false, pbap_open: false, map_error: 'forbidden', pbap_error: 'forbidden'};
    const NOTICE = 'This computer can’t offer messages and contacts right now.';
    let c = linked();
    c.model.handleEvent(conn(off));
    c.now += 10 * OBEX_STUCK_MS;
    c.model.expire();
    let card = c.model.view().card;
    check('switches merely off on the iPhone (forbidden): the normal case, however long',
        [card.fixAction, card.setupAction, card.notices], [false, true, ['Some sharing switches are still off on the iPhone.']]);
    c.model.handleEvent(conn({...off, map_error: 'other'}));
    c.now += 10 * OBEX_STUCK_MS;
    c.model.expire();
    check('one of the two fails otherwise: not that fault', c.model.view().card.fixAction, false);
    c.model.handleEvent(conn({...down, classic_connected: false, le_connected: false}));
    c.now += 10 * OBEX_STUCK_MS;
    c.model.expire();
    check('links down: not that fault either', c.model.view().card.fixAction ?? false, false);

    c = linked();
    c.model.handleEvent(conn(down));
    check('both fail with another reason: nothing said yet, a deadline is set',
        [c.model.view().card.fixAction, c.model.nextDeadline()], [false, T0 + OBEX_STUCK_MS]);
    c.now += OBEX_STUCK_MS - 1;
    c.model.expire();
    check('just under 20 s: still nothing', c.model.view().card.fixAction, false);
    c.now += 1;
    c.model.expire();
    card = c.model.view().card;
    check('20 s: said in the menu, with Fix It, instead of "switches still off"',
        [card.notices, card.fixAction, card.setupAction], [[NOTICE], true, false]);
    c.model.handleEvent(conn({...down, map_error: 'no_daemon', pbap_error: 'no_daemon'}));
    check('obexd absent (no_daemon): the same fault', c.model.view().card.fixAction, true);
    c.model.handleEvent(conn({...down, map_error: 'busy', pbap_error: 'busy'}));
    check('another computer holds the sessions (busy): not that fault', c.model.view().card.fixAction, false);
    c.model.handleEvent(conn({...down, map_error: 'daemon_down', pbap_error: 'daemon_down'}));
    check('obexd coming back (daemon_down): the engine sees to it, not that fault',
        c.model.view().card.fixAction, false);

    // Fix It from the menu: the engine restarts and comes back sound
    c = linked();
    c.model.handleEvent(conn(down));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    c.model.fixStarted();
    check('Fix It: said while the engine restarts', [c.model.fixing, c.model.view().card.notices],
        [true, ['Restarting the phone link…']]);
    c.model.setEngineState('absent');
    let v = c.model.view();
    check('the engine going away is expected: not "Engine not running"', [v.link, v.card.title], ['starting', 'Restarting…']);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn(off));
    card = c.model.view().card;
    check('back with the services on offer: the usual "switches off" again',
        [c.model.fixing, card.fixAction, card.setupAction], [false, false, true]);

    c.model.handleEvent(conn(down));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    card = c.model.view().card;
    check('...but failing again a moment later: the restart did not help', [card.fixAction, card.notices.length], [false, 1]);

    // ... or comes back no better
    c = linked();
    c.model.handleEvent(conn(down));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    c.model.fixStarted();
    c.model.setEngineState('absent');
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn(down));
    check('back, failing again: wait before saying so', c.model.view().card.notices, ['Restarting the phone link…']);
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    card = c.model.view().card;
    check('still failing after 20 s: forget (in Phone Settings) and link again, no second Fix It',
        [card.notices, card.fixAction],
        [['Restarting did not bring messages and contacts back. Forget this iPhone in Phone Settings, then link it again.'], false]);

    // the restart itself fails, or the engine never comes back
    c = linked();
    c.model.handleEvent(conn(down));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    c.model.fixStarted();
    c.model.fixFailed();
    card = c.model.view().card;
    check('systemctl said no: said, Fix It offered again', [card.notices, card.fixAction],
        [['The phone link could not be restarted.'], true]);
    c.model.fixStarted();
    c.model.setEngineState('absent');
    check('a deadline for the restart', c.model.nextDeadline(), c.now + FIX_TIMEOUT_MS);
    c.now += FIX_TIMEOUT_MS;
    c.model.expire();
    check('engine never back: now it is "not running"', [c.model.fixing, c.model.view().link], [false, 'engine']);

    // In the tour, on the step of the switches
    c = linked();
    c.model.handleEvent(conn({...off, ancs_ready: false}));
    c.model.startTour();
    v = c.model.view().tour;
    check('tour on the switches, all off: nothing wrong yet', [v.step, v.obex], ['switches', null]);
    c.model.handleEvent(conn({...down, ancs_ready: false}));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    check('the fault is told on the step', c.model.view().tour.obex, 'down');
    c.model.fixStarted();
    c.model.setEngineState('absent');
    v = c.model.view().tour;
    check('Fix It: the tour survives the restart of the engine', [v?.step, v?.obex, v?.open], ['switches', 'restarting', true]);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn({...off, ancs_ready: false, classic_connected: false, le_connected: false}));
    check('engine back, link not yet: still restarting', c.model.view().tour.obex, 'restarting');
    c.model.handleEvent(conn({...off, ancs_ready: false}));
    v = c.model.view().tour;
    check('link back, services on offer: said, the step goes on', [v.step, v.obex], ['switches', 'fixed']);
    c.model.handleEvent(conn({...off, ancs_ready: false, map_open: true, map_error: 'none'}));
    v = c.model.view().tour;
    check('a switch comes on: nothing left to say', [v.obex, v.switches.messages], [null, true]);

    c = linked();
    c.model.handleEvent(conn({...down, ancs_ready: false}));
    c.model.startTour();
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    c.model.fixStarted();
    c.model.setEngineState('absent');
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn({...down, ancs_ready: false}));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    check('tour: restart did not help', c.model.view().tour.obex, 'still');
    c.model.startForget();
    v = c.model.view().tour;
    check('...start from scratch: the question, with a way back', [v.step, v.forget], ['forget', {state: 'confirm', back: true}]);
}

// --- the engine names the fault itself (bus_closed), and restarts by itself
{
    const closed = {map_open: false, pbap_open: false, map_error: 'bus_closed', pbap_error: 'bus_closed'};
    const off = {map_open: false, pbap_open: false, map_error: 'forbidden', pbap_error: 'forbidden'};
    let c = linked();
    c.model.handleEvent(conn(closed));
    let card = c.model.view().card;
    check('bus_closed: told at once, no 20 s wait', [card.fixAction, card.notices],
        [true, ['This computer can’t offer messages and contacts right now.']]);
    c = linked();
    c.model.handleEvent(conn({...off, ancs_ready: false}));
    c.model.startTour();
    c.model.handleEvent(conn({...closed, ancs_ready: false}));
    check('bus_closed on the step of the switches: told at once', c.model.view().tour.obex, 'down');
    c.model.setEngineState('absent');
    let v = c.model.view();
    check('the engine then leaves by itself: the tour stays, as after Fix It', [v.tour?.step, v.tour?.obex, v.link],
        ['switches', 'restarting', 'starting']);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn({...off, ancs_ready: false}));
    check('a new engine, services on offer: the step goes on', c.model.view().tour.obex, 'fixed');
    c.model.handleEvent(conn({...closed, ancs_ready: false}));
    check('bus still out of reach after the restart: start from scratch', c.model.view().tour.obex, 'still');
}

// --- on the step of the switches the engine is asked to try again (bt_retry_profiles)
{
    const off = {map_open: false, pbap_open: false, ancs_ready: false, map_error: 'forbidden', pbap_error: 'forbidden'};
    const retries = c => c.sent.filter(s => s.command === 'bt_retry_profiles').length;
    const c = linked();
    c.model.handleEvent(conn(off));
    check('no tour: never asked', retries(c), 0);
    c.model.startTour();
    check('entering the step: asked once', [c.model.view().tour.step, retries(c), c.model.nextDeadline()],
        ['switches', 1, T0 + RETRY_PROFILES_MS]);
    c.now += RETRY_PROFILES_MS - 1;
    c.model.expire();
    check('not before 3 s', retries(c), 1);
    c.now += 1;
    c.model.expire();
    c.now += RETRY_PROFILES_MS;
    c.model.expire();
    check('then every 3 s while a switch is missing', retries(c), 3);
    c.model.closeTour();
    c.now += 5 * RETRY_PROFILES_MS;
    c.model.expire();
    check('tour put aside: not asked', retries(c), 3);
    c.model.startTour();
    check('brought back: asked at once', retries(c), 4);
    c.model.handleEvent(conn({...off, map_error: 'other', pbap_error: 'other'}));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    const n = retries(c);
    c.now += 5 * RETRY_PROFILES_MS;
    c.model.expire();
    check('fault told: no more asking, it is not about the switches', [c.model.view().tour.obex, retries(c) - n], ['down', 0]);
    c.model.handleEvent(conn());
    c.now += 5 * RETRY_PROFILES_MS;
    c.model.expire();
    check('all on: not asked any more', retries(c) - n, 0);
}

// --- forgetting the iPhone
{
    const gone = () => [status({device_address: ''}),
        {command: 'bt_connection_changed', device_present: false, device_paired: false, bond_state: 'none', needs_repair: false}];
    let c = linked();
    check('linked: forgetting is not offered in the menu', 'forgetAction' in c.model.view().card, false);
    c.sent.length = 0;
    check('asked: accepted', c.model.startForget(), true);
    let v = c.model.view().tour;
    check('a question first, nothing sent, no "Step n of N"', [v.step, v.forget, v.total, v.open, c.sent],
        ['forget', {state: 'confirm', back: false}, 0, true, []]);
    c.model.closeTour();
    check('Escape on the question: nothing happened', [c.model.view().tour, c.model.view().link, c.sent], [null, 'linked', []]);

    c.model.startForget();
    c.model.forgetConfirm();
    check('Forget: bt_unpair for the chosen iPhone',
        [c.sent.map(s => [s.command, s.address]), c.model.view().tour.forget.state],
        [[['bt_unpair', 'AA:BB:CC:00:00:01']], 'removing']);
    const op = c.sent[0].operation_id;
    c.model.closeTour();
    check('Escape while removing: put aside, not dropped', [c.model.view().tour.open, c.model.view().tour.forget.state], [false, 'removing']);
    c.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', removed: true,
        selection_cleared: true, operation_id: 'someone-else'});
    check('a result for another request: ignored', c.model.view().tour.forget.state, 'removing');
    c.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', removed: true,
        selection_cleared: true, operation_id: op});
    for (const e of gone())
        c.model.handleEvent(e);
    v = c.model.view();
    check('removed here: now the iPhone side, by hand', [v.tour.step, v.tour.forget, v.link],
        ['forget', {state: 'phone', back: false}, 'setup']);
    c.model.startTour();
    c.model.cancelTour();
    v = c.model.view();
    check('Close: the state is clear, Link iPhone', [v.tour, v.pill.strong, v.card.kind], [null, 'Link iPhone', 'direct']);

    // Link Again
    c = linked();
    c.model.startForget();
    c.model.forgetConfirm();
    c.model.handleEvent({command: 'bt_unpair_result', success: true, status: 'unpaired', operation_id: c.sent.at(-1).operation_id});
    for (const e of gone())
        c.model.handleEvent(e);
    c.sent.length = 0;
    c.model.forgetLinkAgain();
    v = c.model.view().tour;
    check('Link Again: the pairing walk, from the search', [v.step, v.index, v.total, v.forget, c.sent.map(s => s.command)],
        ['find', 1, 6, null, ['bt_scan', 'bt_list_devices']]);
    c.model.handleEvent({command: 'bt_devices', devices: [{address: 'AA:BB:CC:00:00:01', name: 'iPhone', iphone: true}]});
    const pair = c.sent.find(s => s.command === 'bt_pair');
    check('...asking for a pairing from scratch', [pair?.address, pair?.fresh], ['AA:BB:CC:00:00:01', true]);

    // What can go wrong
    c = linked();
    c.model.startForget();
    c.model.forgetConfirm();
    c.model.handleEvent({command: 'bt_unpair_result', success: false, status: 'error', operation_id: c.sent.at(-1).operation_id});
    check('the computer could not forget: said, nothing claimed', [c.model.view().tour.forget.state, c.model.view().link], ['error', 'linked']);
    c.model.forgetConfirm();
    check('Try Again: asked again', [c.sent.filter(s => s.command === 'bt_unpair').length, c.model.view().tour.forget.state], [2, 'removing']);
    c.model.handleEvent({command: 'bt_unpair_result', success: false, status: 'not_found', operation_id: c.sent.at(-1).operation_id});
    check('"not found": already forgotten here, go on', c.model.view().tour.forget.state, 'phone');
    c = linked();
    c.model.startForget();
    c.model.forgetConfirm();
    check('a deadline for the answer', c.model.nextDeadline(), T0 + 15000);
    c.now += 15000;
    c.model.expire();
    check('no answer in 15 s: an error, not a wait without end', c.model.view().tour.forget.state, 'error');

    // From a tour that is stuck
    c = linked();
    c.model.handleEvent(conn({map_open: false, pbap_open: false, ancs_ready: false, map_error: 'forbidden', pbap_error: 'forbidden'}));
    c.model.startTour();
    c.model.startForget();
    v = c.model.view().tour;
    check('from the step of the switches: the question, Back offered', [v.step, v.forget], ['forget', {state: 'confirm', back: true}]);
    c.model.forgetBack();
    v = c.model.view().tour;
    check('Back: the step it came from', [v.step, v.forget, v.total], ['switches', null, 3]);
    c.model.startForget();
    c.model.closeTour();
    check('Escape on the question: back too', c.model.view().tour.step, 'switches');

    // Nothing to forget, engine away
    c = setup();
    c.model.setEngineState('connected');
    c.model.handleEvent(status({device_address: ''}));
    c.model.handleEvent(gone()[1]);
    c.model.startForget();
    c.model.forgetConfirm();
    check('nothing known here: no command, straight to the iPhone side', [c.sent, c.model.view().tour.forget.state], [[], 'phone']);
    c = setup();
    c.model.setEngineState('absent');
    check('engine away: refused', [c.model.startForget(), c.model.view().tour], [false, null]);
}

// --- New Message…
{
    const c = linked();
    check('linked, messages readable: the menu offers New Message…', c.model.view().card.composeAction, true);
    c.model.cardOpened();
    c.sent.length = 0;
    c.model.startCompose();
    let card = c.model.view().card;
    check('the search card, empty', [card.kind, card.query, card.results], ['compose', '', []]);
    c.model.composeQuery('cam');
    check('typing asks the engine for matching contacts', c.sent, [{command: 'bt_list_contacts', query: 'cam', limit: 6}]);
    c.model.handleEvent({command: 'bt_contacts', query: 'ca', contacts: [{name: 'Old', addresses: ['tel:+41790000009']}]});
    check('an answer to an older query: ignored', c.model.view().card.results, []);
    c.model.handleEvent({command: 'bt_contacts', query: 'cam', contacts: [
        {name: 'Camille Martin', favorite: true, addresses: ['tel:+41790000001', 'camille@exemple.ch']}]});
    card = c.model.view().card;
    check('one row per address, number readable',
        card.results.map(r => [r.name, r.detail, r.initials, r.thread]),
        [['Camille Martin', '+41 79 000 00 01', 'CM', 'tel:+41790000001'], ['Camille Martin', 'camille@exemple.ch', 'CM', 'camille@exemple.ch']]);
    c.sent.length = 0;
    c.model.composePick(0);
    card = c.model.view().card;
    check('picked: the reply card on that conversation, nothing in it yet',
        [card.kind, card.thread, card.name, card.messages, c.model.view().pill.text],
        ['reply', 'tel:+41790000001', 'Camille Martin', [], 'New message']);
    c.model.reply('Salut !');
    check('written: sent to that address', c.sent.map(s => [s.command, s.thread, s.body]), [['bt_send_message', 'tel:+41790000001', 'Salut !']]);
    c.model.cardClosed();
    c.model.cardOpened();
    c.model.startCompose();
    c.model.composeQuery('+41 76 555 01 42');
    card = c.model.view().card;
    check('a full number: can be written to without being a contact',
        card.results.map(r => [r.name, r.thread]), [['+41 76 555 01 42', 'tel:+41765550142']]);
    c.model.composePick(0);
    check('...and opens under the number', [c.model.view().card.kind, c.model.view().card.name], ['reply', '+41 76 555 01 42']);
    c.model.cardClosed();
    c.model.startCompose();
    c.model.cardClosed();
    check('closed: back to the menu', c.model.view().card.kind, 'menu');
    c.model.handleEvent(conn({map_open: false, map_error: 'forbidden'}));
    check('messages not readable: not offered', c.model.view().card.composeAction, false);
}

// --- the words of the iPhone, in its language
{
    const en = ['en_US.UTF-8', 'en_US', 'en', 'C'];
    const fr = ['fr_FR.UTF-8', 'fr_FR', 'fr', 'C'];
    check('desktop in French: French iPhone', phoneLanguage('', fr, fr), 'fr');
    check('desktop in English with French formats: French iPhone', phoneLanguage('', en, fr), 'fr');
    check('desktop in English all through: English', phoneLanguage('', en, en), 'en');
    check('a language without labels: English', phoneLanguage('', ['de_CH.UTF-8', 'de'], ['de_CH']), 'en');
    check('the choice of Phone Settings wins', [phoneLanguage('en', fr, fr), phoneLanguage('fr', en, en)], ['en', 'fr']);
    check('an unknown choice: guessed', phoneLanguage('xx', fr, fr), 'fr');
    check('English text, French iPhone: the French word in brackets', bilingual('fr', 'pair'), 'Pair (Jumeler)');
    check('same word in both: said once', bilingual('fr', 'bluetooth'), 'Bluetooth');
    check('English iPhone: no brackets', bilingual('en', 'forget'), 'Forget This Device');
    check('every language has every label',
        Object.keys(PHONE_LABELS).map(l => Object.keys(PHONE_LABELS[l]).join()), Object.keys(PHONE_LABELS).map(() => Object.keys(PHONE_LABELS.en).join()));
}

// --- a steady indicator: nothing blinks, nothing swings
{
    const up = (extra = {}) => conn(extra);
    const down = (extra = {}) => conn({classic_connected: false, le_connected: false, map_open: false,
        pbap_open: false, ancs_ready: false, battery: null, ...extra});
    // Links up, but no service has opened yet.
    const bare = (extra = {}) => conn({map_open: false, pbap_open: false, ancs_ready: false, ...extra});
    const at = (c, ms) => {
        c.now = T0 + ms;
    };
    const tick = (c, ms) => {
        at(c, ms);
        c.model.expire();
    };

    let c = setup(true);
    check('steady: nothing claimed at first', c.model.view().link, 'starting');
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    check('status alone could mean "out of range": not shown yet', c.model.view().link, 'starting');
    check('...and the model asks to be called back', c.model.nextDeadline(), T0 + LINK_SETTLE_MS);
    c.model.handleEvent(up());
    check('links up and a service open: linked at once', c.model.view().pill, {tone: 'quiet'});
    check('...nothing left to wait for', c.model.nextDeadline(), null);

    at(c, 10000);
    c.model.handleEvent(down());
    check('links drop: still shown linked', [c.model.view().link, c.model.view().icon.state], ['linked', 'linked']);
    tick(c, 10000 + LINK_SETTLE_MS - 1);
    check('...until the drop has held 3 s', c.model.view().link, 'linked');
    tick(c, 10000 + LINK_SETTLE_MS);
    check('held 3 s: out of range', [c.model.view().link, c.model.view().icon.state], ['away', 'away']);

    at(c, 20000);
    c.model.handleEvent(bare());
    check('links back, no service yet: not claimed', c.model.view().link, 'away');
    at(c, 21000);
    c.model.handleEvent(down());
    tick(c, 26000);
    check('a link that came and went within 3 s never showed', c.model.view().link, 'away');
    at(c, 40000);
    c.model.handleEvent(bare());
    tick(c, 40000 + LINK_SETTLE_MS);
    check('links back that hold 3 s, no service yet: reconnecting', c.model.view().link, 'reconnecting');
    tick(c, 40000 + RECONNECT_SETUP_MS);
    tick(c, 40000 + RECONNECT_SETUP_MS + LINK_SETTLE_MS);
    check('...and a link after 15 s, service or not', c.model.view().link, 'linked');

    // A state the user must see is not delayed when nothing was shown yet.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status({device_address: ''}));
    check('no iPhone chosen, from nothing: said at once', c.model.view().pill.strong, 'Link iPhone');

    // A message and a call never wait.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    c.model.handleEvent(message());
    check('steady: a message shows at once', c.model.view().pill.strong, 'Camille');
    c.model.handleEvent(call('incoming'));
    check('steady: a call shows at once', c.model.view().mode, 'ring');

    // The defect seen for real: links up, down, up, down.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    const seen = new Set();
    let ms = 5000;
    for (let i = 0; i < 3; i++) {
        at(c, ms);
        c.model.handleEvent(down());
        seen.add(c.model.view().link);
        ms += 2000;
        at(c, ms);
        c.model.handleEvent(bare());
        seen.add(c.model.view().link);
        ms += 2000;
    }
    check('links flapping every 2 s: the indicator never swung', [...seen], ['linked']);
    let v = c.model.view();
    check('three drops within 30 s: not shown yet', [v.link, v.unsteady], ['linked', null]);
    tick(c, ms + LINK_SETTLE_MS);
    v = c.model.view();
    check('...then it settles on Link again', [v.link, v.pill.strong, v.unsteady, v.card],
        ['broken', 'Link again', 'flapping', {kind: 'direct', action: 'link'}]);
    ms += 10000;
    for (let i = 0; i < 4; i++) {
        at(c, ms);
        c.model.handleEvent(i % 2 ? bare() : down());
        c.model.expire();
        seen.add(c.model.view().pill.strong);
        ms += 4000;
    }
    check('links the iPhone never serves: it stays there', [...seen], ['linked', 'Link again']);
    // Bare links are up since the last event (ms - 4000).
    tick(c, ms - 4000 + STEADY_MS - 1);
    check('bare for less than 60 s: still Link again', c.model.view().link, 'broken');
    tick(c, ms - 4000 + STEADY_MS);
    tick(c, ms - 4000 + STEADY_MS + LINK_SETTLE_MS);
    check('up for 60 s without a drop: linked again', [c.model.view().link, c.model.view().unsteady], ['linked', null]);

    // 18 November, 02:07-02:15: drops while walking out and back in, then a
    // link the iPhone serves. Away while out of range, linked at once after.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    for (const t of [1000, 3000, 5000]) {
        at(c, t);
        c.model.handleEvent(down());
        at(c, t + 1000);
        c.model.handleEvent(bare());
    }
    at(c, 7000);
    c.model.handleEvent(down());
    c.model.handleEvent({command: 'bt_presence', state: 'away', reason: 'out_of_range', lock: 'off'});
    tick(c, 7000 + 2 * LINK_SETTLE_MS);
    v = c.model.view();
    check('walk out: shown away, not Link again', [v.link, v.pill.strong === 'Link again'], ['away', false]);
    at(c, 60000);
    c.model.handleEvent({command: 'bt_presence', state: 'here', lock: 'none'});
    c.model.handleEvent(up());
    tick(c, 60000 + LINK_SETTLE_MS);
    v = c.model.view();
    check('back with notifications answered: linked, latch gone', [v.link, v.unsteady, v.pill.strong === 'Link again'],
        ['linked', null, false]);

    // Two drops are not three.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    for (const t of [1000, 3000]) {
        at(c, t);
        c.model.handleEvent(down());
        at(c, t + 1000);
        c.model.handleEvent(up());
    }
    tick(c, 20000);
    check('two drops in 30 s: no latch', [c.model.view().link, c.model.view().unsteady], ['linked', null]);
    at(c, 5000 + FLAP_WINDOW_MS);
    c.model.handleEvent(down());
    at(c, 6000 + FLAP_WINDOW_MS);
    c.model.handleEvent(up());
    tick(c, 20000 + FLAP_WINDOW_MS);
    check('a third drop after the window: still no latch', c.model.view().link, 'linked');

    // The user takes it in hand: the click goes to the repair.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(devices([phone({paired: true, bonded: true, bond_state: 'ok'})]));
    c.model.handleEvent(up());
    for (const t of [1000, 3000, 5000]) {
        at(c, t);
        c.model.handleEvent(down());
        at(c, t + 1000);
        c.model.handleEvent(bare());
    }
    tick(c, 6000 + LINK_SETTLE_MS);
    check('flapping with a known pairing: Link again', c.model.view().pill.strong, 'Link again');
    c.model.startTour();
    v = c.model.view();
    check('click while the links happen to be up: the repair, not "all is well"', [v.tour?.step, v.tour?.repair], ['repair', 'ask']);
    c.model.handleEvent(up());
    check('...and links coming up do not whisk the repair away', c.model.view().tour.step, 'repair');
    c.model.cancelTour();
    tick(c, 6000 + 2 * LINK_SETTLE_MS + 1);
    check('Not Now: the latch is gone with the click, the link is shown as it is', c.model.view().link, 'linked');

    // needs_repair: never "linked", and no swinging when it comes and goes.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    at(c, 1000);
    c.model.handleEvent(up({needs_repair: true, bond_state: 'broken'}));
    tick(c, 1000 + LINK_SETTLE_MS);
    check('needs_repair: Link again', [c.model.view().pill.strong, c.model.view().unsteady], ['Link again', 'repair']);
    at(c, 10000);
    c.model.handleEvent(up({needs_repair: false, bond_state: 'ok'}));
    tick(c, 20000);
    check('needs_repair gone for 10 s: still Link again', c.model.view().pill.strong, 'Link again');
    tick(c, 10000 + STEADY_MS);
    tick(c, 10000 + STEADY_MS + LINK_SETTLE_MS);
    check('sound for 60 s: linked', c.model.view().link, 'linked');

    // The engine going away is not the link dropping.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(up());
    for (let i = 0; i < 4; i++) {
        at(c, 1000 + i * 2000);
        c.model.setEngineState('absent');
        c.model.setEngineState('connected');
        c.model.handleEvent(status());
        c.model.handleEvent(up());
    }
    tick(c, 20000);
    check('engine restarts are not drops', [c.model.view().link, c.model.view().unsteady], ['linked', null]);
}

// --- silence: Do Not Disturb, a shared screen, the mute switch
{
    let c = linked();
    c.model.setQuiet({dnd: true});
    c.model.handleEvent(message());
    let v = c.model.view();
    check('Do Not Disturb, a message: no pill, no name, a bare count', [v.mode, v.pill, v.quiet],
        ['idle', {tone: 'quiet'}, 'dnd']);
    check('...nothing is waiting to pop up later', c.model.nextDeadline(), null);
    check('...the menu stays quiet: GNOME shows Do Not Disturb itself', v.card.notices, []);
    check('...and still lists the conversation', v.card.threads.map(t => [t.name, t.unread]), [['Camille Martin', true]]);
    c.model.handleEvent(call('incoming'));
    v = c.model.view();
    check('Do Not Disturb, a call: the bar does not move', [v.mode, v.pill], ['idle', {tone: 'quiet'}]);
    check('...a menu opened on purpose can still answer', v.card.kind, 'ring');
    c.model.handleEvent(call('active'));
    v = c.model.view();
    check('answered on the iPhone meanwhile: no name in the bar either', [v.mode, v.pill.strong, v.card.kind], ['idle', undefined, 'call']);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.handleEvent(conn({battery: 9}));
    check('Do Not Disturb: no battery warning', c.model.view().pill, {tone: 'quiet'});
    c.model.setQuiet({dnd: false});
    v = c.model.view();
    check('Do Not Disturb off: nothing is played again, the count is enough', [v.mode, v.pill, v.quiet],
        ['idle', {tone: 'quiet'}, null]);
    c.model.handleEvent(message({handle: 'H2', body: 'Et maintenant ?'}));
    check('...and the next message shows as usual', c.model.view().pill.text, 'Et maintenant ?');

    // A pill already on screen when silence begins.
    c = linked();
    c.model.handleEvent(message());
    c.model.setQuiet({sharing: true});
    v = c.model.view();
    check('screen shared while a message pill is up: it goes, counted', [v.pill, v.quiet],
        [{tone: 'quiet'}, 'sharing']);
    c.model.setQuiet({sharing: false});
    check('...and does not come back when the sharing stops', c.model.view().unread, 1);

    // Calls through Do Not Disturb: a choice, and only for Do Not Disturb.
    c = linked();
    c.model.setQuiet({dnd: true, callsThroughDnd: true});
    c.model.handleEvent(call('incoming'));
    check('calls let through Do Not Disturb: the call shows', c.model.view().mode, 'ring');
    c.model.handleEvent(message());
    c.model.handleEvent({command: 'bt_calls', calls: []});
    check('...messages still do not', c.model.view().pill, {tone: 'quiet'});
    c.model.setQuiet({sharing: true});
    c.model.handleEvent(call('incoming'));
    check('...and never while the screen is shared', c.model.view().mode, 'idle');

    // The choices of Phone Settings.
    c = linked();
    c.model.setQuiet({dnd: true, sharing: true, followDnd: false, whileSharing: false});
    c.model.handleEvent(message());
    check('told not to follow Do Not Disturb nor sharing: shown', [c.model.view().mode, c.model.view().quiet], ['message', null]);
    c.model.setQuiet({muted: true});
    v = c.model.view();
    check('Mute iPhone on this computer: complete silence', [v.mode, v.pill, v.quiet, v.card.muted],
        ['idle', {tone: 'quiet'}, 'muted', true]);
    c.model.handleEvent(call('incoming'));
    check('...calls included', c.model.view().mode, 'idle');

    // Notifications of other apps: listed, no banner.
    c = linked();
    const note = {command: 'bt_notification', uid: 5, app_id: 'net.whatsapp.WhatsApp', app_name: 'WhatsApp',
        title: 'Camille Martin', body: 'Salut', silent: false, negative_action: true};
    c.model.handleEvent(note);
    c.model.setQuiet({sharing: true});
    c.model.handleEvent({...note, uid: 6, body: 'Tu es là ?'});
    check('other apps: a banner normally, none while quiet', c.shown.map(([, n]) => n.silent), [false, true]);

    // A message older than this session with the engine is counted, not played.
    c = setup();
    c.now = T0 + 600000;
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    c.model.handleEvent(message({timestamp: (T0 + 300000) / 1000}));
    check('a message that arrived before the Shell came back: count only', c.model.view().pill, {tone: 'quiet'});
}

// --- daily life: sound, battery, presence (docs/moteur-quotidien.md)
{
    const audio = (extra = {}) => ({command: 'bt_audio', phone_audio: 'phone', phone_audio_route: 'phone',
        call_audio: 'phone', call_audio_route: 'phone', call_audio_switchable: true, call_audio_reject: true, ...extra});
    const told = [];
    const fresh = () => {
        const c = linked();
        c.settings = [];
        c.model.onSetting = (name, value) => c.settings.push([name, value]);
        return c;
    };

    // An older engine: nothing new is offered, nothing breaks.
    let c = fresh();
    let v = c.model.view();
    check('older engine: no audio switch in the menu', v.card.phoneAudioHere, null);
    c.model.setPhoneAudio(true);
    c.model.wantSetting('lock-on-away', true);
    check('older engine: nothing is sent for what it cannot say', c.sent.filter(x => /audio|lock/.test(x.command)), []);
    c.model.handleEvent(call('incoming'));
    check('older engine: one Answer', c.model.view().card.answerChoice, false);
    c.sent.length = 0;
    c.model.answer();
    c.model.handleEvent(call('active'));
    c.model.setCallAudio('computer');
    check('older engine: the old commands', c.sent, [
        {command: 'bt_call_action', action: 'answer', path: '/call1'},
        {command: 'bt_call_action', action: 'audio_here'}]);

    // Music: "Play iPhone audio on this computer".
    c = fresh();
    c.model.handleEvent(audio());
    check('bt_audio: the menu has the switch, off', c.model.view().card.phoneAudioHere, false);
    check('...and Phone Settings is told what the engine holds', c.settings, [['phone-audio-here', false]]);
    c.sent.length = 0;
    c.model.setPhoneAudio(true);
    check('switch on: asked of the engine, shown at once', [c.sent, c.model.view().card.phoneAudioHere],
        [[{command: 'bt_set_phone_audio', route: 'here'}], true]);
    c.sent.length = 0;
    c.model.wantSetting('phone-audio-here', true);
    check('the same value again: nothing sent', c.sent, []);
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'here'}));
    c.model.handleEvent(audio({phone_audio: 'phone'}));
    check('changed elsewhere: Phone Settings follows', c.settings[c.settings.length - 1], ['phone-audio-here', false]);

    // #38, lean link mode: the classic link is closed, the engine says the
    // route is 'unavailable'. Turning it on is what opens the link.
    const lean = () => {
        const d = setup();
        d.settings = [];
        d.model.onSetting = (name, value) => d.settings.push([name, value]);
        d.model.setEngineState('connected');
        d.model.handleEvent(status({link_mode: 'lean', link_mode_effective: 'lean', classic_state: 'idle',
            classic_leases: [], adapters: [{name: 'studio-pc', path: '/org/bluez/hci0'}]}));
        d.model.handleEvent(conn({classic_connected: false, map_open: false, pbap_open: false}));
        d.model.handleEvent(audio({phone_audio: 'auto', phone_audio_route: 'unavailable'}));
        return d;
    };
    c = lean();
    v = c.model.view();
    check('lean, link closed: the switch is there, off, nothing said under it',
        [v.link, v.card.phoneAudioHere, v.card.phoneAudio], ['linked', false, {state: 'off', computer: 'studio-pc', hint: false}]);
    c.sent.length = 0;
    c.model.cardOpened();
    c.model.setPhoneAudio(true);
    check('turned on: asked of the engine, Phone Settings told, "Connecting…"',
        [c.sent.filter(x => /audio/.test(x.command)), c.settings[c.settings.length - 1], c.model.view().card.phoneAudio.state],
        [[{command: 'bt_set_phone_audio', route: 'here'}], ['phone-audio-here', true], 'connecting']);
    c.model.handleEvent(audio({phone_audio: 'auto', phone_audio_route: 'unavailable'}));
    check('an older bt_audio on its way back does not put the switch back off',
        [c.model.view().card.phoneAudioHere, c.model.view().card.phoneAudio.state,
            c.settings.filter(([name]) => name === 'phone-audio-here').pop()],
        [true, 'connecting', ['phone-audio-here', true]]);
    c.now += 3000;
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'unavailable'}));
    c.model.handleEvent(status({link_mode: 'lean', link_mode_effective: 'lean', classic_state: 'opening',
        classic_leases: ['audio'], adapters: [{name: 'studio-pc'}]}));
    check('the engine holds it, the link opens: still "Connecting…", a deadline is set',
        [c.model.view().card.phoneAudio.state, c.model.nextDeadline() <= c.now + PHONE_AUDIO_WAIT_MS], ['connecting', true]);
    c.now += 4000;
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'here'}));
    v = c.model.view();
    check('bt_audio says here: "Playing on this computer", how to choose it on the iPhone, with its name',
        v.card.phoneAudio, {state: 'playing', computer: 'studio-pc', hint: true});
    c.model.cardClosed();
    c.model.cardOpened();
    check('the menu closed once on it: the explanation is not given again', c.model.view().card.phoneAudio.hint, false);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, le_available: false, ancs_ready: false,
        map_open: false, pbap_open: false, device_present: true}));
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'unavailable'}));
    check('the iPhone gone: the choice stays, nothing is waited for',
        [c.model._phoneAudioHere(), c.model._phoneAudioState(), c.model._phoneAudioWait], [true, 'away', null]);
    c.model.handleEvent(conn({classic_connected: false, map_open: false, pbap_open: false}));
    check('back (#43): the link is not held for the choice; it stands by, nothing is waited for',
        c.model.view().card.phoneAudio.state, 'standby');
    c.sent.length = 0;
    c.model.connectPhoneAudio();
    check('"Connect for Sound": asked of the engine again, connecting',
        [c.sent, c.model.view().card.phoneAudio.state], [[{command: 'bt_set_phone_audio', route: 'here'}], 'connecting']);
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'here'}));
    check('then playing', c.model.view().card.phoneAudio.state, 'playing');
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'unavailable'}));
    check('the engine lets the link go after a silence: standby, no failure', c.model.view().card.phoneAudio.state, 'standby');
    c.model.setPhoneAudio(false);
    c.sent.length = 0;
    c.model.connectPhoneAudio();
    check('"Connect for Sound" with the choice off: nothing', c.sent, []);

    // Never comes: said, with what to do.
    c = lean();
    c.model.setPhoneAudio(true);
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'unavailable'}));
    c.now += PHONE_AUDIO_WAIT_MS - 1;
    c.model.expire();
    check('just before the delay: still connecting, the end of the wait is a deadline',
        [c.model.view().card.phoneAudio.state, c.model.nextDeadline()], ['connecting', c.now + 1]);
    c.now += 1;
    c.model.expire();
    check('the delay over: failed, the switch stays on (the choice is kept)',
        [c.model.view().card.phoneAudio.state, c.model.view().card.phoneAudioHere], ['failed', true]);
    c.model.setPhoneAudio(false);
    c.model.handleEvent(audio({phone_audio: 'phone', phone_audio_route: 'unavailable'}));
    check('off: nothing said', c.model.view().card.phoneAudio.state, 'off');
    c.model.setPhoneAudio(true);
    check('on again: a new wait', c.model.view().card.phoneAudio.state, 'connecting');
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'phone'}));
    check('the link open, the sound not yet here: connecting', c.model.view().card.phoneAudio.state, 'connecting');
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'here'}));
    check('then playing', c.model.view().card.phoneAudio.state, 'playing');

    // #43, 18 November 18:18: lean mode, the LE link gone, the classic link
    // still up and the notifications live over it: linked, not paused.
    c = lean();
    c.model.handleEvent(conn({le_connected: false, classic_connected: true, ancs_ready: true,
        map_open: true, pbap_open: true, le_state: 'paused', le_cause: 'phone_silent'}));
    v = c.model.view();
    check('LE gone, notifications over the classic link: linked, no pause said',
        [v.link, v.card.paused ?? null], ['linked', null]);
    c.model.handleEvent(conn({le_connected: false, classic_connected: false, ancs_ready: false,
        map_open: false, pbap_open: false, le_state: 'paused', le_cause: 'phone_silent'}));
    v = c.model.view();
    check('...the classic link closed too: no longer linked, the pause and its cause said',
        [v.link === 'linked', c.model._notificationsPaused()?.cause ?? null], [false, 'phone_silent']);

    // A computer without a named adapter: the host name.
    c = setup();
    c.model = new Model({send: () => {}, now: () => c.now, hostName: 'portable'});
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    c.model.handleEvent(audio({phone_audio: 'here', phone_audio_route: 'here'}));
    check('no adapter name: the host name', c.model.view().card.phoneAudio.computer, 'portable');

    // Calls: two ways to answer.
    c = fresh();
    c.model.handleEvent(audio());
    c.model.handleEvent(call('incoming'));
    v = c.model.view();
    check('the sound can come here: Answer here / Answer on iPhone', [v.card.kind, v.card.answerChoice], ['ring', true]);
    c.sent.length = 0;
    c.model.answer('computer');
    check('Answer here: answered with the sound here', c.sent,
        [{command: 'bt_call_action', action: 'answer', path: '/call1', audio: 'here'}]);
    c.model.handleEvent(call('active'));
    v = c.model.view();
    check('...shown as on its way, not as done', [v.card.audio, v.card.moving, v.card.subtitle, v.card.hint],
        ['computer', true, '00:00 · moving to this computer…', '']);
    c.model.handleEvent(conn({calls: {audio: 'active', audio_route: 'here'}}));
    v = c.model.view();
    check('...the engine says the sound is here: said so', [v.card.audio, v.card.moving, v.card.subtitle],
        ['computer', false, '00:00 · sound on this computer']);
    c.model.handleEvent(conn({calls: {audio: 'idle', audio_route: 'phone'}}));
    check('...moved on the iPhone itself: the card follows the engine, not the click',
        c.model.view().card.audio, 'phone');
    c.model.handleEvent({command: 'bt_calls', calls: []});
    // Answer here, and the sound never comes.
    c.model.handleEvent(call('incoming'));
    c.model.answer('computer');
    c.model.handleEvent(call('active'));
    c.now += AUDIO_ANSWER_MS - 1;
    c.model.expire();
    check('Answer here, no sound yet: still on its way', c.model.view().card.moving, true);
    c.now += 1;
    c.model.expire();
    v = c.model.view();
    check('Answer here, no sound after 6 s: said, and shown where it is',
        [v.card.audio, v.card.moving, v.card.isError, v.card.subtitle, v.card.hint],
        ['phone', false, true, 'Couldn’t move the call to this computer', 'The sound is still on the iPhone.']);
    c.model.handleEvent(conn({calls: {audio: 'active', audio_route: 'here'}}));
    v = c.model.view();
    check('...it comes after all: the error goes', [v.card.audio, v.card.isError, v.card.hint], ['computer', false, '']);
    c.model.handleEvent(conn({calls: {audio: 'idle', audio_route: 'phone'}}));
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.now = T0;
    c.model.handleEvent(call('incoming'));
    c.sent.length = 0;
    c.model.answer('phone');
    check('Answer on iPhone: answered with the sound there', c.sent,
        [{command: 'bt_call_action', action: 'answer', path: '/call1', audio: 'phone'}]);
    c.model.handleEvent(call('active'));
    v = c.model.view();
    check('...with a word about a silent call', [v.card.audio, /Tap Audio/.test(v.card.hint)], ['phone', true]);
    c.sent.length = 0;
    c.model.setCallAudio('computer');
    check('This computer, during the call: for this call only',
        c.sent, [{command: 'bt_set_call_audio', route: 'here', persist: false}]);
    check('...the model asks to be called back', c.model.nextDeadline() !== null, true);
    c.model.handleEvent(conn({calls: {audio: 'active', audio_route: 'here'}}));
    v = c.model.view();
    check('the engine confirms: sound on this computer, no hint', [v.card.audio, v.card.hint, v.card.isError], ['computer', '', false]);
    c.sent.length = 0;
    c.model.setCallAudio('phone');
    v = c.model.view();
    check('iPhone, during the call: asked, shown as on its way',
        [c.sent, v.card.audio, v.card.moving, v.card.subtitle],
        [[{command: 'bt_set_call_audio', route: 'phone', persist: false}], 'phone', true, '00:00 · moving to iPhone…']);
    check('...called back after 3 s', c.model.nextDeadline() <= c.now + AUDIO_MOVE_MS, true);
    c.model.handleEvent({command: 'bt_call_audio_result', success: true, route: 'phone', persist: false, applied_now: true});
    check('...the result alone proves nothing: still on its way', c.model.view().card.moving, true);
    c.now = T0 + AUDIO_MOVE_MS - 1;
    c.model.expire();
    check('...not given up on before 3 s', c.model.view().card.isError, false);
    c.now = T0 + AUDIO_MOVE_MS;
    c.model.expire();
    v = c.model.view();
    check('back to the iPhone does not happen in 3 s: said, with what to do, and the true state',
        [v.card.audio, v.card.moving, v.card.isError, v.card.subtitle, /tap Audio on the iPhone/.test(v.card.hint)],
        ['computer', false, true, 'Couldn’t move the call to the iPhone', true]);
    c.model.handleEvent({command: 'bt_call_result', action: 'tones', success: true});
    check('...another action that works does not hide it', c.model.view().card.isError, true);
    c.model.handleEvent(conn({calls: {audio: 'idle', audio_route: 'phone'}}));
    v = c.model.view();
    check('...it moves after all: the error goes', [v.card.audio, v.card.isError, /still on this computer/.test(v.card.hint)],
        ['phone', false, false]);
    // The engine refuses outright: no need to wait.
    c.model.handleEvent(conn({calls: {audio: 'active', audio_route: 'here'}}));
    c.model.setCallAudio('phone');
    c.model.handleEvent({command: 'bt_call_audio_result', success: false, route: 'phone', persist: false,
        applied_now: false, message: 'The call audio is still on this computer.'});
    v = c.model.view();
    check('the engine says the sound did not move: said at once',
        [v.card.audio, v.card.moving, v.card.isError, v.card.subtitle],
        ['computer', false, true, 'Couldn’t move the call to the iPhone']);
    c.model.setCallAudio('phone');
    v = c.model.view();
    check('...asking again starts over', [v.card.isError, v.card.moving], [false, true]);
    c.model.handleEvent(conn({calls: {audio: 'idle', audio_route: 'phone'}}));
    check('...and this time it moved', [c.model.view().card.audio, c.model.view().card.moving], ['phone', false]);
    c.model.setCallAudio('computer');
    c.now += AUDIO_MOVE_MS;
    c.model.expire();
    v = c.model.view();
    check('to this computer does not happen: said too',
        [v.card.audio, v.card.isError, v.card.subtitle], ['phone', true, 'Couldn’t move the call to this computer']);
    c.model.handleEvent(audio({call_audio_switchable: false}));
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.handleEvent(call('incoming'));
    check('this computer cannot carry call sound: one Answer', c.model.view().card.answerChoice, false);

    // Battery: the icon says it once per crossing, until the menu is opened.
    const batteryTold = () => {
        const state = c.model.view().icon.state;
        c.model.cardOpened();
        c.model.cardClosed();
        return state;
    };
    c = fresh();
    const low = (extra = {}) => ({command: 'bt_battery_low', level: 20, threshold: 20, state: 'low', source: 'le', initial: false, ...extra});
    c.model.handleEvent(conn({battery: 20, battery_state: 'low'}));
    c.model.handleEvent(low());
    check('bt_battery_low: the icon, and the battery red in the menu',
        [c.model.view().icon.state, c.model.view().mode, c.model.view().card.batteryLow], ['battery', 'idle', true]);
    batteryTold();
    c.model.handleEvent(low());
    c.model.handleEvent(low({level: 19}));
    check('told twice: said once', batteryTold(), 'linked');
    c.model.handleEvent(conn({battery: 12, battery_state: 'low'}));
    check('an engine that announces: no pill at 12 %', c.model.view().pill, {tone: 'quiet'});
    c.model.handleEvent(low({level: 10, threshold: 10, state: 'critical'}));
    check('10 %: said a second time', batteryTold(), 'battery');
    c.model.handleEvent(conn({battery: 30, battery_state: 'ok'}));
    c.model.handleEvent(low());
    check('recharged past 25 %, low again: told again', batteryTold(), 'battery');
    c = fresh();
    c.model.handleEvent(low({level: 8, threshold: 10, state: 'critical', initial: true}));
    check('already low when the engine started: told', batteryTold(), 'battery');
    c.model.handleEvent(low({level: 8, threshold: 20}));
    check('...once, for the lower mark', batteryTold(), 'linked');
    c = fresh();
    c.model.batteryAlerts = false;
    c.model.handleEvent(low());
    check('alerts switched off in Phone Settings: nothing', [batteryTold(), c.model.view().card.notices], ['linked', []]);
    c = fresh();
    c.model.setQuiet({dnd: true});
    c.model.handleEvent(low());
    check('Do Not Disturb: the icon stays at rest', c.model.view().icon.state, 'linked');
    c = fresh();
    c.model.handleEvent(conn({battery: 12}));
    check('older engine (no battery_state): the icon warns too', c.model.view().icon.state, 'battery');

    // Presence: why it is not linked, and the lock.
    c = fresh();
    const gone = conn({classic_connected: false, le_connected: false, battery: null});
    const presence = (extra = {}) => ({command: 'bt_presence', state: 'away', reason: 'out_of_range',
        away_seconds: 3, lock: 'off', lock_in_seconds: 0, wake: false, ...extra});
    c.model.handleEvent(gone);
    check('no bt_presence: Out of range', c.model.view().card.notices, ['Out of range']);
    c.model.handleEvent(presence({state: 'here', reason: '', lock: 'none'}));
    check('links down but the engine still reaches it: reconnecting, and why',
        [c.model.view().link, c.model.view().card.notices], ['reconnecting', ['Your iPhone is back, setting up the link…']]);
    c.model.handleEvent(presence());
    check('away, out of range', c.model.view().card.notices, ['Out of range']);
    c.model.handleEvent(presence({reason: 'switched_off', lock: 'spared'}));
    check('Bluetooth cut on the iPhone: one line says so', c.model.view().card.notices, ['Bluetooth is off on the iPhone']);
    c.model.handleEvent(presence({lock: 'pending', lock_in_seconds: 27}));
    check('about to lock: the same line', c.model.view().card.notices, ['Out of range · this computer locks shortly']);
    c.model.handleEvent(presence({lock: 'locked'}));
    check('locked', c.model.view().card.notices, ['Out of range · this computer was locked']);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null, le_after_reset: 'computer'}));
    check('#59: another device connected, the computer\'s Bluetooth to restart',
        c.model.view().card.notices, ['Turn Bluetooth off and on on this computer to link again.']);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null, le_after_reset: 'phone'}));
    check('#59: restarted already, the iPhone\'s turn',
        c.model.view().card.notices, ['Turn Bluetooth off and on on the iPhone to link again.']);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null, le_after_reset: false}));
    check('#59: the hint goes with the flag', c.model.view().card.notices, ['Out of range · this computer was locked']);
    c.model.handleEvent(presence({reason: 'computer_suspended', lock: 'spared'}));
    check('back from sleep: reconnecting, one line',
        [c.model.view().link, c.model.view().card.notices], ['reconnecting', ['Reconnecting after sleep…']]);
    c.model.handleEvent(presence({state: 'here', reason: '', lock: 'none'}));
    check('reconnecting: one line',
        c.model.view().card.notices, ['Your iPhone is back, setting up the link…']);

    // The lock setting lives in the engine; Phone Settings mirrors it.
    c = fresh();
    c.model.handleEvent(status({lock_on_away: false, lock_away_seconds: 30}));
    check('bt_status: Phone Settings is told', c.settings, [['lock-on-away', false], ['lock-away-seconds', 30]]);
    c.sent.length = 0;
    c.model.wantSetting('lock-on-away', true);
    check('turned on in Phone Settings: both fields sent', c.sent,
        [{command: 'bt_set_lock_on_away', enabled: true, grace_seconds: 30}]);
    c.settings.length = 0;
    c.model.handleEvent(status({lock_on_away: false, lock_away_seconds: 30}));
    check('an older status still on its way: not taken for a change', c.settings, [['lock-away-seconds', 30]]);
    c.model.handleEvent(status({lock_on_away: true, lock_away_seconds: 30}));
    check('confirmed', c.settings[c.settings.length - 2], ['lock-on-away', true]);
    c.sent.length = 0;
    c.model.wantSetting('lock-away-seconds', 60);
    check('delay changed: sent with the switch as it is', c.sent,
        [{command: 'bt_set_lock_on_away', grace_seconds: 60, enabled: true}]);
}

// --- "Reply in app": only on a sure thing
{
    check('WhatsApp link', replyLink('net.whatsapp.WhatsApp', '41790000001'),
        {uri: 'https://web.whatsapp.com/send?phone=41790000001', scheme: 'https'});
    check('Telegram link', replyLink('ph.telegra.Telegraph', '41790000001'),
        {uri: 'tg://resolve?phone=41790000001', scheme: 'tg'});
    check('Signal link', replyLink('org.whispersystems.signal', '41790000001').uri, 'sgnl://signal.me/#p/+41790000001');
    check('an app with no reliable link: none', replyLink('com.hammerandchisel.discord', '41790000001'), null);
    check('not a number: no link', replyLink('net.whatsapp.WhatsApp', 'toString'), null);
    check('one person', replySender('Camille Martin', ''), {name: 'Camille Martin'});
    check('a subtitle means a group: nobody', replySender('Les voisins', 'Camille Martin'), null);
    check('"Name @ Group": nobody', replySender('Camille @ Les voisins', ''), null);
    check('a stranger\'s nickname: nobody', replySender('~Camille', ''), null);
    check('a full number in the title: that number', replySender('+41 79 000 00 01', ''), {number: '41790000001'});
    check('a number without country code: nobody', replySender('079 000 00 01', ''), null);
    const book = [
        {name: 'Camille Martin', addresses: ['tel:+41790000001']},
        {name: 'Léo', addresses: ['tel:+41790000002', 'tel:+41220000002']},
        {name: 'Camille Martinez', addresses: ['tel:+41790000009']},
        {name: 'Ana', addresses: ['tel:0790000003']},
        {name: 'Sam', addresses: ['tel:+41790000004']}, {name: 'sam', addresses: ['tel:+41790000005']},
        {name: 'Zoé', addresses: ['tel:+41790000006', 'tel:+41 79 000 00 06', 'email:zoe@exemple.ch']},
    ];
    check('one contact, one number: sure', contactNumber('camille martin', book), '41790000001');
    check('two numbers: not sure', contactNumber('Léo', book), null);
    check('a local number: not sure', contactNumber('Ana', book), null);
    check('two contacts of that name: not sure', contactNumber('Sam', book), null);
    check('the same number written twice is one number', contactNumber('Zoé', book), '41790000006');
    check('nobody of that name', contactNumber('Inconnu', book), null);

    const c = linked();
    const note = (extra = {}) => ({command: 'bt_notification', uid: 5, app_id: 'net.whatsapp.WhatsApp', app_name: 'WhatsApp',
        title: 'Camille Martin', subtitle: '', body: 'Salut', silent: false, negative_action: true, ...extra});
    const replies = () => c.shown.filter(([kind]) => kind === 'reply').map(([, r]) => r);
    c.sent.length = 0;
    c.model.handleEvent(note());
    check('WhatsApp from one person: the name is looked up in the contacts', c.sent,
        [{command: 'bt_list_contacts', query: 'Camille Martin', limit: 5}]);
    check('...nothing offered before the answer', replies(), []);
    c.model.handleEvent(note({uid: 6, body: 'Tu viens ?'}));
    check('a second notification of the same person: one lookup', c.sent.length, 1);
    c.model.handleEvent({command: 'bt_contacts', query: 'Camille Martin', contacts: book});
    check('found for sure: Reply in app offered on both', replies(), [
        {uid: 5, appId: 'net.whatsapp.WhatsApp', number: '41790000001'},
        {uid: 6, appId: 'net.whatsapp.WhatsApp', number: '41790000001'}]);
    c.model.handleEvent(note({uid: 7, app_id: 'ph.telegra.Telegraph', body: 'Et ici ?'}));
    check('later ones: offered at once, no new lookup', [replies().length, c.sent.length], [3, 1]);
    c.shown.length = 0;
    c.model.handleEvent(note({uid: 8, title: 'Les voisins', subtitle: 'Camille Martin'}));
    c.model.handleEvent(note({uid: 9, title: 'Léo'}));
    c.model.handleEvent({command: 'bt_contacts', query: 'Léo', contacts: book});
    c.model.handleEvent(note({uid: 10, app_id: 'ch.exemple.Agenda', title: 'Camille Martin'}));
    check('a group, an unsure contact, another kind of app: nothing offered', replies(), []);
    c.model.handleEvent(note({uid: 11, title: '+41 79 000 00 07'}));
    check('a sender shown by number: offered without the contacts', replies(), [
        {uid: 11, appId: 'net.whatsapp.WhatsApp', number: '41790000007'}]);
    c.model.handleEvent(conn({pbap_open: false, pbap_error: 'forbidden'}));
    c.sent.length = 0;
    c.model.handleEvent(note({uid: 12, title: 'Zoé'}));
    check('contacts not readable: no lookup, nothing offered', c.sent, []);
}


// ============================================================================
// The classic link on demand (light mode), the call announced by its
// notification, and placing a call
// ============================================================================
const lean = (extra = {}) => conn({
    classic_connected: false, map_open: false, pbap_open: false,
    link_mode: 'lean', link_mode_effective: 'lean', link_mode_reason: '',
    classic_state: 'idle', classic_leases: [], classic_opened_by: '',
    messages: 'on_demand', contacts: 'on_demand', calls_available: 'on_demand',
    le_state: 'connected', le_cause: '', le_detail: '', last_sync: T0 / 1000 - 60, next_sync: T0 / 1000 + 3540,
    ...extra,
});
function leanLinked(extra = {}) {
    const c = setup();
    c.model.setEngineState('connected');
    c.model.handleEvent(status({link_mode: 'lean', calls_enabled: true, call_audio: 'phone'}));
    c.model.handleEvent(lean(extra));
    c.model.handleEvent({command: 'bt_threads', threads: [
        {thread: 'tel:+41790000001', name: 'Camille Martin', address: '+41790000001', preview: 'Salut', timestamp: T0 / 1000 - 500},
        {thread: 'tel:38200', name: '38200', address: '38200', preview: 'Code', timestamp: T0 / 1000 - 900},
    ]});
    c.sent.length = 0;
    return c;
}
const audioState = (extra = {}) => ({
    command: 'bt_audio', phone_audio: 'phone', call_audio: 'phone', call_audio_route: 'phone',
    call_audio_switchable: true, ...extra,
});

// --- X1: linked by the notification link alone
{
    const c = leanLinked();
    let v = c.model.view();
    check('light mode: linked on the notification link alone', [v.link, v.icon.state], ['linked', 'linked']);
    check('light mode: nothing said of how the rest connects, nothing is called a fault (#51)',
        [v.card.notices, v.card.setupAction, v.card.fixAction, v.card.paused],
        [[], false, false, null]);
    check('light mode: messages on demand can be written', [v.card.composeAction, v.card.callAction], [true, true]);
    const atRest = JSON.stringify(c.model.view().card);
    c.model.handleEvent(lean({classic_state: 'opening', classic_leases: ['send']}));
    c.model.handleEvent(lean({classic_state: 'open', classic_connected: true, map_open: true, messages: 'ready',
        classic_opened_by: 'engine'}));
    check('light mode: the menu does not change while the classic link opens and closes',
        JSON.stringify(c.model.view().card) === atRest, true);
    c.model.handleEvent(lean({classic_state: 'lingering', classic_connected: true, map_open: true, messages: 'ready'}));
    check('...nor while it lingers', c.model.view().card.notices, []);
    c.model.handleEvent(lean({classic_connected: true, le_connected: false, le_state: 'waiting', le_cause: 'reconnecting', ancs_ready: false}));
    check('light mode: the classic link alone is not "linked"', c.model.view().link, 'away');
    c.model.handleEvent(lean({messages: 'forbidden', map_error: 'forbidden'}));
    v = c.model.view();
    check('light mode: a switch off on the iPhone is still said, with the way to fix it',
        [v.card.notices, v.card.setupAction, v.card.composeAction],
        [['Some sharing switches are still off on the iPhone.'], true, false]);
    c.model.handleEvent(lean({messages: 'unavailable', contacts: 'unavailable', map_held: true, pbap_held: true,
        map_error: 'none', pbap_error: 'none'}));
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    check('light mode: services put on hold are unavailable, not broken', c.model.view().card.fixAction, false);
    c.model.handleEvent(lean({messages: 'unavailable', contacts: 'unavailable', map_error: 'other', pbap_error: 'other'}));
    check('light mode: both services failing is not a fault before the delay', c.model.view().card.fixAction, false);
    c.now += OBEX_STUCK_MS;
    c.model.expire();
    check('...and is one after it: Fix It', [c.model.view().card.fixAction, c.model.view().card.notices],
        [true, ['This computer can’t offer messages and contacts right now.']]);
    c.model.handleEvent(lean({link_mode_effective: 'full', link_mode_reason: 'phone_keeps_link',
        classic_connected: true, map_open: true, pbap_open: true, messages: 'ready', contacts: 'ready'}));
    v = c.model.view();
    c.model.handleEvent(lean({link_mode_effective: 'full', link_mode_reason: 'notifications_off',
        classic_connected: true, map_open: true, pbap_open: true, messages: 'ready', contacts: 'ready'}));
    v = c.model.view();
    check('started without the notifications granted yet: the switches of the iPhone, and the way to them',
        [v.link, v.icon.state, v.card.notices, v.card.fixAction, v.card.setupAction, v.card.paused],
        ['linked', 'attention', ['Some sharing switches are still off on the iPhone.'], false, true, null]);
    c.model.handleEvent(lean({messages: 'unknown', contacts: 'unknown'}));
    check('never tried on this pairing: nothing to write to yet, and no fault',
        [c.model.view().card.composeAction, c.model.view().card.fixAction], [false, false]);
    c.model.handleEvent(lean({link_mode_effective: 'full', link_mode_reason: 'phone_keeps_link',
        classic_connected: true, map_open: true, pbap_open: true, messages: 'ready', contacts: 'ready'}));
    v = c.model.view();
    check('mode in force differs from the one chosen: not the menu’s business (Phone Settings says it)',
        [v.card.notices, v.icon.state], [[], 'linked']);
    c.model.handleEvent(lean({link_mode_effective: 'full', link_mode_reason: 'phone_keeps_link', le_connected: false,
        classic_connected: true, map_open: true, messages: 'ready', le_state: 'waiting'}));
    check('full mode in force: linked by either link, as before', c.model.view().link, 'linked');
}

// --- X1: an older engine keeps the older rules
{
    const c = linked();
    c.model.handleEvent(conn({classic_connected: false, map_open: false, pbap_open: false}));
    const v = c.model.view();
    check('older engine, classic link down: linked by the other, no "on demand" line, nothing to write to',
        [v.link, v.card.notices, v.card.composeAction, v.card.paused], ['linked', [], false, null]);
    check('older engine: no lease was ever asked', c.sent.filter(s => s.command === 'bt_lease'), []);
    c.model.handleEvent(conn());
    c.model.handleEvent({command: 'bt_threads', threads: [{thread: 'tel:+41790000001', name: 'Camille Martin', address: '+41790000001', preview: 'x', timestamp: 1}]});
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    check('older engine: reading a conversation asks for no lease', c.sent.filter(s => s.command === 'bt_lease'), []);
    check('older engine: Phone Settings is told there is one mode only', c.model._linkModeState(), '');
}

// --- X1: sending with a visible wait
{
    const c = leanLinked();
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    check('light mode: opening a conversation takes the reading lease',
        c.sent, [{command: 'bt_lease', name: 'reading', held: true}]);
    c.sent.length = 0;
    check('reply accepted', c.model.reply('  J’arrive  '), true);
    check('reply sent, and the lease handed back with the card', c.sent.map(s => s.command), ['bt_send_message', 'bt_lease']);
    check('...released', c.sent[1], {command: 'bt_lease', name: 'reading', held: false});
    const id = c.sent[0].operation_id;
    let v = c.model.view();
    check('link closed: the pill says it is connecting first', [v.mode, v.pill.strong], ['sending', 'Connecting to send…']);
    c.model.handleEvent({command: 'bt_send_progress', operation_id: id, thread: 'tel:+41790000001', stage: 'connecting'});
    check('bt_send_progress connecting: still connecting', c.model.view().pill.strong, 'Connecting to send…');
    c.now += SEND_TIMEOUT_MS - 1000;
    c.model.handleEvent({command: 'bt_send_progress', operation_id: 'someone-else', stage: 'sending'});
    check('progress of another operation: ignored', c.model.view().pill.strong, 'Connecting to send…');
    c.model.handleEvent({command: 'bt_send_progress', operation_id: id, thread: 'tel:+41790000001', stage: 'sending'});
    check('bt_send_progress sending: Sending…', c.model.view().pill.strong, 'Sending…');
    c.now += 5000;
    c.model.expire();
    check('progress restarts the wait: not given up yet', c.model.view().mode, 'sending');
    // The conversation reopened meanwhile says the same, and takes no second reply.
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    v = c.model.view();
    check('card reopened while sending: says so, and is busy', [v.card.subtitle, v.card.isError, v.card.busy], ['Sending…', false, true]);
    check('...a second reply is not taken', c.model.reply('encore'), false);
    c.model.cardClosed();
    c.model.handleEvent({command: 'bt_send_result', operation_id: id, thread: 'tel:+41790000001', success: true});
    check('sent', [c.model.view().mode, c.model.view().pill.strong], ['sent', 'Sent']);

    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    c.model.reply('Deuxième');
    const id2 = c.sent.filter(s => s.command === 'bt_send_message').at(-1).operation_id;
    c.model.handleEvent({command: 'bt_send_result', operation_id: id2, thread: 'tel:+41790000001', success: false,
        message: 'Couldn’t reach the iPhone to send.'});
    v = c.model.view();
    check('not sent: said in the bar', [v.mode, v.pill.strong], ['notice', 'Not sent']);
    c.model.cardOpened();
    v = c.model.view();
    check('not sent: the card comes back with the text and the engine’s own words',
        [v.card.kind, v.card.draft, v.card.subtitle, v.card.isError, v.card.busy],
        ['reply', 'Deuxième', 'Couldn’t reach the iPhone to send.', true, false]);

    const d = leanLinked({classic_connected: true, map_open: true, messages: 'ready', classic_state: 'open'});
    d.model.cardOpened();
    d.model.openThread('tel:+41790000001');
    d.model.reply('Vite');
    check('link already open: Sending… at once', d.model.view().pill.strong, 'Sending…');
    const e = linked();
    e.model.handleEvent({command: 'bt_threads', threads: [{thread: 'tel:+41790000001', name: 'Camille Martin', address: '+41790000001', preview: 'x', timestamp: 1}]});
    e.model.cardOpened();
    e.model.openThread('tel:+41790000001');
    e.model.reply('Vite');
    check('older engine: Sending… as before', e.model.view().pill.strong, 'Sending…');
}

// --- X1: leases
{
    const c = leanLinked();
    c.model.cardOpened();
    check('the menu alone takes no lease', c.sent, []);
    c.model.startCompose();
    check('writing a new message takes the reading lease', c.sent, [{command: 'bt_lease', name: 'reading', held: true}]);
    check('the lease asks to be renewed', c.model.nextDeadline(), T0 + LEASE_RENEW_MS);
    c.now += LEASE_RENEW_MS;
    c.model.expire();
    check('renewed after four minutes', c.sent.length, 2);
    c.model.cardClosed();
    check('handed back when the card closes', c.sent.at(-1), {command: 'bt_lease', name: 'reading', held: false});
    check('nothing left to wait for', c.model.nextDeadline(), null);

    const t = setup();
    t.model.setEngineState('connected');
    t.model.handleEvent(status({link_mode: 'lean'}));
    t.model.handleEvent(lean({messages: 'forbidden', contacts: 'forbidden', ancs_ready: false,
        classic_connected: true, classic_state: 'open'}));
    t.sent.length = 0;
    t.model.startTour();
    check('light mode: the tour takes the setup lease', [t.model.view().tour.step, t.sent[0]],
        ['switches', {command: 'bt_lease', name: 'setup', held: true}]);
    t.model.handleEvent(lean({messages: 'on_demand', contacts: 'on_demand', ancs_ready: false}));
    check('a switch found on counts although the link closed again',
        [t.model.view().tour.switches.messages, t.model.view().tour.switches.contacts], [true, true]);
    t.sent.length = 0;
    t.model.cancelTour?.();
    t.model.closeTour();
    check('tour closed: the lease is handed back', t.sent.filter(s => s.command === 'bt_lease').at(-1),
        {command: 'bt_lease', name: 'setup', held: false});
}

// --- X1: notifications paused, and the battery kept
{
    const c = linked();
    c.model.handleEvent(conn({le_state: 'connected', le_cause: ''}));
    c.model.handleEvent(conn({le_connected: false, ancs_ready: false, battery: null,
        le_state: 'waiting', le_cause: 'reconnecting'}));
    let v = c.model.view();
    check('notification link just lost: linked, nothing said before the engine calls it a pause',
        [v.link, v.icon.state, v.card.notices, v.card.paused], ['linked', 'linked', [], null]);
    check('the battery keeps its place, marked as the last one known', [v.card.battery, v.card.batteryStale], [96, true]);
    c.now += 25 * 60000;
    c.model.handleEvent(conn({le_connected: false, ancs_ready: false, battery: null,
        le_state: 'paused', le_cause: 'phone_silent', le_detail: 'The iPhone stopped answering.'}));
    v = c.model.view();
    check('paused: the indicator stops looking fine', [v.link, v.icon.state, v.a11y],
        ['linked', 'paused', 'iPhone linked, notifications paused']);
    check('paused: the menu says why and what to do', v.card.paused, {
        cause: 'phone_silent',
        text: 'The iPhone is within reach but has stopped sending notifications.',
        remedy: 'Turn Bluetooth off and on again on your iPhone.', onPhone: true});
    check('paused: no other line, no word beside the name', [v.card.notices, 'state' in v.card], [[], false]);
    check('paused: the battery is 25 minutes old', [v.card.battery, v.card.batteryStale, v.card.batteryMinutes], [96, true, 25]);
    c.model.handleEvent(message({timestamp: c.now / 1000}));
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('paused, unread: the count stays, the reader hears both',
        [c.model.view().unread, c.model.view().a11y], [1, 'iPhone linked, unread messages, notifications paused']);
    for (const [cause, start] of [['after_resume', 'This computer woke up'], ['unresolved_address', 'An iPhone answered'],
        ['advert_off_air', 'This computer has stopped announcing']]) {
        c.model.handleEvent(conn({le_connected: false, le_state: 'paused', le_cause: cause}));
        c.model.cardOpened();
        c.model.cardClosed();
        check(`paused, ${cause}: its own sentence and remedy`,
            [c.model._notificationsPaused().text.startsWith(start), c.model._notificationsPaused().remedy.length > 10], [true, true]);
    }
    c.model.handleEvent(conn({le_connected: false, le_state: 'paused', le_cause: 'something_new', le_detail: 'The radio is asleep.'}));
    check('paused, a cause this version does not know: the engine’s sentence', c.model._notificationsPaused().text, 'The radio is asleep.');
    c.model.handleEvent(conn({le_state: 'connected', le_cause: '', battery: 91}));
    v = c.model.view();
    check('back: nothing said any more, live battery', [v.unread, v.a11y, v.card.paused, v.card.battery, v.card.batteryStale],
        [1, 'iPhone linked, unread messages', null, 91, false]);
    c.model.handleEvent(conn({le_state: 'off', le_cause: 'disabled', le_connected: false, ancs_ready: false}));
    check('notifications turned off on this computer: nothing to announce', c.model.view().card.paused, null);

    // Out of range in light mode: the word says it, no second sentence.
    const a = leanLinked();
    a.model.handleEvent(lean({le_connected: false, ancs_ready: false, battery: null, le_state: 'paused', le_cause: 'phone_away'}));
    let w = a.model.view();
    check('light mode, iPhone gone: out of range, nothing more, battery kept as last known',
        [w.link, w.card.notices, w.card.paused, w.card.battery, w.card.batteryStale], ['away', ['Out of range'], null, 96, true]);
    a.model.handleEvent(lean({le_connected: false, ancs_ready: false, battery: null, le_state: 'paused', le_cause: 'after_resume'}));
    w = a.model.view();
    check('light mode, after sleep: away, and the pause is explained', [w.link, w.card.paused?.cause], ['away', 'after_resume']);
    a.now += 13 * 3600 * 1000;
    check('a battery level half a day old is not shown any more', a.model.view().card.battery, null);

    const o = linked();
    o.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    check('older engine: the battery goes with the link, as before',
        [o.model.view().card.battery, o.model.view().card.batteryStale], [null, false]);
}

// --- X1: the mode, as Phone Settings sees it
{
    const c = setup();
    c.settings = [];
    c.model.onSetting = (name, value) => c.settings.push([name, value]);
    c.model.setEngineState('connected');
    c.model.handleEvent(status({link_mode: 'lean'}));
    c.model.handleEvent(lean());
    check('the engine’s mode reaches Phone Settings, with the mode in force',
        c.settings.filter(([n]) => n.startsWith('link-mode')), [['link-mode-state', 'lean:'], ['link-mode', 'lean']]);
    c.settings.length = 0;
    c.sent.length = 0;
    c.model.wantSetting('link-mode', 'full');
    check('"Always connected" chosen: bt_set_link_mode', c.sent, [{command: 'bt_set_link_mode', mode: 'full'}]);
    c.model.handleEvent(status({link_mode: 'lean'}));
    check('the older value still on its way is not taken for a change', c.settings, []);
    c.model.handleEvent(status({link_mode: 'full'}));
    c.model.handleEvent(lean({link_mode: 'full', link_mode_effective: 'full'}));
    check('confirmed', c.settings, [['link-mode', 'full'], ['link-mode-state', 'full:']]);
    c.sent.length = 0;
    c.model.wantSetting('link-mode', 'full');
    c.model.wantSetting('link-mode', 'neither');
    check('nothing to change, or not a mode: nothing sent', c.sent, []);
    c.model.handleEvent(lean({link_mode: 'lean', link_mode_effective: 'full', link_mode_reason: 'notifications_off'}));
    check('mode in force differs: Phone Settings gets the reason', c.settings.at(-1), ['link-mode-state', 'full:notifications_off']);
    const o = linked();
    o.sent.length = 0;
    o.model.wantSetting('link-mode', 'full');
    check('older engine: the choice is not sent', o.sent, []);
}

// --- X1: a message shown from its notification, then completed
{
    const sms = (extra = {}) => ({
        command: 'bt_notification', uid: 61, app_id: 'com.apple.MobileSMS', app_name: 'Messages',
        title: 'Camille Martin', subtitle: '', body: 'On se retrouve à 19h ?', category: 4,
        timestamp: T0 / 1000, silent: false, ...extra,
    });
    const c = leanLinked();
    c.model.handleEvent(sms());
    let v = c.model.view();
    check('light mode: the notification of Messages shows at once in the pill',
        [v.mode, v.pill.strong, v.pill.text, c.shown], ['message', 'Camille', 'On se retrouve à 19h ?', []]);
    c.model.handleEvent(message({handle: 'H9'}));
    v = c.model.view();
    check('the message itself takes its place: one pill, nothing counted twice',
        [v.mode, v.pill.text, v.unread, c.model._provisional.size], ['message', 'On se retrouve à 19h ?', 0, 0]);
    c.model.cardOpened();
    v = c.model.view();
    check('opened: the real conversation, one bubble, marked read once',
        [v.card.thread, v.card.messages.length, v.card.repliable, c.sent.filter(s => s.command === 'bt_mark_read')],
        ['tel:+41790000001', 1, true, [{command: 'bt_mark_read', handles: ['H9'], read: true}]]);
    c.model.cardClosed();

    // Opened before the message came, sender known by name: answerable at once.
    c.sent.length = 0;
    c.model.handleEvent(sms({uid: 62, body: 'Tu es là ?'}));
    c.model.cardOpened();
    v = c.model.view();
    check('known sender: the card opens on the conversation, nothing marked yet',
        [v.card.thread, v.card.messages.map(m => m.body), v.card.repliable, v.card.arriving, c.sent.filter(s => s.command === 'bt_mark_read')],
        ['tel:+41790000001', ['Tu es là ?'], true, false, []]);
    c.model.handleEvent(message({handle: 'H10', body: 'Tu es là ?'}));
    check('the message arrives under the open card: marked read, still one bubble',
        [c.model.view().card.messages.length, c.sent.filter(s => s.command === 'bt_mark_read')],
        [1, [{command: 'bt_mark_read', handles: ['H10'], read: true}]]);
    c.model.cardClosed();
    check('nothing left unread', c.model.view().unread, 0);

    // An unknown sender: the bubble first, the reply field when the conversation is known.
    c.sent.length = 0;
    c.model.handleEvent(sms({uid: 63, title: 'Zoé Favre', body: 'Salut, c’est Zoé'}));
    c.model.cardOpened();
    v = c.model.view();
    check('unknown sender: shown, not answerable yet, and said so',
        [v.card.name, v.card.messages.map(m => m.body), v.card.repliable, v.card.arriving], ['Zoé Favre', ['Salut, c’est Zoé'], false, true]);
    c.model.handleEvent(message({handle: 'H11', thread: 'tel:+41790000009', address: '+41790000009', name: 'Zoé Favre', body: 'Salut, c’est Zoé'}));
    v = c.model.view();
    check('...then completed: same card, now on its conversation',
        [v.card.thread, v.card.repliable, v.card.arriving, v.card.messages.length], ['tel:+41790000009', true, false, 1]);
    c.model.cardClosed();

    // Read here from the notification, card closed, then the message comes.
    c.sent.length = 0;
    c.model.handleEvent(sms({uid: 64, body: 'Dernière'}));
    c.model.cardOpened();
    c.model.cardClosed();
    c.model.handleEvent(message({handle: 'H12', body: 'Dernière'}));
    check('already read here: no second pill, marked read on the iPhone',
        [c.model.view().mode, c.model.view().unread, c.sent.filter(s => s.command === 'bt_mark_read')],
        ['idle', 0, [{command: 'bt_mark_read', handles: ['H12'], read: true}]]);

    // The message never follows.
    c.model.handleEvent(sms({uid: 65, title: 'Inconnu Total', body: 'Sans suite'}));
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('unopened: counted', c.model.view().unread, 1);
    check('...and waited for', c.model.nextDeadline(), T0 + PROVISIONAL_MS);
    c.now = T0 + PROVISIONAL_MS;
    c.model.expire();
    check('no message after a minute, no conversation: it goes to the desktop, the count drops',
        [c.model.view().unread, c.shown.at(-1)?.[0], c.shown.at(-1)?.[1].title], [0, 'show', 'Inconnu Total']);
    c.shown.length = 0;
    c.model.handleEvent(sms({uid: 66, body: 'Reste dans le fil'}));
    c.now += PROVISIONAL_MS;
    c.model.expire();
    check('no message after a minute, conversation known: stays there, unread',
        [c.model.view().unread, c.shown, c.model.view().card.threads.find(t => t.unread)?.thread], [1, [], 'tel:+41790000001']);

    // Read or cleared on the iPhone first.
    const r = leanLinked();
    r.model.handleEvent(sms({uid: 70}));
    r.model.handleEvent({command: 'bt_notification_removed', uid: 70});
    check('cleared on the iPhone before anything: the pill goes', [r.model.view().mode, r.model.view().unread], ['idle', 0]);
    r.model.handleEvent(sms({uid: 71}));
    r.model.handleEvent(message({handle: 'H20', read: true}));
    check('read on the iPhone when it arrives: the pill goes', [r.model.view().mode, r.model.view().unread], ['idle', 0]);
    r.model.handleEvent(sms({uid: 72, silent: true}));
    r.model.handleEvent({command: 'bt_notifications', notifications: [sms({uid: 73})]});
    check('already on the iPhone at connection: never a pill, never on the desktop', [r.model.view().mode, r.shown.filter(([k]) => k === 'show')], ['idle', []]);
    r.model.handleEvent(sms({uid: 74, title: 'Les voisins', subtitle: 'Camille Martin', body: 'Apéro ?'}));
    r.model.handleEvent(message({handle: 'H21', thread: 'group:1', name: 'Les voisins', address: '', body: 'Apéro ?'}));
    check('a group: matched by its text', [r.model._provisional.size, r.model.view().mode], [0, 'message']);

    const f = leanLinked({messages: 'forbidden'});
    f.model.handleEvent(sms({uid: 80}));
    check('messages not shared: the notification is shown as one of another app', [f.model.view().mode, f.shown[0]?.[0]], ['note', 'show']);
    const o = linked();
    o.model.handleEvent(sms({uid: 81}));
    check('older engine: the pill waits for the message, as before', [o.model.view().mode, o.shown], ['idle', []]);
    const h = leanLinked();
    h.model.setQuiet({dnd: true});
    h.model.handleEvent(sms({uid: 82}));
    check('silence: counted, nothing grows', [h.model.view().mode, h.model.view().unread], ['idle', 1]);
}

// --- X2: the call announced by its notification
{
    const ring = (extra = {}) => ({
        command: 'bt_incoming_call', uid: 7, state: 'ringing', caller: 'Camille Martin',
        can_answer_on_phone: true, can_decline: true, answer_here: 'connecting', since: T0 / 1000, ...extra,
    });
    const c = leanLinked();
    c.model.handleEvent(audioState());
    c.sent.length = 0;
    c.model.handleEvent(ring());
    let v = c.model.view();
    check('bt_incoming_call: the card is there before any bt_calls',
        [v.mode, v.pill.strong, v.card.kind, v.card.name], ['ring', 'Camille is calling', 'ring', 'Camille Martin']);
    check('...Decline and Answer on iPhone at once, Answer here connecting',
        [v.card.answerChoice, v.card.answerHere, v.card.answerOnPhone, v.card.canDecline], [true, 'connecting', true, true]);
    c.model.answer('computer');
    check('Answer here while connecting: nothing sent', c.sent, []);
    check('the wait has an end', c.model.nextDeadline(), T0 + ANSWER_HERE_MS);
    c.model.handleEvent(ring({answer_here: 'ready', path: '/call1'}));
    c.model.handleEvent(call('incoming'));
    v = c.model.view();
    check('hands-free ready: Answer here works, same card, same name',
        [v.card.answerHere, v.card.name, v.mode], ['ready', 'Camille Martin', 'ring']);
    c.sent.length = 0;
    c.model.answer('computer');
    check('Answer here: by uid and path, sound here', c.sent.filter(s => s.command === 'bt_call_action'),
        [{command: 'bt_call_action', action: 'answer', uid: 7, audio: 'here', path: '/call1'}]);
    c.model.handleEvent(call('active'));
    c.model.handleEvent(ring({state: 'ended'}));
    check('answered: the call card takes over', [c.model.view().mode, c.model.view().card.kind], ['call', 'call']);

    // Declined before the hands-free link.
    const d = leanLinked();
    d.model.handleEvent(ring());
    d.sent.length = 0;
    d.model.decline();
    check('Decline without hands-free: by uid', d.sent, [{command: 'bt_call_action', action: 'decline', uid: 7}]);
    d.model.cardOpened();
    d.model.handleEvent(ring({state: 'ended'}));
    check('ended: the card closes', [d.model.view().mode, d.model.view().cardOpen], ['idle', false]);

    // Answered on the iPhone, then the sound brought here.
    const p = leanLinked();
    p.model.handleEvent(audioState());
    p.model.handleEvent(ring());
    p.sent.length = 0;
    p.model.answer('phone');
    check('Answer on iPhone without hands-free: by uid, sound on the phone',
        p.sent, [{command: 'bt_call_action', action: 'answer', uid: 7, audio: 'phone'}]);
    check('...and said', p.model.view().card.subtitle, 'Answering on the iPhone…');
    p.model.handleEvent(lean({calls_available: 'on_demand', calls: {audio: 'idle', audio_route: 'phone'}}));
    p.model.handleEvent(call('active'));
    p.model.handleEvent(ring({state: 'ended'}));
    let w = p.model.view();
    check('picked up on the iPhone: call card, This computer not ready yet', [w.mode, w.card.audio, w.card.computerReady], ['call', 'phone', false]);
    p.sent.length = 0;
    p.model.setCallAudio('computer');
    check('...so the move is not asked', p.sent, []);
    p.model.handleEvent(lean({calls_available: 'ready', classic_connected: true, calls: {audio: 'idle', audio_route: 'phone'}}));
    check('hands-free ready during the call: This computer can be chosen', p.model.view().card.computerReady, true);
    p.model.setCallAudio('computer');
    check('...and is', p.sent, [{command: 'bt_set_call_audio', route: 'here', persist: false}]);
    p.model.handleEvent(lean({calls_available: 'ready', classic_connected: true, calls: {audio: 'active', audio_route: 'here'}}));
    w = p.model.view();
    check('the card shows where the sound really is', [w.card.audio, w.card.moving], ['computer', false]);

    // The hands-free link never comes.
    const u = leanLinked();
    u.model.handleEvent(ring());
    u.model.handleEvent(ring({answer_here: 'unavailable'}));
    check('unavailable, said by the engine', u.model.view().card.answerHere, 'unavailable');
    u.model.handleEvent(ring({answer_here: 'ready', path: '/call1'}));
    check('the hands-free link comes after all: Answer here works again', u.model.view().card.answerHere, 'ready');
    u.model.handleEvent(ring({answer_here: 'unavailable'}));
    u.sent.length = 0;
    u.model.answer('computer');
    u.model.answer('phone');
    check('...Answer on iPhone still works', u.sent.map(s => s.audio), ['phone']);
    const s = leanLinked();
    s.model.handleEvent(ring());
    s.now += ANSWER_HERE_MS;
    s.model.expire();
    check('an engine that stays silent: the card stops waiting by itself', s.model.view().card.answerHere, 'unavailable');

    // The caller hangs up; the missed call goes to the desktop as before.
    const m = leanLinked();
    m.model.handleEvent(ring({caller: '+41225550199'}));
    check('a caller the iPhone does not know: its number, written legibly',
        [m.model.view().card.name, m.model.view().pill.strong], ['+41 22 555 01 99', '+41 22 555 01 99']);
    m.model.handleEvent(ring({state: 'ended'}));
    m.model.handleEvent({command: 'bt_notification', uid: 90, app_id: 'com.apple.mobilephone', app_name: 'Phone',
        title: '+41225550199', body: 'Missed Call', category: 2, timestamp: T0 / 1000, silent: false});
    check('ringing over: idle, and the missed call is a desktop notification',
        [m.model.view().mode, m.shown.filter(([k]) => k === 'show').map(([, n]) => n.body)], ['idle', ['Missed Call']]);
    m.model.handleEvent({command: 'bt_calls', calls: [], incoming: ring({uid: 8})});
    check('subscribed during the ringing: bt_list_calls says which call rings', m.model.view().mode, 'ring');
    m.model.handleEvent({command: 'bt_calls', calls: [], incoming: null});
    check('...and when none does', m.model.view().mode, 'idle');
    m.model.handleEvent(ring({uid: 9, can_answer_on_phone: false, can_decline: false}));
    m.sent.length = 0;
    m.model.answer('phone');
    m.model.decline();
    check('a notification without actions: nothing can be asked before hands-free',
        [m.model.view().card.answerOnPhone, m.model.view().card.canDecline, m.sent], [false, false, []]);
    m.model.setQuiet({dnd: true});
    check('silence: the announced call does not grow either', m.model.view().mode, 'idle');

    // An engine without bt_incoming_call: the card of today.
    const o = linked();
    o.model.handleEvent(audioState());
    o.model.handleEvent(call('incoming'));
    o.sent.length = 0;
    w = o.model.view();
    check('older engine: the two answers, no waiting state', [w.card.answerChoice, w.card.answerHere], [true, null]);
    o.model.answer('computer');
    o.model.handleEvent(call('incoming'));
    o.model.decline();
    check('older engine: answer and hang up by path, as before', o.sent.filter(s => s.command === 'bt_call_action'),
        [{command: 'bt_call_action', action: 'answer', path: '/call1', audio: 'here'},
            {command: 'bt_call_action', action: 'hangup', path: '/call1'}]);
}

// --- X3: placing a call
{
    const book = [
        {name: 'Camille Martin', addresses: ['tel:+41790000001', 'email:camille@exemple.ch']},
        {name: 'Camille Sans Numéro', addresses: ['email:autre@exemple.ch']},
    ];
    const c = leanLinked();
    c.model.handleEvent(audioState({call_audio: 'here'}));
    c.model.handleEvent({command: 'bt_call_history', calls: [
        {number: '+41790000002', name: 'Léo', type: 'missed', timestamp: T0 / 1000 - 100},
        {number: '', name: '', type: 'received', timestamp: T0 / 1000 - 200}]});
    c.sent.length = 0;
    let v = c.model.view();
    check('menu: Call… on offer, recent calls with a number can be called',
        [v.card.callAction, v.card.history.map(h => h.number)], [true, ['+41790000002', '']]);
    check('menu: each conversation with a number has its call button',
        v.card.threads.map(t => t.callable), ['+41790000001', '38200']);
    c.model.cardOpened();
    c.model.startDial();
    v = c.model.view();
    check('Call…: a card to look someone up, sound preset from the setting', [v.card.kind, v.card.query, v.card.results, v.card.audio],
        ['dial', '', [], 'computer']);
    check('...no lease: the engine takes its own for a call', c.sent, []);
    c.model.dialQuery('cam');
    check('typing looks the contacts up', c.sent, [{command: 'bt_list_contacts', query: 'cam', limit: 6}]);
    c.model.handleEvent({command: 'bt_contacts', query: 'cam', contacts: book});
    v = c.model.view();
    check('only what has a number is offered', v.card.results,
        [{number: '+41790000001', name: 'Camille Martin', initials: 'CM', detail: '+41 79 000 00 01'}]);
    c.model.dialQuery('+41 (22) 555-01 99');
    check('a number typed by hand is offered as it is, first',
        c.model.view().card.results[0], {number: '+41225550199', name: '+41 22 555 01 99', initials: '', detail: ''});
    c.model.dialQuery('144');
    check('a short number too', c.model.view().card.results.map(r => r.number), ['144']);
    c.model.dialQuery('cam');
    c.model.handleEvent({command: 'bt_contacts', query: 'cam', contacts: book});
    c.model.setDialAudio('phone');
    check('sound chosen: iPhone', c.model.view().card.audio, 'phone');
    c.sent.length = 0;
    c.model.dialPick(0);
    const id = c.sent[0].operation_id;
    check('picked: bt_call_dial with the number, the sound and an operation', c.sent,
        [{command: 'bt_call_dial', number: '+41790000001', operation_id: id, audio: 'phone'}]);
    v = c.model.view();
    check('link closed: connecting first, and it can be given up',
        [v.mode, v.pill.strong, v.card.kind, v.card.name, v.card.subtitle, v.card.cancellable],
        ['dialing', 'Connecting…', 'dialing', 'Camille Martin', 'Connecting to iPhone…', true]);
    c.model.handleEvent({command: 'bt_call_progress', operation_id: id, number: '+41790000001', stage: 'hands_free'});
    check('hands_free: getting the call ready', c.model.view().card.subtitle, 'Getting the call ready…');
    c.model.handleEvent({command: 'bt_call_progress', operation_id: 'other', stage: 'dialing'});
    check('progress of another operation: ignored', c.model.view().card.subtitle, 'Getting the call ready…');
    c.model.handleEvent({command: 'bt_call_progress', operation_id: id, number: '+41790000001', stage: 'dialing'});
    v = c.model.view();
    check('dialing: Calling…, no way back but hanging up', [v.card.subtitle, v.card.cancellable], ['Calling…', false]);
    c.sent.length = 0;
    c.model.cancelDial();
    check('...so Cancel does nothing', [c.sent, c.model.view().mode], [[], 'dialing']);
    for (let i = 0; i < 2; i++)
        c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: true, operation_id: id});
    check('bt_call_result, twice: still calling, no error anywhere', [c.model.view().mode, c.model._callError], ['dialing', '']);
    {
        // The engine of the light mode sends it once.
        const once = leanLinked();
        once.model.cardOpened();
        once.model.startDial({number: '+41790000001', name: 'Camille Martin'});
        once.sent.length = 0;
        once.model.dialPick(0);
        once.model.handleEvent({command: 'bt_call_result', action: 'dial', success: true, operation_id: once.sent[0].operation_id});
        check('bt_call_result once: calling, then the call card', once.model.view().card.subtitle, 'Calling…');
        once.model.handleEvent(call('dialing'));
        check('...', once.model.view().mode, 'call');
    }
    c.model.handleEvent(call('dialing', {name: 'Camille Martin'}));
    v = c.model.view();
    check('the call shows up: the usual pill and card take over', [v.mode, v.card.kind, v.pill.strong], ['call', 'call', 'Camille · Calling…']);
    c.model.handleEvent(call('active', {name: 'Camille Martin'}));
    check('established', c.model.view().pill.strong, 'Camille · 00:00');
    c.model.handleEvent({command: 'bt_calls', calls: []});

    // Given up before it is dialled.
    c.model.cardOpened();
    c.model.startDial();
    c.model.dialQuery('0790000002');
    c.sent.length = 0;
    c.model.dial('079 000 00 02', 'computer');
    const id2 = c.sent[0].operation_id;
    check('dial() by number: normalised, sound here', c.sent[0], {command: 'bt_call_dial', number: '0790000002', operation_id: id2, audio: 'here'});
    c.model.cancelDial();
    check('Cancel: bt_call_cancel, back to the menu', [c.sent[1], c.model.view().mode, c.model.view().card.kind, c.model.view().cardOpen],
        [{command: 'bt_call_cancel', operation_id: id2}, 'idle', 'menu', true]);
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, operation_id: id2, reason: 'cancelled'});
    check('the engine confirms: nothing said', [c.model.view().mode, c.model._callError], ['idle', '']);

    // Failures.
    for (const [reason, text] of [['no_link', 'Couldn’t reach the iPhone.'], ['no_hands_free', 'Couldn’t reach the iPhone.'],
        ['busy', 'Another call is already being placed.'], ['calls_off', 'Calls from this computer are turned off in Phone Settings.'],
        ['refused', 'The iPhone did not place the call.']]) {
        c.model.startDial({number: '+41790000002', name: 'Léo'});
        c.sent.length = 0;
        c.model.dialPick(0);
        const op = c.sent[0].operation_id;
        c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, operation_id: op, reason, message: 'raw'});
        c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, operation_id: op, reason, message: 'raw'});
        v = c.model.view();
        check(`failed (${reason}): said on the card, with a way to try again`,
            [v.mode, v.card.kind, v.card.failed, v.card.subtitle, v.card.isError, c.model._callError],
            ['idle', 'dialing', true, text, true, '']);
    }
    c.sent.length = 0;
    c.model.retryDial();
    check('Try Again: the same call again', [c.sent[0].number, c.sent[0].audio, c.model.view().mode], ['+41790000002', 'here', 'dialing']);
    c.model.cardClosed();
    check('menu closed while calling: the pill keeps saying it', c.model.view().mode, 'dialing');
    c.now += DIAL_TIMEOUT_MS;
    c.model.expire();
    v = c.model.view();
    check('no word from the engine: given up, said in the bar', [v.mode, v.pill.strong], ['notice', 'Call not placed']);
    c.model.cardOpened();
    check('...and on the card when it is opened', [c.model.view().card.kind, c.model.view().card.subtitle], ['dialing', 'The iPhone did not answer.']);
    c.model.cardClosed();
    check('closed: forgotten', c.model.view().card.kind, 'menu');

    // From a conversation and from the recent calls.
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    check('a conversation with a number can be called', c.model.view().card.callable, '+41790000001');
    c.model.openThread('tel:38200');
    check('...a short code as well, it is a number', c.model.view().card.callable, '38200');
    c.sent.length = 0;
    c.model.startDial({number: '+41790000001', name: 'Camille Martin'});
    v = c.model.view();
    check('from a conversation: the card opens on that person, nothing dialled yet',
        [v.card.kind, v.card.query, v.card.results.length, c.sent.filter(s => s.command === 'bt_call_dial')], ['dial', 'Camille Martin', 1, []]);
    c.model.handleEvent({command: 'bt_contacts', query: 'Camille Martin', contacts: book.concat(book)});
    check('...and stays on that person', c.model.view().card.results.length, 1);
    c.model.startDial({number: 'BanqueDemo', name: ''});
    check('a sender known by letters cannot be called: empty card', c.model.view().card.results, []);

    // What decides whether Call… is there.
    const off = leanLinked({calls_available: 'off'});
    check('calls turned off: no Call…', [off.model.view().card.callAction, off.model.view().card.history.every(h => !h.number)], [false, true]);
    off.model.cardOpened();
    off.model.startDial();
    check('...and startDial does nothing', off.model.view().card.kind, 'menu');
    const only = leanLinked({calls_available: 'control_only'});
    only.model.cardOpened();
    check('no hands-free link right now: Call… stays where it is', only.model.view().card.callAction, true);
    only.model.startDial({number: '+41790000001', name: ''});
    only.sent.length = 0;
    only.model.dialPick(0);
    only.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, reason: 'no_hands_free',
        operation_id: only.sent[0].operation_id});
    check('...and the call that cannot be placed says so, once, with a way to try again',
        [only.model.view().card.failed, only.model.view().card.subtitle, only.model._callError],
        [true, 'Couldn’t reach the iPhone.', '']);
    const full = leanLinked({calls_available: 'ready', link_mode: 'full', link_mode_effective: 'full', classic_connected: true});
    full.model.cardOpened();
    full.model.startDial({number: '+41790000001', name: ''});
    full.model.dialPick(0);
    check('hands-free already there: Calling… at once', full.model.view().card.subtitle, 'Calling…');
    check('no call audio choice with an engine that cannot place it: no audio sent',
        [full.model.view().card.audio, full.sent.find(s => s.command === 'bt_call_dial').audio], [undefined, undefined]);

    // An older engine: bt_call_dial alone, no progress, no cancel.
    const o = linked();
    o.model.handleEvent(conn({calls: {available: true, audio: 'idle'}}));
    o.model.cardOpened();
    check('older engine with call control: Call… on offer', o.model.view().card.callAction, true);
    o.model.startDial({number: '+41790000001', name: 'Camille Martin'});
    o.sent.length = 0;
    o.model.dialPick(0);
    v = o.model.view();
    check('older engine: Calling… until the result, nothing to cancel',
        [o.sent[0].command, v.card.subtitle, v.card.cancellable], ['bt_call_dial', 'Calling…', false]);
    o.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, message: 'No hands-free link.'});
    check('older engine: its own words on failure', o.model.view().card.subtitle, 'No hands-free link.');
    const none = linked();
    none.model.handleEvent(conn({calls: null}));
    check('older engine, call control off: no Call…', none.model.view().card.callAction, false);
}

// --- the icon of the top bar: one drawing per state, no dot, no count
{
    const note = (extra = {}) => ({
        command: 'bt_notification', uid: 70, app_id: 'com.apple.mobilephone', app_name: 'Phone',
        title: 'Camille Martin', subtitle: '', body: 'Missed Call', category: 2, timestamp: T0 / 1000,
        silent: false, ...extra,
    });
    let c = setup();
    check('nothing known yet: the bare glyph, dimmed', c.model.view().icon, {state: 'bare', moving: false, dim: true});
    c.model.setEngineState('absent');
    check('engine absent: bare glyph beside the words', c.model.view().icon, {state: 'bare', moving: false});
    c = linked();
    let v = c.model.view();
    check('linked: its own drawing, still, nothing beside it', [v.icon, v.pill], [{state: 'linked', moving: false}, {tone: 'quiet'}]);
    check('linked: nothing to wait for', c.model.nextDeadline(), null);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, map_open: false, pbap_open: false, ancs_ready: false}));
    check('out of range: the dimmed drawing', c.model.view().icon, {state: 'away', moving: false});
    c.model.handleEvent(status({device_address: ''}));
    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: false});
    v = c.model.view();
    check('Link iPhone: bare glyph, the words stay beside it', [v.icon, v.pill], [{state: 'bare', moving: false}, {tone: 'accent', strong: 'Link iPhone'}]);

    // A message: the pill for 7 s, then the bubble, moving for a minute.
    c = linked();
    c.model.handleEvent(message());
    v = c.model.view();
    check('message pill: the bubble is at its head, moving', [v.mode, v.icon], ['message', {state: 'unread', moving: true}]);
    c.now = T0 + MESSAGE_PILL_MS;
    c.model.expire();
    check('pill gone: bubble at rest, still moving', c.model.view().icon, {state: 'unread', moving: true});
    c.now = T0 + ATTENTION_MS - 1;
    check('one second short of the minute: still moving', c.model.view().icon.moving, true);
    c.now = T0 + ATTENTION_MS;
    const before = c.changes;
    c.model.expire();
    v = c.model.view();
    check('a minute after the message: the bubble stays, still',
        [v.icon, v.unread, c.changes > before, c.model.nextDeadline()], [{state: 'unread', moving: false}, 1, true, null]);
    c.now += 5000;
    c.model.handleEvent(message({handle: 'H2', body: 'Tu es là ?', timestamp: c.now / 1000}));
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('another message: it moves for a minute again', [c.model.view().icon, c.model.view().unread],
        [{state: 'unread', moving: true}, 2]);
    check('the menu says how many', c.model.view().card.unread, 2);
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    check('being read: the bubble does not move', c.model.view().icon, {state: 'unread', moving: false});
    c.model.cardClosed();
    check('read: back to linked', c.model.view().icon, {state: 'linked', moving: false});

    // Ringing, on a call, placing a call.
    c = linked();
    c.model.handleEvent(call('incoming'));
    check('ringing: moves as long as it rings', c.model.view().icon, {state: 'ringing', moving: true});
    c.now += 10 * 60 * 1000;
    check('...ten minutes later too', c.model.view().icon, {state: 'ringing', moving: true});
    c.model.handleEvent(call('active'));
    check('on a call: the slow wave', c.model.view().icon, {state: 'call', moving: true});
    c.model.handleEvent({command: 'bt_calls', calls: []});
    check('call over: linked', c.model.view().icon, {state: 'linked', moving: false});
    c.model.startDial({number: '+41790000001', name: 'Camille Martin'});
    c.model.dial('+41790000001', 'phone', 'Camille Martin');
    check('placing a call: drawn like a call', [c.model.view().mode, c.model.view().icon], ['dialing', {state: 'call', moving: true}]);

    // A missed call: until the menu is opened.
    c = linked();
    c.model.handleEvent(note());
    v = c.model.view();
    check('missed call: the arrow, and the reader hears it', [v.icon, v.a11y], [{state: 'missed', moving: false}, 'iPhone linked, missed call']);
    check('missed call: still listed on the desktop', c.shown.filter(([kind]) => kind === 'show').length, 1);
    c.model.handleEvent(message());
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('missed call and unread message: the missed call first', c.model.view().icon.state, 'missed');
    c.model.cardOpened();
    c.model.cardClosed();
    check('menu opened: the arrow goes, the bubble is left', c.model.view().icon.state, 'unread');
    c = linked();
    c.model.handleEvent(note());
    c.model.handleEvent({command: 'bt_notification_removed', uid: 70});
    check('missed call looked at on the iPhone: the arrow goes', c.model.view().icon.state, 'linked');
    c = linked();
    c.model.handleEvent(note({silent: true}));
    c.model.handleEvent({command: 'bt_notifications', notifications: [note({uid: 71, timestamp: T0 / 1000 - 500})]});
    check('missed calls already on the iPhone: no arrow', c.model.view().icon.state, 'linked');
    c.model.handleEvent(note({uid: 90}));
    check('...nor when the same one is sent again under another number', c.model.view().icon.state, 'linked');
    c = linked();
    c.model.handleEvent(message());
    c.model.cardOpened();
    c.model.handleEvent(note());
    c.model.cardClosed();
    check('missed call while a reply card was open: still to be seen', c.model.view().icon.state, 'missed');

    // Voicemail: the notification of the Phone app, category 3.
    c = linked();
    c.model.handleEvent(note({uid: 80, title: 'Voicemail', body: 'Camille Martin', category: 3}));
    v = c.model.view();
    check('voicemail: the spools turn for a minute', [v.icon, v.a11y, c.model.nextDeadline()],
        [{state: 'voicemail', moving: true}, 'iPhone linked, new voicemail', T0 + ATTENTION_MS]);
    check('voicemail: listed on the desktop too', c.shown.filter(([kind]) => kind === 'show').length, 1);
    c.now = T0 + ATTENTION_MS;
    c.model.expire();
    check('...then stay still', [c.model.view().icon, c.model.nextDeadline()], [{state: 'voicemail', moving: false}, null]);
    c.model.cardOpened();
    c.model.cardClosed();
    check('menu opened: no spools any more', c.model.view().icon.state, 'linked');

    // Notifications paused: the ring blinks a minute, then stays.
    c = linked();
    c.now += 5 * 60 * 1000;
    c.model.handleEvent(conn({le_connected: false, ancs_ready: false, le_state: 'paused', le_cause: 'phone_silent'}));
    check('paused: the ring blinks', [c.model.view().icon, c.model.nextDeadline()],
        [{state: 'paused', moving: true}, c.now + ATTENTION_MS]);
    c.now += ATTENTION_MS;
    c.model.expire();
    check('...a minute, then stays', [c.model.view().icon, c.model.nextDeadline()], [{state: 'paused', moving: false}, null]);
    c.model.handleEvent(conn({le_connected: false, ancs_ready: false, le_state: 'paused', le_cause: 'phone_silent', battery: 95}));
    check('still paused, something else changed: it does not blink again', c.model.view().icon.moving, false);
    c.model.handleEvent(message({timestamp: c.now / 1000}));
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('paused, unread: the bubble', c.model.view().icon.state, 'unread');

    // Silence: no bubble, no arrow, nothing moves.
    c = linked();
    c.model.setQuiet({dnd: true});
    c.model.handleEvent(message());
    c.model.handleEvent(note());
    c.model.handleEvent(call('incoming'));
    v = c.model.view();
    check('Do Not Disturb: the icon at rest whatever arrives', [v.icon, v.unread, c.model.nextDeadline()],
        [{state: 'linked', moving: false}, 1, null]);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.handleEvent(conn({le_connected: false, ancs_ready: false, le_state: 'paused', le_cause: 'phone_silent'}));
    check('Do Not Disturb, notifications paused: the ring, still', c.model.view().icon, {state: 'paused', moving: false});
    c.model.setQuiet({dnd: false});
    check('silence over: what waits is shown, nothing moves again', c.model.view().icon, {state: 'missed', moving: false});
    c = linked();
    c.model.setQuiet({dnd: true, callsThroughDnd: true});
    c.model.handleEvent(call('incoming'));
    check('calls let through Do Not Disturb: it rings', c.model.view().icon, {state: 'ringing', moving: true});

    // Sent: the check mark takes the place of the icon.
    c = linked();
    c.model.handleEvent(message());
    c.model.cardOpened();
    c.model.reply('Oui');
    check('sending: bare glyph', [c.model.view().icon.state, c.model.view().pill.check], ['bare', undefined]);
    c.model.handleEvent({command: 'bt_send_result', operation_id: c.sent.find(x => x.command === 'bt_send_message').operation_id, success: true});
    check('sent: the check mark', c.model.view().pill, {tone: 'neutral', check: true, strong: 'Sent'});
    c.model.setEngineState('absent');
    c.model.setEngineState('connected');
    check('engine gone and back: nothing kept', [c.model._missed.size, c.model._voicemail.size, c.model._attentionAt], [0, 0, null]);
}

// --- the sound of a call placed from here (engine of 17 November)
{
    // The classic link is closed: the engine cannot say yet that this
    // computer can carry the sound. The choice is offered and sent anyway.
    const c = leanLinked();
    c.model.handleEvent(audioState({call_audio: 'phone', call_audio_switchable: false}));
    c.model.cardOpened();
    c.model.startDial({number: '+41790000001', name: 'Camille Martin'});
    check('Call…: This computer by default, whatever the setting for incoming calls, link closed or not',
        c.model.view().card.audio, 'computer');
    c.sent.length = 0;
    c.model.dialPick(0);
    const id = c.sent[0].operation_id;
    check('audio is sent although call_audio_switchable is false', c.sent,
        [{command: 'bt_call_dial', number: '+41790000001', operation_id: id, audio: 'here'}]);
    c.model.handleEvent({command: 'bt_call_progress', operation_id: id, number: '+41790000001', stage: 'dialing'});
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: true, operation_id: id});
    c.now += 10000;
    c.model.expire();
    check('placed, ten seconds on: still waiting for the call to show', [c.model.view().mode, c.model.view().card.subtitle], ['dialing', 'Calling…']);
    c.model.handleEvent({command: 'bt_call_progress', operation_id: id, number: '+41790000001', stage: 'no_call'});
    let v = c.model.view();
    check('no_call: the call didn’t start, Try Again offered',
        [v.mode, v.card.kind, v.card.failed, v.card.subtitle, v.card.isError], ['idle', 'dialing', true, 'The call didn’t start.', true]);
    c.sent.length = 0;
    c.model.retryDial();
    check('Try Again: the same call, the same sound', [c.sent[0].number, c.sent[0].audio], ['+41790000001', 'here']);
    const again = c.sent[0].operation_id;
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: true, operation_id: again});
    c.now += 25000;
    c.model.expire();
    check('placed and the engine silent: said the same way', [c.model.view().card.failed, c.model.view().card.subtitle], [true, 'The call didn’t start.']);
    c.model.cardClosed();

    // The call shows up; the sound was asked here and follows.
    const calls = (extra = {}) => ({operator: '', audio: 'idle', audio_switchable: true, audio_route: 'phone', ...extra});
    const up = extra => lean({classic_connected: true, classic_state: 'open', calls_available: 'ready', calls: calls(extra)});
    c.model.cardOpened();
    c.model.startDial({number: '+41790000001', name: 'Camille Martin'});
    c.model.setDialAudio('phone');
    c.sent.length = 0;
    c.model.dialPick(0);
    check('iPhone chosen: sent too', c.sent[0].audio, 'phone');
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: true, operation_id: c.sent[0].operation_id});
    c.model.handleEvent(up({audio_wanted: 'phone'}));
    c.model.handleEvent(call('alerting'));
    v = c.model.view();
    check('sound wanted on the iPhone: nothing on its way', [v.mode, v.card.audio, v.card.moving], ['call', 'phone', false]);
    c.model.handleEvent({command: 'bt_calls', calls: []});

    c.model.handleEvent(up({audio_wanted: 'here'}));
    c.model.handleEvent(call('alerting'));
    v = c.model.view();
    check('sound wanted here, not here yet: shown as on its way, not as done',
        [v.mode, v.card.audio, v.card.moving, /moving to this computer/.test(v.card.subtitle), v.card.isError],
        ['call', 'computer', true, true, false]);
    c.now += 8000;
    c.model.expire();
    check('eight seconds on, the engine still asking: no error of our own', [c.model.view().card.moving, c.model.view().card.isError], [true, false]);
    c.model.handleEvent(up({audio_wanted: 'here', audio_route: 'here', audio: 'active'}));
    c.model.handleEvent({command: 'bt_call_progress', stage: 'audio_here'});
    v = c.model.view();
    check('audio_here: the sound is on this computer', [v.card.audio, v.card.moving, v.card.isError, /sound on this computer/.test(v.card.subtitle)],
        ['computer', false, false, true]);
    c.model.handleEvent({command: 'bt_calls', calls: []});

    // It does not come: said, and the call goes on with the iPhone.
    c.model.handleEvent(up({audio_wanted: 'here'}));
    c.model.handleEvent(call('active'));
    c.model.handleEvent({command: 'bt_call_progress', stage: 'audio_failed', reason: 'no_voice_link'});
    c.model.handleEvent(up({audio_wanted: 'here', audio_failed: true, reason: 'no_voice_link'}));
    v = c.model.view();
    check('audio_failed: said, the call goes on with the iPhone',
        [v.mode, v.card.audio, v.card.moving, v.card.isError, v.card.subtitle, v.card.hint],
        ['call', 'phone', false, true, 'Couldn’t bring the call sound to this computer', 'The sound is still on the iPhone.']);
    c.sent.length = 0;
    c.model.hangUp();
    check('...and can still be ended from here', c.sent.map(s => s.action), ['hangup']);
    c.model.handleEvent(up({audio_wanted: 'here', audio_route: 'here', audio: 'active'}));
    check('the sound comes after all: the error goes', [c.model.view().card.audio, c.model.view().card.isError], ['computer', false]);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.handleEvent(up());
    c.model.handleEvent(call('active'));
    c.model.handleEvent({command: 'bt_call_progress', stage: 'audio_failed', reason: 'voice_link_unused'});
    check('voice link opened and played nowhere: the card says what to do',
        /silent on both sides/.test(c.model.view().card.hint), true);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    check('call over: nothing left of it', [c.model.view().mode, c.model._callError, c.model._audioFailed], ['idle', '', null]);

    // Read from `calls` alone, by an extension enabled during the call.
    const late = leanLinked();
    late.model.handleEvent(audioState());
    late.model.handleEvent(call('active'));
    late.model.handleEvent(up({audio_wanted: 'here', audio_failed: true, reason: 'no_voice_link'}));
    check('calls.audio_failed alone: the card says it too',
        [late.model.view().card.isError, late.model.view().card.subtitle], [true, 'Couldn’t bring the call sound to this computer']);

    // Hands-free up and this computer cannot carry the sound: no choice.
    const cannot = leanLinked({calls_available: 'ready', classic_connected: true});
    cannot.model.handleEvent(audioState({call_audio_switchable: false}));
    cannot.model.cardOpened();
    cannot.model.startDial({number: '+41790000001', name: ''});
    cannot.sent.length = 0;
    check('hands-free up, sound cannot come here: no choice shown', cannot.model.view().card.audio, null);
    cannot.model.dialPick(0);
    check('...and none sent', cannot.sent[0].audio, undefined);

    // Answering a call the old way, with the engine unsure the sound can come.
    const ring = linked();
    ring.model.handleEvent(audioState({call_audio_switchable: false}));
    ring.model.handleEvent(call('incoming'));
    ring.sent.length = 0;
    ring.model.answer('phone');
    check('answer: audio sent although call_audio_switchable is false', ring.sent,
        [{command: 'bt_call_action', action: 'answer', path: '/call1', audio: 'phone'}]);
}

// ============================================================================
// Reconnecting: between away and linked (issue #30)
// ============================================================================
{
    // The rules alone.
    const facts = (extra = {}) => ({
        link: 'away', confirmed: false, shown: 'away', presence: null, leState: null, leCause: null,
        upFor: null, signAgo: null, fails: 0, ...extra,
    });
    const here = {state: 'here', reason: ''};
    const away = (reason = 'out_of_range') => ({state: 'away', reason});
    check('rules: away and nothing seen: away', reconnectReason(facts({presence: away()})), null);
    check('rules: the iPhone seen, no useful link: back', reconnectReason(facts({presence: here})), 'back');
    check('rules: a link up, not served yet, on the way back: setting up',
        reconnectReason(facts({link: 'linked', upFor: 2000})), 'setting_up');
    check('rules: ...not while it was shown linked (a blink)',
        reconnectReason(facts({link: 'linked', upFor: 2000, shown: 'linked'})), null);
    check('rules: ...nor after a pairing (shown "Link iPhone")',
        reconnectReason(facts({link: 'linked', upFor: 2000, shown: 'setup'})), null);
    check('rules: ...nor once served', reconnectReason(facts({link: 'linked', upFor: 2000, confirmed: true})), null);
    check('rules: ...nor after 15 s without service',
        reconnectReason(facts({link: 'linked', upFor: RECONNECT_SETUP_MS})), null);
    check('rules: the engine holds off: retrying, even out of range',
        reconnectReason(facts({presence: away(), leState: 'waiting', leCause: 'holding_off'})), 'retrying');
    check('rules: unsteady, the iPhone seen a moment ago: retrying',
        reconnectReason(facts({presence: away(), leState: 'waiting', leCause: 'unsteady', signAgo: 20000})), 'retrying');
    check('rules: unsteady, nothing seen for a minute: away',
        reconnectReason(facts({presence: away(), leState: 'waiting', leCause: 'unsteady', signAgo: RECONNECT_HOLD_MS})), null);
    check('rules: just woke up (engine): sleep',
        reconnectReason(facts({presence: away('computer_suspended'), leState: 'waiting', leCause: 'after_resume'})), 'sleep');
    check('rules: woke up, older engine, a moment ago: sleep',
        reconnectReason(facts({presence: away('computer_suspended'), signAgo: 1000})), 'sleep');
    check('rules: woke up, older engine, long ago: away',
        reconnectReason(facts({presence: away('computer_suspended'), signAgo: 10 * 60000})), null);
    check('rules: two links died unused: dropping', reconnectReason(facts({presence: away(), fails: 2})), 'dropping');
    check('rules: one, the iPhone still seen: dropping', reconnectReason(facts({presence: here, fails: 1})), 'dropping');
    check('rules: one, out of range: walking away', reconnectReason(facts({presence: away(), fails: 1})), null);
    check('rules: Bluetooth off on the iPhone: never',
        [reconnectReason(facts({presence: away('switched_off'), leState: 'waiting', leCause: 'holding_off'})),
            reconnectReason(facts({presence: away('switched_off'), fails: 3, signAgo: 0}))], [null, null]);
    check('rules: paused (180 s): never',
        reconnectReason(facts({presence: here, leState: 'paused', leCause: 'phone_silent'})), null);
    check('rules: notifications paused in the engine: never (#42)',
        [reconnectReason(facts({presence: here, enabled: false})),
            reconnectReason(facts({presence: away(), leState: 'waiting', leCause: 'holding_off', enabled: false})),
            reconnectReason(facts({link: 'linked', upFor: 2000, enabled: false}))], [null, null, null]);
    check('rules: enabled said outright changes nothing', reconnectReason(facts({presence: here, enabled: true})), 'back');
    check('rules: other states are not concerned',
        ['bluetooth', 'broken', 'setup', 'engine'].map(link => reconnectReason(facts({link, presence: here}))), [null, null, null, null]);
    check('the engine holds off 15 s, 30 s, then twice as long, 120 s at most',
        [0, 1, 2, 3, 4, 5, 9].map(holdOffSeconds), [null, 15, 30, 60, 120, 120, 120]);

    // The model, with the steady indicator, in light mode.
    const lean = (extra = {}) => conn({
        classic_connected: false, map_open: false, pbap_open: false,
        link_mode: 'lean', link_mode_effective: 'lean', link_mode_reason: '',
        le_state: 'connected', le_cause: '', le_detail: '', le_strikes: 0, ...extra,
    });
    const leDown = (cause = 'reconnecting', extra = {}) => lean({le_connected: false, ancs_ready: false, battery: null,
        le_state: 'waiting', le_cause: cause, ...extra});
    // A link that comes up and dies before the iPhone serves it.
    const leBare = (extra = {}) => lean({ancs_ready: false, ...extra});
    const presence = (state, reason = '') => ({command: 'bt_presence', state, reason, away_seconds: 3, lock: 'off'});
    const at = (c, ms) => {
        c.now = T0 + ms;
    };
    const tick = (c, ms) => {
        at(c, ms);
        c.model.expire();
    };
    const start = () => {
        const c = setup(true);
        c.model.setEngineState('connected');
        c.model.handleEvent(status({link_mode: 'lean'}));
        c.model.handleEvent(lean());
        c.model.handleEvent(presence('here'));
        return c;
    };

    // 18 November, on the way back home: the iPhone was away, then tried
    // several times over three minutes before the link held.
    let c = start();
    check('home scenario: linked to begin with', c.model.view().link, 'linked');
    at(c, 1000);
    c.model.handleEvent(leDown());
    c.model.handleEvent(presence('away', 'out_of_range'));
    tick(c, 1000 + LINK_SETTLE_MS);
    let v = c.model.view();
    check('walk out: away, grey and still', [v.link, v.icon.state, v.icon.moving, v.card.notices],
        ['away', 'away', false, ['Out of range']]);
    tick(c, 30 * 60000);
    check('half an hour out of range: still away, nothing pending', [c.model.view().link, c.model.nextDeadline()],
        ['away', null]);
    // Back at the door: a link comes up and dies, the engine holds off.
    const seen = new Set();
    const look = () => seen.add(c.model.view().link);
    let ms = 31 * 60000;
    at(c, ms);
    c.model.handleEvent(presence('here'));
    c.model.handleEvent(leBare());
    look();
    at(c, ms += 1000);
    c.model.handleEvent(leDown('holding_off', {le_strikes: 1}));
    c.model.handleEvent(presence('away', 'out_of_range'));
    look();
    tick(c, ms += LINK_SETTLE_MS);
    v = c.model.view();
    check('first link died, the engine waits: reconnecting, moving', [v.link, v.icon.state, v.icon.moving, v.pill],
        ['reconnecting', 'reconnecting', true, {tone: 'quiet'}]);
    check('...the menu: one line, what comes next',
        [v.card.notices, v.card.linked], [['The link keeps dropping, trying again in 15 s'], false]);
    check('...read aloud', v.a11y, 'Reconnecting to the iPhone');
    c.model.cardOpened();
    check('menu open: the countdown ticks every 5 s', c.model.nextDeadline() - c.now, 2000);
    c.model.cardClosed();
    // The pause is over: the engine calls again.
    at(c, ms += 12000);
    c.model.handleEvent(leDown('unsteady', {le_strikes: 1}));
    look();
    check('the engine calls again: trying again', c.model.view().card.notices, ['The link keeps dropping, trying again…']);
    at(c, ms += 20000);
    c.model.handleEvent(presence('here'));
    c.model.handleEvent(leBare({le_strikes: 1}));
    look();
    at(c, ms += 1200);
    c.model.handleEvent(leDown('holding_off', {le_strikes: 2}));
    c.model.handleEvent(presence('away', 'out_of_range'));
    look();
    check('second link died: 30 s', c.model.view().card.notices, ['The link keeps dropping, trying again in 30 s']);
    tick(c, ms += 30000);
    c.model.handleEvent(leDown('unsteady', {le_strikes: 2}));
    look();
    tick(c, ms += 40000);
    look();
    check('a minute and more without a link: still reconnecting, the engine says it calls', c.model.view().link, 'reconnecting');
    at(c, ms += 5000);
    c.model.handleEvent(presence('here'));
    c.model.handleEvent(leBare({le_strikes: 2}));
    look();
    tick(c, ms += 1500);
    look();
    check('a link up, not served yet: still reconnecting, almost there', c.model.view().card.notices[0],
        'Almost there, getting notifications ready…');
    at(c, ms += 1000);
    c.model.handleEvent(lean({le_strikes: 0}));
    look();
    v = c.model.view();
    check('served: linked at once, still', [v.link, v.icon.state, v.icon.moving], ['linked', 'linked', false]);
    check('the way back went away → reconnecting → linked, never Link again', [...seen], ['away', 'reconnecting', 'linked']);
    check('nothing left to wait for', c.model.nextDeadline(), null);

    // A served link that blinks never says reconnecting.
    c = start();
    seen.clear();
    for (const [down, back] of [[1000, 3000], [40000, 46000], [80000, 89000]]) {
        at(c, down);
        c.model.handleEvent(leDown());
        look();
        tick(c, (down + back) / 2);
        look();
        at(c, back - 500);
        c.model.handleEvent(leBare());
        look();
        at(c, back);
        c.model.handleEvent(lean());
        look();
    }
    check('served link dropping for 2, 6 and 9 s, the iPhone still here: linked all along', [...seen], ['linked']);
    at(c, 120000);
    c.model.handleEvent(leDown());
    tick(c, 120000 + RECONNECT_AFTER_LINK_MS - 1);
    check('...a drop of nearly 10 s: still linked', c.model.view().link, 'linked');
    tick(c, 120000 + RECONNECT_AFTER_LINK_MS);
    check('...10 s: reconnecting, the iPhone being here', [c.model.view().link, c.model.view().card.notices[0]],
        ['reconnecting', 'Your iPhone is back, setting up the link…']);

    // Bluetooth turned off on the iPhone: away, whatever the engine does.
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown('unsteady'));
    c.model.handleEvent(presence('away', 'switched_off'));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS + LINK_SETTLE_MS);
    v = c.model.view();
    check('switched off on the iPhone: away, said so, nothing moves',
        [v.link, v.icon.state, v.icon.moving, v.card.notices], ['away', 'away', false, ['Bluetooth is off on the iPhone']]);
    c.model.handleEvent(leDown('holding_off', {le_strikes: 1}));
    tick(c, 30000);
    check('...even holding off', c.model.view().link, 'away');

    // Seen, never linked: said five minutes at most.
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown());
    c.model.handleEvent(presence('away', 'out_of_range'));
    tick(c, 10000);
    at(c, 20000);
    c.model.handleEvent(presence('here'));
    tick(c, 20000 + LINK_SETTLE_MS);
    check('the iPhone seen, no link: reconnecting', c.model.view().link, 'reconnecting');
    tick(c, 20000 + RECONNECT_MAX_MS - 1);
    check('...for nearly five minutes', c.model.view().link, 'reconnecting');
    check('...the model asks to be called back when it is over', c.model.nextDeadline(), T0 + 20000 + RECONNECT_MAX_MS);
    tick(c, 20000 + RECONNECT_MAX_MS);
    tick(c, 20000 + RECONNECT_MAX_MS + LINK_SETTLE_MS);
    v = c.model.view();
    check('...then away, and why', [v.link, v.icon.moving, v.card.notices],
        ['away', false, ['Nearby, not linked']]);
    const spentAt = 20000 + RECONNECT_MAX_MS + LINK_SETTLE_MS;
    at(c, spentAt + 1000);
    c.model.handleEvent(presence('away', 'out_of_range'));
    at(c, spentAt + 2000);
    c.model.handleEvent(presence('here'));
    tick(c, spentAt + 2000 + 3 * LINK_SETTLE_MS);
    check('...away for a second, seen again: not said again (#42)', c.model.view().link, 'away');
    at(c, spentAt + 20000);
    c.model.handleEvent(presence('away', 'out_of_range'));
    tick(c, spentAt + 20000 + RECONNECT_CALM_MS);
    at(c, spentAt + 20000 + RECONNECT_CALM_MS + 1000);
    c.model.handleEvent(presence('here'));
    tick(c, spentAt + 20000 + RECONNECT_CALM_MS + 1000 + 3 * LINK_SETTLE_MS);
    check('...seen again, after a calm moment: said again', c.model.view().link, 'reconnecting');

    // The five minutes run on when the reason goes away for a moment (#42).
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown());
    c.model.handleEvent(presence('away', 'out_of_range'));
    tick(c, 10000);
    at(c, 20000);
    c.model.handleEvent(presence('here'));
    tick(c, 20000 + LINK_SETTLE_MS);
    check('cap: the iPhone seen, no link: reconnecting', c.model.view().link, 'reconnecting');
    for (const t of [100000, 200000]) {
        at(c, t);
        c.model.handleEvent(presence('away', 'out_of_range'));
        at(c, t + RECONNECT_CALM_MS - 1000);
        c.model.handleEvent(presence('here'));
    }
    tick(c, 20000 + RECONNECT_MAX_MS - 1);
    check('cap: ...away twice for a few seconds, still reconnecting', c.model.view().link, 'reconnecting');
    check('cap: ...the five minutes still count from the start', c.model.nextDeadline() <= T0 + 20000 + RECONNECT_MAX_MS, true);
    tick(c, 20000 + RECONNECT_MAX_MS);
    tick(c, 20000 + RECONNECT_MAX_MS + LINK_SETTLE_MS);
    check('cap: ...then away, on time', c.model.view().link, 'away');
    at(c, 20000 + RECONNECT_MAX_MS + 10000);
    c.model.handleEvent(lean());
    at(c, 20000 + RECONNECT_MAX_MS + 11000);
    c.model.handleEvent(leDown());
    c.model.handleEvent(presence('here'));
    tick(c, 20000 + RECONNECT_MAX_MS + 11000 + RECONNECT_AFTER_LINK_MS + LINK_SETTLE_MS);
    check('cap: ...a confirmed link in between starts it afresh', c.model.view().link, 'reconnecting');

    // Notifications paused in the engine: the menu says Paused, nothing moves (#42).
    c = start();
    c.model.handleEvent(status({link_mode: 'lean', enabled: false}));
    at(c, 1000);
    c.model.handleEvent(leDown('unsteady', {le_strikes: 2}));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS + LINK_SETTLE_MS);
    v = c.model.view();
    check('paused, the iPhone still here: not reconnecting, the pause mark, still, the menu says Paused',
        [v.link === 'reconnecting', v.icon.state, v.icon.moving, v.card.notices],
        [false, 'paused', false, ['Paused']]);
    c.model.handleEvent(status({link_mode: 'lean', enabled: true}));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS + 2 * LINK_SETTLE_MS);
    check('...resumed: reconnecting again', c.model.view().link, 'reconnecting');

    // Woke up: the engine says so.
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown('after_resume'));
    c.model.handleEvent(presence('away', 'computer_suspended'));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS);
    v = c.model.view();
    check('after sleep: reconnecting, said so', [v.link, v.card.notices], ['reconnecting', ['Reconnecting after sleep…']]);
    at(c, 200000);
    c.model.handleEvent(leDown('after_resume', {le_state: 'paused', le_detail: 'Since this computer woke up…'}));
    tick(c, 200000 + LINK_SETTLE_MS);
    v = c.model.view();
    check('after 180 s the engine calls it a pause: away, and the pause is explained',
        [v.link, v.icon.moving, v.card.notices, v.card.paused?.cause],
        ['away', false, ['Not back since this computer slept'], 'after_resume']);

    // An engine that no longer holds off (#29): links that die, a new try.
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown());
    c.model.handleEvent(presence('away', 'out_of_range'));
    tick(c, 1000 + LINK_SETTLE_MS);
    for (const t of [60000, 64000]) {
        at(c, t);
        c.model.handleEvent(presence('here'));
        c.model.handleEvent(leBare());
        at(c, t + 900);
        c.model.handleEvent(leDown('unsteady', {le_strikes: t === 60000 ? 1 : 2}));
        c.model.handleEvent(presence('away', 'out_of_range'));
    }
    tick(c, 64900 + LINK_SETTLE_MS);
    v = c.model.view();
    check('no holding off: reconnecting, trying again, no countdown', [v.link, v.card.notices],
        ['reconnecting', ['The link keeps dropping, trying again…']]);
    tick(c, 64900 + RECONNECT_HOLD_MS);
    tick(c, 64900 + RECONNECT_HOLD_MS + LINK_SETTLE_MS);
    check('...a minute without any sign: away', c.model.view().link, 'away');

    // An older engine (no le_state): links that keep dying while the
    // iPhone is here are still "Link again" after the latch, as before.
    c = setup(true);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    c.model.handleEvent(presence('here'));
    const down = conn({classic_connected: false, le_connected: false, map_open: false, pbap_open: false,
        ancs_ready: false, battery: null});
    const bare = conn({map_open: false, pbap_open: false, ancs_ready: false});
    for (const t of [1000, 3000, 5000, 7000]) {
        at(c, t);
        c.model.handleEvent(down);
        at(c, t + 1000);
        c.model.handleEvent(bare);
    }
    at(c, 9000);
    c.model.handleEvent(down);
    tick(c, 9000 + RECONNECT_AFTER_LINK_MS + LINK_SETTLE_MS);
    check('older engine, the latch of links that keep dropping: Link again, not reconnecting',
        [c.model.view().link, c.model.view().pill.strong], ['broken', 'Link again']);

    // In silence the icon says it still.
    c = start();
    c.model.setQuiet({dnd: true});
    at(c, 1000);
    c.model.handleEvent(leDown('holding_off', {le_strikes: 1}));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS);
    v = c.model.view();
    check('Do Not Disturb: reconnecting, still', [v.icon.state, v.icon.moving], ['reconnecting', false]);

    // The battery level known last stays, marked as such.
    c = start();
    at(c, 1000);
    c.model.handleEvent(leDown('holding_off', {le_strikes: 1}));
    tick(c, 1000 + RECONNECT_AFTER_LINK_MS);
    v = c.model.view();
    check('reconnecting: the last battery level, marked old', [v.card.battery, v.card.batteryStale], [96, true]);
}

// --- the drawings of the icon: every state of both styles, every loop
{
    const problems = [];
    for (const style of ICON_STYLES) {
        for (const state of ICON_STATES) {
            const layers = iconLayers(style, state);
            if (layers.length === 0)
                problems.push(`${style}/${state}: empty`);
            for (const layer of layers) {
                const svg = layerSvg(layer, '#ffffff');
                if (!svg.startsWith('<svg') || svg.includes('{c}') || !svg.includes('#ffffff'))
                    problems.push(`${style}/${state}: bad image`);
                if (!['glyph', 'amber', 'green', 'red'].includes(layer.ink))
                    problems.push(`${style}/${state}: unknown ink ${layer.ink}`);
                if (layer.motion && !MOTIONS[layer.motion])
                    problems.push(`${style}/${state}: unknown motion ${layer.motion}`);
                const turns = layer.motion && ('rotation' in MOTIONS[layer.motion].tracks || 'scale' in MOTIONS[layer.motion].tracks);
                if (turns && !layer.pivot)
                    problems.push(`${style}/${state}: a layer that turns or grows without a pivot`);
                if (layer.pivot && layer.pivot.some(n => n < 0 || n > 16))
                    problems.push(`${style}/${state}: pivot out of the grid`);
            }
        }
    }
    check('every state of both styles has a drawing made of sound layers', problems, []);
    const loops = Object.entries(MOTIONS).filter(([, m]) => !(m.duration > 0) || Object.values(m.tracks).some(frames =>
        frames[0][0] !== 0 || frames[frames.length - 1][0] !== 1 || frames.some(([at], i) => i > 0 && at < frames[i - 1][0])));
    check('every loop runs from 0 to 1, keyframes in order', loops.map(([name]) => name), []);
    const moving = (style, state) => iconLayers(style, state).filter(l => l.motion).map(l => l.motion);
    check('what moves, state by state, in both styles',
        ICON_STYLES.map(style => ICON_STATES.filter(state => moving(style, state).length > 0)),
        [['reconnecting', 'unread', 'ringing', 'call', 'voicemail', 'paused', 'internet'],
            ['reconnecting', 'unread', 'ringing', 'call', 'voicemail', 'paused', 'internet']]);
    check('Phone, linked: the phone alone; Handset, linked: its amber dot',
        [iconLayers('phone', 'linked').map(l => l.ink), iconLayers('handset', 'linked').map(l => l.ink)],
        [['glyph'], ['glyph', 'amber']]);
    check('the labelled states never wear the dot of "linked"', iconLayers('handset', 'bare').map(l => l.ink), ['glyph']);
    check('an unknown style falls back on the phone', iconLayers('nope', 'linked'), iconLayers('phone', 'linked'));
    const inks = {glyph: '#ffffff', amber: '#f2b45c', green: '#6fd58e', red: '#ff8a7a'};
    const still = stateSvg('handset', 'call', inks, {size: 64, pad: 4, background: '#1d1d20'});
    check('a whole state as one image, for Phone Settings: every layer, in its color',
        [still.startsWith('<svg'), still.includes('{c}'), still.split('<g ').length - 1, still.includes('#6fd58e'), still.includes('viewBox="-4 -4 24 24"')],
        [true, false, iconLayers('handset', 'call').length, true, true]);
}

// --- #51: the icon says how the link is; the menu says nothing when all is well
{
    // The marks: one drawing each, in both styles, amber for something to do.
    check('the marks are states of the icon', ['away', 'reconnecting', 'paused', 'attention', 'battery', 'internet_failed']
        .every(state => ICON_STATES.includes(state)), true);
    check('attention: an amber mark beside the glyph, in both styles',
        ICON_STYLES.map(style => iconLayers(style, 'attention').map(l => l.ink)), [['glyph', 'amber'], ['glyph', 'amber']]);
    check('...never the plain dot of "linked" (Handset)',
        iconLayers('handset', 'attention')[1].art === iconLayers('handset', 'linked')[1].art, false);
    check('paused: two bars, not a ring, in both styles',
        ICON_STYLES.map(style => /M[\d.]+ [\d.]+v[\d.]+M[\d.]+ [\d.]+v[\d.]+/.test(iconLayers(style, 'paused')[1].art)), [true, true]);

    const c = linked();
    let v = c.model.view();
    check('all well: the bare icon, a menu without a word about the link',
        [v.icon.state, v.card.notices, v.card.paused, v.a11y], ['linked', [], null, 'iPhone linked']);
    check('all well: no state, no status line, no steady line or action in the menu',
        ['state', 'statusLine', 'steady', 'steadyAction'].filter(key => key in v.card), []);
    check('all well: nothing to do', [v.card.permissionAction, v.card.setupAction, v.card.fixAction, v.card.updateAction],
        [false, false, false, false]);
    check('all well: the battery, not red, charging not known', [v.card.battery, v.card.batteryLow, v.card.batteryCharging],
        [96, false, null]);

    // One thing after the other goes wrong; the icon wears one mark, the
    // menu says one line, the first by priority.
    const said = () => {
        const w = c.model.view();
        return [w.icon.state, w.card.notices, w.card.paused?.cause ?? null,
            [w.card.permissionAction, w.card.setupAction, w.card.fixAction, w.card.updateAction]];
    };
    c.model.setEngineInstall({installed: true, outdated: true});
    check('an older engine: the amber mark, one line, Update',
        said(), ['attention', ['A newer engine is ready to install.'], null, [false, false, false, true]]);
    let now = {map_error: 'forbidden'};
    c.model.handleEvent(conn(now));
    check('a sharing switch off on the iPhone comes first: its line, Continue Setup',
        said(), ['attention', ['Some sharing switches are still off on the iPhone.'], null, [false, true, false, false]]);
    c.model.setSystemSetup({experimental: true, ertm: false, ready: false, persistent: {experimental: true, ertm: true},
        required: {}, elevation: true, command: '', le_params: true});
    check('a setting of this computer missing comes first: its line, Finish Setting Up',
        said(), ['attention', ['Some features are off until a Bluetooth setting of this computer is changed.'], null,
            [true, false, false, false]]);
    now = {...now, le_connected: false, ancs_ready: false, le_state: 'paused', le_cause: 'phone_silent'};
    c.model.handleEvent(conn(now));
    check('notifications paused come first: the pause mark, its own block, no other line',
        said(), ['paused', [], 'phone_silent', [false, false, false, false]]);
    now = {...now, battery: 12, battery_state: 'low'};
    c.model.handleEvent(conn(now));
    c.model.handleEvent({command: 'bt_battery_low', level: 12, threshold: 20, state: 'low', source: 'le', initial: false});
    check('the battery going low comes first, until the menu is opened', said()[0], 'battery');
    check('...and the battery is drawn red in the menu', c.model.view().card.batteryLow, true);
    c.model.handleEvent(message({timestamp: c.now / 1000}));
    c.now += MESSAGE_PILL_MS;
    c.model.expire();
    check('news comes first of all', said()[0], 'unread');
    c.model.setQuiet({dnd: true});
    check('in silence, news is not told: the pause again', said()[0], 'paused');
    c.model.setQuiet({dnd: false});
    // The message read, the menu opened.
    c.model.cardOpened();
    c.model.openThread('tel:+41790000001');
    c.model.cardClosed();
    c.model.handleEvent(conn({...now, le_connected: true, ancs_ready: true, le_state: 'connected', le_cause: ''}));
    check('back, menu seen: the setting still missing is the mark again', said()[0], 'attention');
    c.model.setSystemSetup({experimental: true, ertm: true, ready: true, persistent: {experimental: true, ertm: true},
        required: {}, elevation: true, command: '', le_params: true});
    c.model.handleEvent(conn());
    c.model.setEngineInstall({installed: true, outdated: false});
    v = c.model.view();
    check('all fixed: the bare icon, nothing in the menu', [v.icon.state, v.card.notices], ['linked', []]);

    // The battery: red by what the engine says, else at 20 % or less;
    // charging only when the engine says.
    c.model.handleEvent(conn({battery: 25, battery_state: 'low'}));
    check('battery_state low: red', c.model.view().card.batteryLow, true);
    c.model.handleEvent(conn({battery: 18, battery_state: 'ok'}));
    check('battery_state ok: not red', c.model.view().card.batteryLow, false);
    c.model.handleEvent(conn({battery: 20}));
    check('an engine that does not say: 20 % is red', c.model.view().card.batteryLow, true);
    c.model.handleEvent(conn({battery: 21}));
    check('...21 % is not', c.model.view().card.batteryLow, false);
    c.model.handleEvent(conn({battery: 40, battery_charging: true}));
    check('charging, when the engine says', c.model.view().card.batteryCharging, true);
}

// --- the steadier LE link: a wished setting, never a needed one
{
    const ready = {experimental: true, device_class: false, ertm: true, ready: true,
        persistent: {experimental: true, device_class: false, ertm: true}, required: {experimental: true, ertm: true}};
    const missing = {experimental: false, device_class: false, ertm: false, ready: false, persistent: {},
        required: {experimental: true, ertm: true}};

    // Needed and wished settings missing: one pass, three points.
    const c = unlinked();
    c.model.setSystemSetup({...missing, le_params: false, elevation: true, command: ''});
    c.model.startTour();
    let w = c.model.view().tour;
    check('steady: wished alongside the needed ones, in the same pass',
        [w.step, w.permissionNeeds, w.permissionWishes], ['permission', ['experimental', 'ertm'], ['le_params']]);
    c.model.tourPermission('waiting');
    c.model.setSystemSetup({...ready, le_params: true, elevation: true, command: ''});
    c.model.tourPermission('done');
    check('...granted with the rest: the setup goes on', c.model.view().tour.permission, 'done');

    // The needed ones applied, the wished one could not be made: goes on
    // anyway, and is not offered again.
    const d = unlinked();
    d.model.setSystemSetup({...missing, le_params: false, elevation: true, command: ''});
    d.model.startTour();
    d.model.tourPermission('waiting');
    d.model.setSystemSetup({...ready, le_params: false, elevation: true, command: ''});
    d.model.tourPermission('done');
    check('needed done, wish still missing: done all the same', d.model.view().tour.permission, 'done');
    settle(d);
    check('...the search follows', d.model.view().tour.step, 'find');
    d.model.cancelTour();
    d.model.startTour();
    check('...and the wish is not offered again in this session', d.model.view().tour.step, 'find');

    // Only the wish: offered on its own; turned down, the setup goes on.
    const e = unlinked();
    e.model.setSystemSetup({...ready, le_params: false, elevation: true, command: ''});
    check('only the wish missing: nothing is "needed"', [e.model.view().pill.strong !== 'Permission needed'], [true]);
    e.model.startTour();
    w = e.model.view().tour;
    check('only the wish: the tour offers it first', [w.step, w.permissionNeeds, w.permissionWishes, w.total],
        ['permission', [], ['le_params'], 7]);
    e.model.tourPermission('waiting');
    e.model.tourPermission('refused');
    check('password not given for a wish: let go, nothing blocks', e.model.view().tour.permission, 'skipped');
    settle(e);
    check('...the setup goes on to the search', e.model.view().tour.step, 'find');

    const f = unlinked();
    f.model.setSystemSetup({...ready, le_params: false, elevation: true, command: ''});
    f.model.startTour();
    f.model.tourSkipPermission();
    settle(f);
    check('"Skip" on the wish: on to the search', f.model.view().tour.step, 'find');
    const g = unlinked();
    g.model.setSystemSetup({...missing, le_params: false, elevation: true, command: ''});
    g.model.startTour();
    g.model.tourSkipPermission();
    check('"Skip" does nothing while a needed setting is missing', g.model.view().tour.permission, 'idle');

    e.model.cancelTour();
    e.model.startTour();
    check('turned down: not offered again by the tour in this session', e.model.view().tour.step, 'find');
    e.model.cancelTour();
    e.model.startSteadyTour();
    check('...but asked for from Phone Settings: offered again', e.model.view().tour.step, 'permission');

    // No password window here: a wish is not something to copy in a terminal.
    const h = unlinked();
    h.model.setSystemSetup({...ready, le_params: false, elevation: false, command: 'sudo x apply'});
    h.model.startTour();
    check('no password window: the wish is not offered', h.model.view().tour.step, 'find');
    // An older helper says nothing of it: nothing offered.
    const i = unlinked();
    i.model.setSystemSetup({...ready, elevation: true, command: ''});
    i.model.startTour();
    check('older helper, no le_params field: nothing offered', i.model.view().tour.step, 'find');

    // Linked: the menu says nothing of how tolerant the link is (#51), in
    // any of the states the engine reports.
    const l = linked();
    l.model.setSystemSetup({...ready, le_params: true, elevation: true, command: ''});
    let v = l.model.view();
    check('older engine: no line, no action', ['steady' in v.card, 'steadyAction' in v.card, v.card.notices], [false, false, []]);
    for (const [state, values] of [['pending', null], ['applied', {interval_ms: 60, latency: 0, timeout_ms: 6000}],
        ['refused', {interval_ms: 30, latency: 0, timeout_ms: 720}], ['no_right', null]]) {
        l.model.handleEvent(conn({le_conn_params: state, le_conn_values: values}));
        v = l.model.view();
        check(`steady link ${state}: nothing in the menu, the icon at rest`, [v.card.notices, v.icon.state], [[], 'linked']);
    }
    l.model.setSystemSetup({...ready, le_params: false, elevation: true, command: ''});
    v = l.model.view();
    check('linked, helper missing: not in the menu (Phone Settings offers it), nothing else is asked',
        [v.card.notices, v.card.permissionAction, v.link, v.icon.state], [[], false, 'linked', 'linked']);
    l.model.startSteadyTour();
    w = l.model.view().tour;
    check('...and its click opens the permission step with the wish alone',
        [w.step, w.permissionNeeds, w.permissionWishes], ['permission', [], ['le_params']]);
    l.model.tourPermission('waiting');
    l.model.setSystemSetup({...ready, le_params: true, elevation: true, command: ''});
    l.model.tourPermission('done');
    settle(l);
    check('...granted: the tour ends', l.model.view().tour.step, 'done');
    check('...and the menu still says nothing', l.model.view().card.notices, []);
    const m = linked();
    m.model.setSystemSetup({...ready, ertm: false, ready: false, le_params: false, elevation: true, command: ''});
    v = m.model.view();
    check('a needed setting missing too: the usual entry, the mark of the icon', [v.card.permissionAction, v.icon.state],
        [true, 'attention']);
}

// --- the engine installed or not (Phone Settings › Engine)
{
    const c = setup();
    c.model.setEngineState('absent');
    c.model.setEngineInstall({installed: false, outdated: false});
    let v = c.model.view();
    check('engine not installed: install first, try again second',
        [v.card.title, v.card.action.id, v.card.secondary.id], ['Landline needs its engine', 'install-engine', 'retry']);
    c.model.setEngineInstall({installed: true, outdated: false});
    v = c.model.view();
    check('engine installed but silent: try again first, its settings second',
        [v.card.title, v.card.action.id, v.card.secondary.id], ['The engine is not running', 'retry', 'install-engine']);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    check('current engine: no update offered', c.model.view().card.updateAction, false);
    c.model.setEngineInstall({installed: true, outdated: true});
    check('older engine: the menu offers the update', c.model.view().card.updateAction, true);
}

// --- Internet from iPhone (model/internet.js, lib/hotspot.js)
{
    const at = (c, ms) => {
        c.now = T0 + ms;
    };
    const tick = (c, ms) => {
        at(c, ms);
        c.model.expire();
    };
    // What a failure of NetworkManager's device means.
    check('a refusal before the iPhone accepted anything',
        failureKind(DEVICE.prepare, 44), 'refused');
    check('accepted (BNEP up, asking for an address), then the link went',
        failureKind(DEVICE.ipConfig, 44), 'dropped');
    check('no address in time: no Internet from the iPhone',
        [failureKind(DEVICE.ipConfig, 5), failureKind(DEVICE.ipConfig, 17)], ['no-internet', 'no-internet']);
    check('it was working, then fell (the iPhone walked away, or stopped sharing)',
        [failureKind(DEVICE.activated, 40), failureKind(DEVICE.activated, 44)], ['lost', 'lost']);

    // lib/hotspot.js against a stand-in for libnm.
    const fake = () => {
        const f = {state: 'off', asked: [], views: [], reject: false};
        f.hotspot = new Hotspot(view => f.views.push(view), {timeoutMs: 30});
        f.hotspot.setBackend((onChange, onFailure) => {
            f.onChange = onChange;
            f.onFailure = onFailure;
            return {
                state: () => f.state,
                connect: () => {
                    f.asked.push('up');
                    return f.reject ? Promise.reject(new Error('refused')) : Promise.resolve();
                },
                disconnect: () => {
                    f.asked.push('down');
                    return Promise.resolve();
                },
                destroy: () => {},
            };
        });
        f.hotspot.setAddress('AA:BB:CC:00:00:01');
        return f;
    };
    let f = fake();
    check('NetworkManager has a profile: off', f.hotspot.view, {state: 'off', failed: false, reason: ''});
    f.hotspot.set(true);
    check('asked: connecting at once, before NetworkManager starts', [f.asked, f.hotspot.view.state], [['up'], 'connecting']);
    f.hotspot.set(true);
    check('asked twice: one request', f.asked, ['up']);
    f.state = 'connecting';
    f.onChange();
    f.state = 'off';
    f.onChange();
    check('NetworkManager gave up, its reason not in yet: still connecting', f.hotspot.view.state, 'connecting');
    f.onFailure('aa:bb:cc:00:00:01', 'refused');
    check('the iPhone refused: failed, with the reason', f.hotspot.view, {state: 'off', failed: true, reason: 'refused'});
    f.hotspot.set(true);
    f.state = 'on';
    f.onChange();
    check('on: the failure is forgotten', f.hotspot.view, {state: 'on', failed: false, reason: ''});
    f.onFailure('AA:BB:CC:00:00:01', 'dropped');
    check('a failure nobody asked about, the connection still up: nothing', f.hotspot.view.failed, false);
    f.state = 'off';
    f.onFailure('AA:BB:CC:00:00:01', 'lost');
    check('the connection fell by itself: told', f.hotspot.view, {state: 'off', failed: true, reason: 'lost'});
    f.onFailure('11:22:33:44:55:66', 'refused');
    check('another device failing is not this iPhone', f.hotspot.view.reason, 'lost');
    f.hotspot.set(false);
    check('turned off: no failure, NetworkManager asked', [f.hotspot.view.failed, f.asked.at(-1)], [false, 'down']);

    f = fake();
    f.hotspot.set(true);
    const loop = new GLib.MainLoop(null, false);
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 80, () => {
        loop.quit();
        return GLib.SOURCE_REMOVE;
    });
    loop.run();
    check('no answer in time: failed, and NetworkManager is not left half-way',
        [f.hotspot.view, f.asked], [{state: 'off', failed: true, reason: 'timeout'}, ['up', 'down']]);
    f.hotspot.destroy();

    f = fake();
    f.reject = true;
    f.hotspot.set(true);
    await Promise.resolve();
    await Promise.resolve();
    check('NetworkManager would not even take the request: an error', f.hotspot.view.reason, 'error');

    // The model: one switch.
    const net = (c, state, extra = {}) => c.model.setHotspot({state, failed: false, reason: '', ...extra});
    const asked = c => {
        c.internet = [];
        c.model.onInternet = on => c.internet.push(on);
    };
    let c = linked();
    asked(c);
    check('no word from NetworkManager: no entry', c.model.view().card.internet, null);
    net(c, 'unavailable');
    check('no profile for this iPhone: no entry', c.model.view().card.internet, null);
    net(c, 'off');
    let v = c.model.view();
    check('off: the switch, nothing under it', [v.card.internet.on, v.card.internet.line, v.icon.state], [false, '', 'linked']);
    c.model.setInternet(true);
    check('turned on, the iPhone here: asked of NetworkManager', c.internet, [true]);
    net(c, 'connecting');
    v = c.model.view();
    check('connecting: said, the arrows take turns on the icon',
        [v.card.internet.on, v.card.internet.line, v.icon.state, v.icon.moving, v.a11y],
        [true, 'Connecting to your iPhone…', 'internet', true, 'iPhone linked, connecting to its Internet']);
    net(c, 'on');
    v = c.model.view();
    check('on: the iPhone provides it, with its battery; the icon says it, still',
        [v.card.internet.on, v.card.internet.line, v.icon.state, v.icon.moving, v.a11y],
        [true, 'Online through your iPhone · battery 96 %', 'internet', false, 'iPhone linked, online through the iPhone']);
    c.model.handleEvent(message());
    tick(c, MESSAGE_PILL_MS + 1);
    check('a message unread says more than the connection', c.model.view().icon.state, 'unread');
    c.model.setInternet(false);
    check('turned off from the menu: asked of NetworkManager', c.internet, [true, false]);

    // The iPhone did not share: the one switch to turn on there, drawn.
    c = linked();
    asked(c);
    net(c, 'off');
    net(c, 'off', {failed: true, reason: 'refused'});
    v = c.model.view();
    check('refused: why, the one thing to do, the drawing, try again',
        [v.card.internet.on, v.card.internet.state, v.card.internet.line, v.card.internet.error,
            v.card.internet.remedy, v.card.internet.guide, v.card.internet.guideShown, v.card.internet.action],
        [false, 'failed', 'Your iPhone did not share its connection.', true,
            'On your iPhone, open Settings › Personal Hotspot and turn on Allow Others to Join. Then try again.',
            true, true, {id: 'retry', label: 'Try Again'}]);
    check('refused: the icon says it until the menu is opened',
        [v.icon.state, v.a11y], ['internet_failed', 'iPhone linked, its Internet did not connect']);
    c.model.cardOpened();
    c.model.cardClosed();
    check('...then the icon is back to linked, the menu still says it',
        [c.model.view().icon.state, c.model.view().card.internet.state], ['linked', 'failed']);
    c.model.toggleInternetGuide();
    check('"Hide": the drawing is put away', c.model.view().card.internet.guideShown, false);
    c.model.toggleInternetGuide();
    check('"Show Me How": back', c.model.view().card.internet.guideShown, true);
    c.model.phoneLabel = key => bilingual('fr', key);
    check('the words of a French iPhone, in brackets',
        c.model.view().card.internet.remedy,
        'On your iPhone, open Settings (Réglages) › Personal Hotspot (Partage de connexion) and turn on ' +
        'Allow Others to Join (Autoriser d’autres utilisateurs). Then try again.');
    c.model.phoneLabel = null;
    c.model.setInternet(true);
    check('"Try Again": asked again, the failure put away',
        [c.internet, c.model.view().card.internet.state], [[true], 'off']);
    net(c, 'on');
    net(c, 'off');
    net(c, 'off', {failed: true, reason: 'dropped'});
    check('once it has worked, the drawing waits to be asked for',
        [c.model.view().card.internet.guide, c.model.view().card.internet.guideShown], [true, false]);

    // The other reasons.
    const reason = r => {
        const k = linked();
        net(k, 'off', {failed: true, reason: r});
        const i = k.model.view().card.internet;
        return [i.line, i.remedy, i.guide, i.action?.id ?? null];
    };
    check('no address from the iPhone', reason('no-internet'),
        ['Your iPhone answered, but gave no Internet.', 'Check that Mobile Data is on, on your iPhone. Then try again.',
            false, 'retry']);
    check('no answer in time', reason('timeout'),
        ['Your iPhone did not answer in time.', 'Unlock it and keep it close, then try again.', false, 'retry']);
    check('could not be asked', reason('error'),
        ['This computer could not start the connection.', 'Try again in a moment.', false, 'retry']);
    check('fell while the iPhone stays here: it stopped sharing', reason('lost').slice(0, 3),
        ['Your iPhone stopped sharing its connection.',
            'On your iPhone, open Settings › Personal Hotspot and turn on Allow Others to Join. Then try again.', true]);

    // Turned on while the iPhone is away: it waits, then connects.
    c = linked();
    asked(c);
    net(c, 'off');
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    c.model.setInternet(true);
    v = c.model.view();
    check('the iPhone away: nothing asked, the switch on, waiting',
        [c.internet, v.card.internet.on, v.card.internet.state, v.card.internet.line],
        [[], true, 'waiting', 'Connects as soon as your iPhone is back.']);
    c.model.handleEvent(conn());
    check('back: asked once', c.internet, [true]);
    c.model.handleEvent(conn());
    check('...and only once', c.internet, [true]);

    // Online, then the iPhone walks away: NetworkManager drops it.
    c = linked();
    asked(c);
    net(c, 'on');
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    check('the other link gone, the connection still up: the icon says Internet',
        [c.model.view().icon.state, c.model.view().a11y], ['internet', 'Online through the iPhone']);
    net(c, 'off', {failed: true, reason: 'lost'});
    v = c.model.view();
    check('walked away: stopped, said quietly, offered to reconnect when back',
        [v.card.internet.on, v.card.internet.line, v.card.internet.error, v.card.internet.action],
        [false, 'Stopped when your iPhone went out of range.', false,
            {id: 'keep', label: 'Reconnect When It’s Back'}]);
    c.model.keepInternet();
    check('"Reconnect When It’s Back": waiting, nothing asked yet',
        [c.model.view().card.internet.state, c.internet], ['waiting', []]);
    c.model.handleEvent(conn());
    check('back: reconnected', c.internet, [true]);

    // NetworkManager may notice before the engine does.
    c = linked();
    net(c, 'on');
    at(c, 1000);
    net(c, 'off', {failed: true, reason: 'lost'});
    check('fell, the iPhone still said here: it stopped sharing',
        c.model.view().card.internet.line, 'Your iPhone stopped sharing its connection.');
    at(c, 20000);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    check('...gone 19 s later: it walked away after all',
        c.model.view().card.internet.line, 'Stopped when your iPhone went out of range.');

    // Refused while the iPhone is away (asked from the GNOME tile, say).
    c = linked();
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    net(c, 'off', {failed: true, reason: 'refused'});
    check('refused, the iPhone away: said so, no drawing',
        [c.model.view().card.internet.line, c.model.view().card.internet.guide], ['Your iPhone is not nearby.', false]);

    // The drawings of the icon.
    check('Phone: the phone steps aside for two arrows in the glyph color; red when it failed',
        [iconLayers('phone', 'internet').map(l => l.ink), iconLayers('phone', 'internet_failed').map(l => l.ink)],
        [['glyph', 'glyph', 'glyph'], ['glyph', 'red', 'red']]);
    check('Handset: the arrows where the amber dot of "linked" sits',
        [iconLayers('handset', 'internet').map(l => l.ink), iconLayers('handset', 'internet_failed').map(l => l.ink)],
        [['glyph', 'amber', 'amber'], ['glyph', 'red', 'red']]);
}

// ============================================================================
// #49: the official apps of this computer
// ============================================================================
{
    // Signal Desktop and Discord installed, a browser; nothing for Telegram,
    // WhatsApp, Slack or Teams.
    const found = {
        apps: {'signal-desktop.desktop': 'Signal', 'com.discordapp.Discord.desktop': 'Discord'},
        handlers: {sgnl: 'signal-desktop.desktop', discord: 'com.discordapp.Discord.desktop', tg: null, https: 'browser.desktop'},
        web: true,
    };
    const c = linked();
    const settingsWritten = [];
    c.model.onSetting = (name, value) => settingsWritten.push([name, value]);
    c.model.appRoute = id => appRoute(id, found);
    let running = new Set();
    c.model.appRunning = id => running.has(id);
    const note = (uid, extra = {}) => ({command: 'bt_notification', uid, app_id: 'org.whispersystems.signal', app_name: 'Signal',
        title: '+41 79 000 00 07', subtitle: '', body: 'Salut', silent: false, negative_action: true, ...extra});
    const card = uid => {
        c.model.openNote(uid);
        const v = c.model.view().card;
        c.model.cardClosed();
        return v;
    };

    c.model.handleEvent(note(20));
    let v = card(20);
    check('Signal Desktop installed, sender sure: Open in Signal, on the conversation', v.handoff,
        {label: 'Open in Signal', open: {uri: 'sgnl://signal.me/#p/+41790000007'}, conversation: true, media: ''});
    check('...and nothing suggested: the app is here', v.suggestion, null);

    c.model.handleEvent(note(21, {title: 'Les voisins', subtitle: 'Camille Martin', body: '🎤 Voice message'}));
    v = card(21);
    check('a group: Signal itself, not a conversation; the voice message said where it plays', v.handoff,
        {label: 'Open in Signal', open: {app: 'signal-desktop.desktop'}, conversation: false,
            media: 'Voice message — play it in Signal'});

    c.model.handleEvent(note(22, {app_id: 'net.whatsapp.WhatsApp', app_name: 'WhatsApp', body: '📷 Photo'}));
    v = card(22);
    check('WhatsApp (no official Linux app): the official web client, on the conversation', v.handoff,
        {label: 'Open in WhatsApp Web', open: {uri: 'https://web.whatsapp.com/send?phone=41790000007'}, conversation: true,
            media: 'Photo — see it in WhatsApp Web'});

    c.model.handleEvent(note(23, {app_id: 'net.whatsapp.WhatsApp', app_name: 'WhatsApp', title: 'Les voisins', subtitle: 'Léo'}));
    check('WhatsApp group: the web client itself', card(23).handoff.open, {uri: 'https://web.whatsapp.com/'});

    c.model.handleEvent(note(24, {app_id: 'ph.telegra.Telegraph', app_name: 'Telegram', body: 'Message vocal'}));
    c.model.openNote(24);
    v = c.model.view().card;
    check('Telegram Desktop missing: Telegram Web, no conversation by number there', [v.handoff.label, v.handoff.open,
        v.handoff.media], ['Open in Telegram Web', {uri: 'https://web.telegram.org/'}, 'Voice message — play it in Telegram Web']);
    check('...and one quiet line suggests the official app', v.suggestion, {
        text: 'Telegram Desktop isn’t on this computer. The official app shows voice messages and media here.',
        get: 'Get Telegram Desktop', url: 'https://desktop.telegram.org/'});
    check('...remembered at once, for good', settingsWritten, [['suggested-apps', ['telegram']]]);
    c.model.cardClosed();
    c.model.handleEvent(note(25, {app_id: 'ph.telegra.Telegraph', app_name: 'Telegram'}));
    check('the next Telegram card: said once is enough', card(25).suggestion, null);
    c.model.setSuggestedApps([]);
    check('suggested apps as Phone Settings keeps them', card(25).suggestion?.get, 'Get Telegram Desktop');

    c.model.handleEvent(note(26, {app_id: 'com.hammerandchisel.discord', app_name: 'Discord', body: 'GIF'}));
    check('Discord installed: Discord itself (no conversation link for a number)', card(26).handoff,
        {label: 'Open in Discord', open: {app: 'com.discordapp.Discord.desktop'}, conversation: false,
            media: 'Sticker — see it in Discord'});
    c.model.handleEvent(note(27, {app_id: 'com.microsoft.skype.teams', app_name: 'Teams', body: 'Hello'}));
    v = card(27);
    check('Teams not installed: its web client; no app to suggest for Linux', [v.handoff.label, v.handoff.open, v.suggestion],
        ['Open Teams in the Browser', {uri: 'https://teams.microsoft.com/'}, null]);
    c.model.handleEvent(note(28, {app_id: 'com.apple.mobilemail', app_name: 'Mail'}));
    check('an app that is not a messaging service: nothing new', card(28).handoff, null);

    // Signal not installed at all.
    const bare = {apps: {}, handlers: {https: 'browser.desktop'}, web: true};
    c.model.appRoute = id => appRoute(id, bare);
    c.model.handleEvent(note(29, {body: '🎤 Voice message (0:12)'}));
    c.model.openNote(29);
    v = c.model.view().card;
    check('Signal missing, no web client: no button, the voice message is on the iPhone', [v.handoff.label, v.handoff.open,
        v.handoff.media], ['', null, 'Voice message — play it on the iPhone']);
    check('...Signal Desktop suggested', v.suggestion.get, 'Get Signal Desktop');
    c.model.stopSuggesting();
    check('Don’t Suggest Apps: the line goes, the choice is kept', [c.model.view().card.suggestion, settingsWritten.at(-1)],
        [null, ['suggest-apps', false]]);
    c.model.cardClosed();
    c.model.setSuggestedApps([]);
    check('...and nothing is suggested any more', card(29).suggestion, null);

    // A link handler that is not an official app is never used.
    const unofficial = {apps: {'org.kotatogram.desktop': 'Kotatogram'}, handlers: {tg: 'org.kotatogram.desktop', https: 'b.desktop'}, web: true};
    check('an unofficial Telegram client that opens tg: links: never offered', appRoute('ph.telegra.Telegraph', unofficial).via,
        'Telegram Web');

    // "Leave notifications to apps open on this computer".
    c.model.appRoute = id => appRoute(id, found);
    const pill = uid => {
        c.model.handleEvent(note(uid, {title: `Personne ${uid}`, body: `Texte ${uid}`}));
        const mode = c.model.view().mode;
        c.now += 60000;
        c.model.expire();
        return mode;
    };
    running = new Set(['org.whispersystems.signal']);
    check('option off: the pill grows even with Signal open', pill(40), 'note');
    c.model.skipRunningApps = true;
    check('option on, Signal open here: no pill', pill(41), 'idle');
    check('...still waiting in the menu', c.model.view().card.phone.groups.some(g => g.notes.some(n => n.uid === 41)), true);
    running = new Set();
    check('option on, Signal closed: the pill again', pill(42), 'note');
}

// ============================================================================
// #50: phone links (tel:, callto:) call through the iPhone
// ============================================================================
{
    const c = linked();
    c.model.handleEvent({command: 'bt_threads', threads: [
        {thread: 'tel:+41790000001', name: 'Camille Martin', address: '+41790000001', preview: 'Salut', timestamp: T0 / 1000 - 500},
    ]});
    c.sent.length = 0;
    c.model.phoneLink('tel:+41-79-000-00-01');
    let v = c.model.view();
    check('a tel: link: the card asks, with the contact\'s name, nothing dialled',
        [v.cardOpen, v.card.kind, v.card.name, v.card.subtitle, v.card.canCall, c.sent],
        [true, 'phonelink', 'Call Camille Martin?', '+41 79 000 00 01', true, []]);
    check('...nothing in the way', v.card.why, '');
    c.model.cardOpened();
    check('the menu opening keeps that card', c.model.view().card.kind, 'phonelink');
    c.sent.length = 0;
    c.model.confirmPhoneLink();
    const id = c.sent[0]?.operation_id;
    check('Call: the way of Call…', c.sent, [{command: 'bt_call_dial', number: '+41790000001', operation_id: id}]);
    check('...its card follows the call', [c.model.view().card.kind, c.model.view().card.name], ['dialing', 'Camille Martin']);
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, reason: 'refused', operation_id: id});
    c.model.cardClosed();
    c.model.cancelDial();

    c.sent.length = 0;
    c.model.phoneLink('tel:0790000009;phone-context=+41');
    v = c.model.view();
    check('a number nobody has: the number, local as written', [v.card.name, v.card.subtitle, v.card.number],
        ['Call 079 000 00 09?', 'Through your iPhone', '0790000009']);
    c.model.cancelPhoneLink();
    check('Cancel: closed, nothing sent', [c.model.view().cardOpen, c.sent], [false, []]);

    c.model.phoneLink('tel:*21*0790000009%23');
    v = c.model.view();
    check('a service code from a page: never called', [v.card.name, v.card.why, v.card.canCall],
        ['Phone link', 'This link has no phone number that can be called.', false]);
    c.model.confirmPhoneLink();
    check('...Call does nothing', c.sent, []);
    c.model.cardClosed();
    check('closing the menu forgets the link', c.model.view().card.kind, 'menu');

    c.model.handleEvent({command: 'bt_connection_changed', device_present: false, device_paired: true});
    c.model.phoneLink('callto:+41790000001');
    v = c.model.view();
    check('the iPhone away: said, nothing to call', [v.card.kind, v.card.why, v.card.canCall],
        ['phonelink', 'Your iPhone isn’t connected, so the call can’t be placed from here.', false]);
    c.model.cardClosed();

    // The sound choice, with an engine that can be told.
    const l = leanLinked();
    l.model.handleEvent(audioState());
    l.model.phoneLink('tel:+41 79 000 00 01');
    check('light mode: the sound choice, this computer first', l.model.view().card.audio, 'computer');
    l.model.setPhoneLinkAudio('phone');
    l.model.confirmPhoneLink();
    check('...sent with the call', [l.sent.at(-1).command, l.sent.at(-1).audio], ['bt_call_dial', 'phone']);

    // A message pill open when the link comes: the link wins.
    const m = linked();
    m.model.handleEvent(message());
    m.model.phoneLink('tel:+41790000002');
    m.model.cardOpened();
    check('a message in the pill does not take the card of the link', m.model.view().card.kind, 'phonelink');
}

// ============================================================================
// #52: the card turns 'direct' while the menu is open
// ============================================================================
{
    const c = linked();
    c.model.cardOpened();
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, device_paired: false}));
    const v = c.model.view();
    check('menu open, the iPhone forgets the pairing: still open, the card is direct (the indicator closes it)',
        [v.cardOpen, v.card], [true, {kind: 'direct', action: 'link'}]);
}

// ============================================================================
// #56: phone links, rough edges
// ============================================================================
{
    // A link during a call: its card shows over the call card and says why.
    const c = linked();
    c.model.handleEvent(call('active'));
    c.model.phoneLink('tel:+41790000002');
    let v = c.model.view();
    check('a link during a call: its card, not hidden under the call card',
        [v.mode, v.card.kind, v.card.why, v.card.canCall], ['call', 'phonelink', 'You’re already on a call.', false]);
    c.model.cancelPhoneLink();
    check('...Close: the call card again, the link gone', [c.model.view().card.kind, c.model.view().cardOpen], ['call', false]);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.cardOpened();
    check('...after the call: the menu, never the link', c.model.view().card.kind, 'menu');
    c.model.cardClosed();

    // A link, then a call rings: the call takes over, the link never comes back.
    c.model.phoneLink('tel:+41790000002');
    c.model.cardOpened();
    c.model.handleEvent(call('incoming'));
    check('a call rings over the link: the ring card', c.model.view().card.kind, 'ring');
    c.model.handleEvent(call('active'));
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.cardOpened();
    check('...the call over: no link card coming back', c.model.view().card.kind, 'menu');
    c.model.cardClosed();

    // A link while a call is being placed from here: said, not hidden.
    c.model.startDial();
    c.model.dial('+41790000001', 'computer');
    c.model.phoneLink('tel:+41790000002');
    v = c.model.view();
    check('a link while a call is being placed: its card says so', [v.card.kind, v.card.why],
        ['phonelink', 'Another call is being placed.']);
    c.model.cancelPhoneLink();
    check('...Close: the call on its way again', c.model.view().card.kind, 'dialing');
    c.model.cardClosed();
    c.model.handleEvent({command: 'bt_call_result', action: 'dial', success: false, reason: 'refused',
        operation_id: c.sent.filter(x => x.command === 'bt_call_dial').at(-1).operation_id});
    c.model.cancelDial();

    // "Call" refused by dial(): the card stays and says so.
    const d = linked();
    d.model.phoneLink('tel:+41790000002');
    d.model.dial = () => false;
    check('Call refused: false', d.model.confirmPhoneLink(), false);
    v = d.model.view();
    check('...the card stays, says why, nothing more to click but Close',
        [v.cardOpen, v.card.kind, v.card.why, v.card.canCall], [true, 'phonelink', 'The call could not be placed.', false]);
}

// ============================================================================
// #50: "Use Landline for phone links" offered once, in the menu
// ============================================================================
{
    const c = linked();
    const settings = [];
    c.model.onSetting = (name, value) => settings.push([name, value]);
    c.model.setPhoneLinksOffer(true);
    check('no other app opens tel:: nothing yet, the menu is closed', [c.model.view().card.phoneLinksOffer, settings], [false, []]);
    c.model.cardOpened();
    check('the menu opens: the offer, remembered at once', [c.model.view().card.phoneLinksOffer, settings],
        [true, [['phone-links-offered', true]]]);
    c.model.answerPhoneLinksOffer();
    check('an answer: gone', c.model.view().card.phoneLinksOffer, false);
    c.model.cardClosed();
    c.model.cardOpened();
    check('never again', [c.model.view().card.phoneLinksOffer, settings.length], [false, 1]);
    c.model.cardClosed();
    c.model.setPhoneLinksOffer(true);
    c.model.cardOpened();
    check('...even told again (a late look of the extension)', c.model.view().card.phoneLinksOffer, false);
    c.model.cardClosed();

    const u = linked();
    u.model.setPhoneLinksOffer(true);
    u.model.cardOpened();
    u.model.cardClosed();
    u.model.cardOpened();
    check('not answered: still shown once only', u.model.view().card.phoneLinksOffer, false);

    const a = setup();
    a.model.setEngineState('connected');
    a.model.handleEvent(status());
    a.model.handleEvent(conn({classic_connected: false, le_connected: false}));
    a.model.setPhoneLinksOffer(true);
    a.model.cardOpened();
    check('the iPhone away (no call possible): not offered, kept for later', a.model.view().card.phoneLinksOffer, false);
    a.model.cardClosed();
    a.model.handleEvent(conn());
    a.model.cardOpened();
    check('...linked again: offered then', a.model.view().card.phoneLinksOffer, true);

    const o = linked();
    o.model.setPhoneLinksOffer(false);
    o.model.cardOpened();
    check('another app opens tel: (the extension says no): never offered', o.model.view().card.phoneLinksOffer, false);
}

print(`\n${count - failed}/${count} checks passed`);
if (failed > 0)
    imports.system.exit(1);
