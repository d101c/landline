// SPDX-License-Identifier: GPL-2.0-or-later
// Règles du modèle d'état, sans Shell : gjs -m tests/model.test.js
import {
    Model, initials, firstName, formatDuration, formatNumber, sender,
    MESSAGE_PILL_MS, SENT_PILL_MS, SEND_TIMEOUT_MS, TOUR_MIN_STEP_MS, TOUR_VERIFY_MS,
    OBEX_STUCK_MS, FIX_TIMEOUT_MS, RETRY_PROFILES_MS,
    LINK_SETTLE_MS, FLAP_WINDOW_MS, STEADY_MS, AUDIO_MOVE_MS, AUDIO_ANSWER_MS,
} from '../extension/lib/model.js';
import {phoneLanguage, bilingual, PHONE_LABELS} from '../extension/lib/phoneLabels.js';
import {replyLink, replySender, contactNumber} from '../extension/lib/replyLinks.js';

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
    check('engine absent: said in words', [v.link, v.pill.strong, v.pill.dot], ['engine', 'Engine not running', 'warn']);
    check('engine absent: card explains', [v.card.kind, v.card.problem, v.card.action.id], ['problem', 'engine', 'retry']);
    c.model.setEngineState('connected');
    c.model.handleEvent(status());
    c.model.handleEvent(conn());
    v = c.model.view();
    check('linked: quiet glyph and full dot', v.pill, {tone: 'quiet', icon: 'phone', dot: 'on'});
    check('linked: menu header', [v.card.kind, v.card.statusLine, v.card.battery], ['menu', 'Linked · Swisscom', 96]);
    check('linked: the first line has the name, one word and the battery; nothing under it',
        [v.card.name, v.card.state, v.card.notices], ['iPhone', 'Linked', []]);
    check('the menu never offers to forget the iPhone', 'forgetAction' in v.card, false);
    c.model.handleEvent(conn({classic_connected: false, le_connected: false, battery: null}));
    v = c.model.view();
    check('out of range: hollow dot, dimmed', v.pill, {tone: 'quiet', dim: true, icon: 'phone', dot: 'off'});
    check('out of range: menu says so', [v.card.statusLine, v.card.battery], ['Out of range', null]);
    check('out of range: the same word beside the name, no battery, nothing under', [v.card.state, v.card.notices], ['Out of range', []]);
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
    check('...which is then a notice in the menu', c.model.view().card.notices.length, 1);
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
    check('ignored: back to rest with a count', v.pill, {tone: 'quiet', icon: 'phone', count: 1});
    check('ignored: nothing left to wait for', c.model.nextDeadline(), null);
    c.model.handleEvent(message());
    check('same message twice: counted once', c.model.view().pill.count, 1);
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
    check('sent: confirmation', c.model.view().pill, {tone: 'neutral', icon: 'check', strong: 'Sent'});
    c.now += SENT_PILL_MS;
    c.model.expire();
    check('then back to the dot', c.model.view().pill, {tone: 'quiet', icon: 'phone', dot: 'on'});
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
    check('incoming call: pill', v.pill, {tone: 'accent', icon: 'call', strong: 'Camille is calling'});
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
    check('on a call: green pill with timer', v.pill, {tone: 'call', icon: 'call', strong: 'Camille · 00:00'});
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
    check('call over: rest, card closed, missed message counted', [v.mode, v.cardOpen, v.pill.count], ['idle', false, 1]);

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
    check('battery low: said once', c.model.view().pill, {tone: 'accent', icon: 'phone', strong: 'iPhone 12 %'});
    c.now += 7000;
    c.model.expire();
    c.model.handleEvent(conn({battery: 11}));
    check('battery still low: not repeated', c.model.view().mode, 'idle');
    check('battery in the menu', c.model.view().card.battery, 11);
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
    check('dismissed here: cleared there', c.sent, [{command: 'bt_notification_action', uid: 7, action: 'negative'}]);
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
        {tone: 'accent', strong: '+41 76 555 01 42', text: 'Bonjour', icon: 'person'});
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
    check('links up and a service open: linked at once', c.model.view().pill, {tone: 'quiet', icon: 'phone', dot: 'on'});
    check('...nothing left to wait for', c.model.nextDeadline(), null);

    at(c, 10000);
    c.model.handleEvent(down());
    check('links drop: still shown linked', [c.model.view().link, c.model.view().pill.dot], ['linked', 'on']);
    tick(c, 10000 + LINK_SETTLE_MS - 1);
    check('...until the drop has held 3 s', c.model.view().link, 'linked');
    tick(c, 10000 + LINK_SETTLE_MS);
    check('held 3 s: out of range', [c.model.view().link, c.model.view().pill.dot], ['away', 'off']);

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
    check('links that hold 3 s are a link, service or not', c.model.view().link, 'linked');

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
        c.model.handleEvent(i % 2 ? up() : down());
        c.model.expire();
        seen.add(c.model.view().pill.strong);
        ms += 4000;
    }
    check('whatever the links do next, it stays there', [...seen], ['linked', 'Link again']);
    // Links are up since the last event (ms - 4000).
    tick(c, ms - 4000 + STEADY_MS - 1);
    check('up for less than 60 s: still Link again', c.model.view().link, 'broken');
    tick(c, ms - 4000 + STEADY_MS);
    tick(c, ms - 4000 + STEADY_MS + LINK_SETTLE_MS);
    check('up for 60 s without a drop: linked again', [c.model.view().link, c.model.view().unsteady], ['linked', null]);

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
        ['idle', {tone: 'quiet', icon: 'phone', count: 1}, 'dnd']);
    check('...nothing is waiting to pop up later', c.model.nextDeadline(), null);
    check('...the menu says why', v.card.notices, ['Do Not Disturb is on: nothing pops up here.']);
    check('...and still lists the conversation', v.card.threads.map(t => [t.name, t.unread]), [['Camille Martin', true]]);
    c.model.handleEvent(call('incoming'));
    v = c.model.view();
    check('Do Not Disturb, a call: the bar does not move', [v.mode, v.pill], ['idle', {tone: 'quiet', icon: 'phone', count: 1}]);
    check('...a menu opened on purpose can still answer', v.card.kind, 'ring');
    c.model.handleEvent(call('active'));
    v = c.model.view();
    check('answered on the iPhone meanwhile: no name in the bar either', [v.mode, v.pill.strong, v.card.kind], ['idle', undefined, 'call']);
    c.model.handleEvent({command: 'bt_calls', calls: []});
    c.model.handleEvent(conn({battery: 9}));
    check('Do Not Disturb: no battery warning', c.model.view().pill, {tone: 'quiet', icon: 'phone', count: 1});
    c.model.setQuiet({dnd: false});
    v = c.model.view();
    check('Do Not Disturb off: nothing is played again, the count is enough', [v.mode, v.pill, v.quiet],
        ['idle', {tone: 'quiet', icon: 'phone', count: 1}, null]);
    c.model.handleEvent(message({handle: 'H2', body: 'Et maintenant ?'}));
    check('...and the next message shows as usual', c.model.view().pill.text, 'Et maintenant ?');

    // A pill already on screen when silence begins.
    c = linked();
    c.model.handleEvent(message());
    c.model.setQuiet({sharing: true});
    v = c.model.view();
    check('screen shared while a message pill is up: it goes, counted', [v.pill, v.quiet],
        [{tone: 'quiet', icon: 'phone', count: 1}, 'sharing']);
    c.model.setQuiet({sharing: false});
    check('...and does not come back when the sharing stops', c.model.view().pill.count, 1);

    // Calls through Do Not Disturb: a choice, and only for Do Not Disturb.
    c = linked();
    c.model.setQuiet({dnd: true, callsThroughDnd: true});
    c.model.handleEvent(call('incoming'));
    check('calls let through Do Not Disturb: the call shows', c.model.view().mode, 'ring');
    c.model.handleEvent(message());
    c.model.handleEvent({command: 'bt_calls', calls: []});
    check('...messages still do not', c.model.view().pill, {tone: 'quiet', icon: 'phone', count: 1});
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
        ['idle', {tone: 'quiet', icon: 'phone', count: 1}, 'muted', true]);
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
    check('a message that arrived before the Shell came back: count only', c.model.view().pill, {tone: 'quiet', icon: 'phone', count: 1});
}

// --- daily life: sound, battery, presence (docs/moteur-quotidien.md)
{
    const audio = (extra = {}) => ({command: 'bt_audio', phone_audio: 'phone', phone_audio_route: 'phone',
        call_audio: 'phone', call_audio_route: 'phone', call_audio_switchable: true, call_audio_reject: true, ...extra});
    const told = [];
    const fresh = () => {
        const c = linked();
        c.settings = [];
        c.alerts = [];
        c.model.onSetting = (name, value) => c.settings.push([name, value]);
        c.model.onAlert = a => c.alerts.push(a);
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

    // Battery: one quiet notification per crossing.
    c = fresh();
    const low = (extra = {}) => ({command: 'bt_battery_low', level: 20, threshold: 20, state: 'low', source: 'le', initial: false, ...extra});
    c.model.handleEvent(conn({battery: 20, battery_state: 'low'}));
    c.model.handleEvent(low());
    check('bt_battery_low: one alert', c.alerts, [{kind: 'battery', level: 20, threshold: 20}]);
    c.model.handleEvent(low());
    c.model.handleEvent(low({level: 19}));
    check('told twice: said once', c.alerts.length, 1);
    c.model.handleEvent(conn({battery: 12, battery_state: 'low'}));
    check('an engine that announces: no pill at 12 %', c.model.view().pill, {tone: 'quiet', icon: 'phone', dot: 'on'});
    c.model.handleEvent(low({level: 10, threshold: 10, state: 'critical'}));
    check('10 %: a second alert', c.alerts.map(a => a.threshold), [20, 10]);
    c.model.handleEvent(conn({battery: 30, battery_state: 'ok'}));
    c.model.handleEvent(low());
    check('recharged past 25 %, low again: told again', c.alerts.length, 3);
    c = fresh();
    c.model.handleEvent(low({level: 8, threshold: 10, state: 'critical', initial: true}));
    c.model.handleEvent(low({level: 8, threshold: 20}));
    check('already low when the engine started: told once, for the lower mark', c.alerts.map(a => a.threshold), [10]);
    c = fresh();
    c.model.batteryAlerts = false;
    c.model.handleEvent(low());
    check('alerts switched off in Phone Settings: nothing', c.alerts, []);
    c = fresh();
    c.model.setQuiet({dnd: true});
    c.model.handleEvent(low());
    check('Do Not Disturb: no battery alert', c.alerts, []);
    c = fresh();
    c.model.handleEvent(conn({battery: 12}));
    check('older engine (no battery_state): the pill still warns', c.model.view().pill.strong, 'iPhone 12 %');

    // Presence: why it is not linked, and the lock.
    c = fresh();
    const gone = conn({classic_connected: false, le_connected: false, battery: null});
    const presence = (extra = {}) => ({command: 'bt_presence', state: 'away', reason: 'out_of_range',
        away_seconds: 3, lock: 'off', lock_in_seconds: 0, wake: false, ...extra});
    c.model.handleEvent(gone);
    check('no bt_presence: Out of range', c.model.view().card.statusLine, 'Out of range');
    c.model.handleEvent(presence({state: 'here', reason: '', lock: 'none'}));
    check('links down but the engine still reaches it: Reconnecting…', c.model.view().card.statusLine, 'Reconnecting…');
    c.model.handleEvent(presence());
    check('away, out of range', c.model.view().card.statusLine, 'Out of range');
    c.model.handleEvent(presence({reason: 'switched_off', lock: 'spared'}));
    check('Bluetooth cut on the iPhone: said so', c.model.view().card.statusLine, 'Bluetooth is off on the iPhone');
    check('...two words beside the name, the sentence under it',
        [c.model.view().card.state, c.model.view().card.notices], ['Bluetooth off', ['Bluetooth is off on the iPhone.']]);
    c.model.handleEvent(presence({lock: 'pending', lock_in_seconds: 27}));
    check('about to lock', c.model.view().card.statusLine, 'Out of range · this computer locks shortly');
    check('...beside the name: Out of range; under it: the lock',
        [c.model.view().card.state, c.model.view().card.notices], ['Out of range', ['This computer locks shortly.']]);
    c.model.handleEvent(presence({lock: 'locked'}));
    check('locked', c.model.view().card.statusLine, 'Out of range · this computer was locked');
    check('...under the name: locked when the iPhone left',
        [c.model.view().card.state, c.model.view().card.notices], ['Out of range', ['This computer was locked when the iPhone left.']]);
    c.model.handleEvent(presence({reason: 'computer_suspended', lock: 'spared'}));
    check('back from sleep: Reconnecting… beside the name',
        [c.model.view().card.state, c.model.view().card.notices], ['Reconnecting…', ['Reconnecting after sleep…']]);
    c.model.handleEvent(presence({state: 'here', reason: '', lock: 'none'}));
    check('reconnecting: one word, nothing under', [c.model.view().card.state, c.model.view().card.notices], ['Reconnecting…', []]);

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

print(`\n${count - failed}/${count} checks passed`);
if (failed > 0)
    imports.system.exit(1);
