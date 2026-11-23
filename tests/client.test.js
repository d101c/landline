// SPDX-License-Identifier: GPL-2.0-or-later
// Client du moteur contre tools/faux-moteur.py : gjs -m tests/client.test.js
// Le socket est créé dans un dossier temporaire ; le vrai moteur n'est jamais
// contacté.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {EngineClient, defaultSocketPath} from '../extension/lib/engineClient.js';
import {Model, STARTUP_COMMANDS} from '../extension/lib/model.js';

const loop = new GLib.MainLoop(null, false);
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
// Commands not answered yet: queued, plus the one in flight.
const pending = client => client._queue.length + (client._waiting ? 1 : 0);

async function until(test, ms = 5000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (test())
            return true;
        await sleep(20);
    }
    return false;
}

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const dir = GLib.dir_make_tmp('landline-test-XXXXXX');
const socket = `${dir}/faux.sock`;

const fakes = [];
class Fake {
    constructor(args = []) {
        fakes.push(this);
        this.lines = [];
        this.proc = Gio.Subprocess.new(
            ['python3', `${here}/../tools/faux-moteur.py`, '--socket', socket, ...args],
            Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE);
        this.stdin = this.proc.get_stdin_pipe();
        this.out = new Gio.DataInputStream({base_stream: this.proc.get_stdout_pipe()});
        this._read();
    }

    _read() {
        this.out.read_line_async(GLib.PRIORITY_DEFAULT, null, (s, res) => {
            const [bytes] = s.read_line_finish(res);
            if (bytes === null)
                return;
            this.lines.push(new TextDecoder().decode(bytes));
            this._read();
        });
    }

    received() {
        return this.lines.filter(l => l.startsWith('RECU ')).map(l => l.slice(5));
    }

    receivedCommands() {
        return this.received().map(l => {
            try {
                return JSON.parse(l).command;
            } catch (e) {
                return l;
            }
        });
    }

    say(text) {
        this.stdin.write_all(`${text}\n`, null);
        this.stdin.flush(null);
    }

    async ready() {
        return until(() => this.lines.some(l => l.startsWith('PRET')));
    }

    stop() {
        this.proc.force_exit();
    }
}

async function main() {
    check('default socket path is the engine one',
        defaultSocketPath().endsWith('/landline/landlined.sock'), true);

    const events = [];
    const states = [];
    const sent = [];
    let client = null;
    // The clock of the model can be pushed forward, to skip a wait.
    let skew = 0;
    const model = new Model({send: c => {
        sent.push(c);
        client.send(c);
    }, now: () => Date.now() + skew, callControl: true, steady: false});
    client = new EngineClient({
        path: socket,
        hello: STARTUP_COMMANDS,
        retryDelays: [150, 300],
        ackTimeout: 600,
        onEvent: e => {
            events.push(e);
            model.handleEvent(e);
        },
        onState: s => {
            states.push(s);
            model.setEngineState(s);
        },
    });
    const names = () => events.map(e => e.command);

    // --- no engine yet
    client.start();
    await until(() => states.includes('absent'));
    check('no socket: reported absent', states, ['absent']);
    check('no socket: the model says so', model.view().pill.strong, 'Engine not running');
    check('nothing is queued while absent', [client.send({command: 'bt_status'}), pending(client)], [false, 0]);

    // --- engine appears: automatic reconnection and start-up sequence
    let fake = new Fake();
    await fake.ready();
    await until(() => names().includes('bt_call_history'));
    check('reconnects by itself', states, ['absent', 'connected']);
    check('start-up commands arrive in order, one after the other',
        fake.receivedCommands().slice(0, STARTUP_COMMANDS.length), STARTUP_COMMANDS.map(c => c.command));
    check('replies are told apart by name',
        ['protocol_info', 'bt_status', 'bt_devices', 'bt_notifications', 'bt_calls', 'bt_threads', 'bt_call_history']
            .every(n => names().includes(n)), true);
    check('queue is empty afterwards', pending(client), 0);
    // The engine starts with call control off: the model turns it on.
    await until(() => model.view().card.statusLine?.includes('Swisscom'));
    check('call control turned on once linked', sent.filter(c => c.command === 'bt_set_calls'),
        [{command: 'bt_set_calls', enabled: true}]);
    let v = model.view();
    check('model: linked, nothing said of the link in the menu', [v.link, v.card.name, v.card.notices, v.card.battery],
        ['linked', 'iPhone de Camille', [], 96]);
    check('model: conversations read', v.card.threads.map(t => t.name), ['Léo', 'Camille Martin']);

    // --- an incoming message, then a reply (OK acknowledgement, then events)
    fake.say('message');
    await until(() => model.view().mode === 'message');
    check('message event reaches the model', model.view().pill.strong, 'Camille');
    model.cardOpened();
    model.reply('Oui, parfait');
    await until(() => model.view().mode === 'sent');
    check('reply goes out and is confirmed', model.view().pill.strong, 'Sent');
    await until(() => names().filter(n => n === 'bt_message').length >= 2);
    check('bare OK lines are not events', events.some(e => e === 'OK' || e.command === undefined), false);
    const got = fake.received().map(l => JSON.parse(l));
    check('engine got mark-read then the message',
        got.slice(-2).map(c => [c.command, c.body ?? c.handles.length]),
        [['bt_mark_read', 1], ['bt_send_message', 'Oui, parfait']]);
    check('queue is empty after OK replies', pending(client), 0);

    // --- several commands at once: sent one by one, none lost
    const before = fake.received().length;
    client.send({command: 'bt_hold', profile: 'ancs', held: false});
    client.send({command: 'bt_list_threads'});
    client.send({command: 'not json to the engine'});
    client.send({command: 'bt_list_calls'});
    check('commands wait their turn', pending(client), 4);
    await until(() => pending(client) === 0);
    check('all sent, in order', fake.receivedCommands().slice(before),
        ['bt_hold', 'bt_list_threads', 'not json to the engine', 'bt_list_calls']);

    // --- refused send
    fake.say('refuse-envoi');
    fake.say('message Encore là ?');
    await until(() => model.view().mode === 'message');
    model.cardOpened();
    model.reply('Oui');
    await until(() => model.view().mode === 'notice');
    check('refused reply is reported', model.view().pill.strong, 'Not sent');
    model.cardOpened();
    check('...with the engine reason', model.view().card.subtitle, 'The iPhone refused the message.');
    model.cardClosed();

    // --- call: ring, answer from here, sound, hang up
    fake.say('appel');
    await until(() => model.view().mode === 'ring');
    await until(() => model.view().card.name === 'Camille Martin');
    check('call rings, name read from the list', [model.view().pill.strong, model.view().card.name],
        ['Camille is calling', 'Camille Martin']);
    check('the engine says where the sound of calls can go: two ways to answer',
        [events.some(e => e.command === 'bt_audio'), model.view().card.answerChoice], [true, true]);
    model.answer('phone');
    await until(() => model.view().mode === 'call');
    check('answered from here, sound on the iPhone', [model.view().pill.tone, model.view().card.audio], ['call', 'phone']);
    check('...the answer said where the sound goes', sent.filter(c => c.action === 'answer').map(c => c.audio), ['phone']);
    model.setCallAudio('computer');
    await until(() => events.some(e => e.command === 'bt_connection_changed' && e.calls?.audio_route === 'here'));
    check('sound moved here, for this call', [model.view().card.audio,
        sent.filter(c => c.command === 'bt_set_call_audio').map(c => [c.route, c.persist])], ['computer', [['here', false]]]);
    check('...and no longer shown as on its way', [model.view().card.moving, model.view().card.isError], [false, false]);
    // Back to the iPhone during the call: the engine says it did not happen.
    fake.say('son-refuse');
    model.setCallAudio('phone');
    check('back to the iPhone: on its way', [model.view().card.audio, model.view().card.moving], ['phone', true]);
    await until(() => events.some(e => e.command === 'bt_call_audio_result' && e.success === false));
    check('the engine could not close the voice link: said, and the sound shown where it is',
        [model.view().card.audio, model.view().card.moving, model.view().card.isError, model.view().card.subtitle],
        ['computer', false, true, 'Couldn’t move the call to the iPhone']);
    // The same, with an engine that says nothing: the delay decides.
    fake.say('son-coince');
    model.setCallAudio('phone');
    await until(() => events.filter(e => e.command === 'bt_call_audio_result').length >= 3);
    check('an engine that claims it worked: still shown as on its way', model.view().card.moving, true);
    skew += 3000;
    model.expire();
    check('...3 s later the sound has not moved: said',
        [model.view().card.audio, model.view().card.isError, model.view().card.subtitle],
        ['computer', true, 'Couldn’t move the call to the iPhone']);
    skew -= 3000;
    // And when it works.
    fake.say('son-libre');
    model.setCallAudio('phone');
    await until(() => events.some(e => e.command === 'bt_connection_changed' && e.calls?.audio_route === 'phone' && e.calls?.calls !== 0)
        && model.view().card.moving === false);
    check('back to the iPhone, for good: shown from what the engine says',
        [model.view().card.audio, model.view().card.isError], ['phone', false]);
    model.hangUp();
    await until(() => model.view().mode !== 'call');
    check('hung up', model.view().mode, 'idle');
    check('call results arrive twice and do no harm',
        names().filter(n => n === 'bt_call_result').length, 4);

    // --- sound of the iPhone, presence
    check('start-up: the audio state was read', model.view().card.phoneAudioHere, false);
    model.setPhoneAudio(true);
    await until(() => events.some(e => e.command === 'bt_audio' && e.phone_audio === 'here'));
    check('Play iPhone audio on this computer: the engine took it', model.view().card.phoneAudioHere, true);
    model.setPhoneAudio(false);
    await until(() => events.some(e => e.command === 'bt_phone_audio_result' && e.route === 'phone'));
    fake.say('eloigne switched_off');
    await until(() => model.view().link === 'away');
    check('away because Bluetooth was cut on the iPhone: said so', model.view().card.notices, ['Bluetooth is off on the iPhone']);
    fake.say('relie');
    await until(() => model.view().link === 'linked');

    // --- states
    fake.say('hors-portee');
    await until(() => model.view().link === 'away');
    check('out of range', model.view().icon.state, 'away');
    fake.say('permission');
    await until(() => model.view().link === 'permission');
    check('permission needed', [model.view().pill.strong, model.view().card.steps.length], ['Permission needed', 2]);
    fake.say('permission-ok');
    fake.say('a-relier');
    await until(() => model.view().link === 'relink');
    check('link again', model.view().pill.strong, 'Link again');
    fake.say('relie');
    await until(() => model.view().link === 'linked');
    fake.say('batterie-faible');
    await until(() => model.view().icon.state === 'battery');
    check('battery low: the icon and the menu (drawn red), no pill',
        [model.view().icon.state, model.view().card.batteryLow, model.view().mode], ['battery', true, 'idle']);

    fake.say('batterie 96');

    // --- pairing confirmation
    fake.say('appairage');
    await until(() => model.view().mode === 'pair');
    check('pairing code shown', model.view().card.code, '703405');
    model.confirmPairing(true);
    await until(() => names().includes('bt_pair_result'));
    check('pairing confirmed by the engine', events.find(e => e.command === 'bt_pair_result').success, true);

    // --- the setup tour, driven by the engine events
    const ticker = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => {
        if (model.nextDeadline() !== null && model.nextDeadline() <= Date.now())
            model.expire();
        return GLib.SOURCE_CONTINUE;
    });
    const tour = () => model.view().tour;
    fake.say('sans-iphone');
    await until(() => model.view().link === 'setup');
    model.startTour();
    await until(() => tour()?.step === 'code', 8000);
    check('tour: found, called, number shown', [tour().code, tour().deviceName], ['703405', 'iPhone de Camille']);
    model.confirmPairing(true);
    await until(() => tour()?.step === 'pairing');
    check('tour: a waiting step is shown after the number', tour().step, 'pairing');
    await until(() => tour()?.step === 'switches');
    check('tour: links up, switches still off', [tour().links, tour().switches],
        [{classic: true, le: true}, {notifications: false, messages: false, contacts: false}]);
    fake.say('interrupteur notifications');
    fake.say('interrupteur messages');
    fake.say('interrupteur contacts');
    await until(() => tour()?.step === 'try');
    fake.say('message Essai');
    await until(() => tour()?.proof !== null);
    check('tour: the message sent as a try is shown', tour().proof, {name: 'Camille Martin', body: 'Essai'});
    model.tourNext();
    await until(() => tour()?.contacts > 0);
    check('tour: done, real counts', [tour().step, tour().contacts, tour().calls], ['done', 214, true]);
    model.cancelTour();

    // --- a pairing that exists but does not work, then the repair
    fake.say('sans-iphone');
    fake.say('appairage-partiel');
    await until(() => model.view().link === 'broken' && model.view().mode === 'idle', 9000);
    check('partial pairing: Link again, never "linked"', model.view().pill.strong, 'Link again');
    model.startTour();
    check('tour: starts with the repair, nothing asked of the engine yet',
        [tour().step, fake.receivedCommands().includes('bt_unpair')], ['repair', false]);
    model.tourRepairDone();
    await until(() => tour()?.step === 'code', 8000);
    check('repair: forgotten here, then a real pairing with its number',
        [fake.receivedCommands().includes('bt_unpair'), tour().code], [true, '703405']);
    model.confirmPairing(true);
    await until(() => tour()?.step === 'switches');
    check('repair: links up this time', tour().links, {classic: true, le: true});
    model.cancelTour();

    fake.say('echecs-authentification');
    await until(() => model.view().link === 'broken');
    await sleep(2500);
    check('authentication failures in a loop: stays "Link again", never flips to linked',
        [model.view().link, model.view().pill.strong], ['broken', 'Link again']);
    fake.say('relie');
    await until(() => model.view().link === 'linked');

    // --- the same with an engine from before the pairing contract
    fake.say('contrat-ancien');
    fake.say('sans-iphone');
    fake.say('appairage-partiel');
    await until(() => model.view().link === 'broken');
    check('older engine, partial pairing: found out without its help', model.view().pill.strong, 'Link again');
    model.startTour();
    model.tourRepairDone();
    await until(() => tour()?.step === 'code', 8000);
    model.confirmPairing(true);
    await until(() => tour()?.step === 'switches');
    check('older engine: repaired and really linked', [tour().links, model.view().link], [{classic: true, le: true}, 'linked']);
    model.cancelTour();
    fake.say('contrat-nouveau');
    fake.say('relie');
    await until(() => model.view().link === 'linked');
    GLib.source_remove(ticker);

    // --- the system OBEX service out of reach: told, engine restarted, back
    fake.say('obex-injoignable');
    await until(() => events.at(-1)?.map_error === 'other');
    let last = events.findLast(e => e.command === 'bt_connection_changed');
    check('fake engine: links up, messages and contacts failing with "other"',
        [last.classic_connected, last.le_connected, last.map_open, last.pbap_open, last.map_error, last.pbap_error],
        [true, true, false, false, 'other', 'other']);
    check('not told at once', model.view().card.fixAction, false);
    skew += 20000;
    model.expire();
    check('told after 20 s, with Fix It', [model.view().link, model.view().card.fixAction], ['linked', true]);
    states.length = 0;
    model.fixStarted();
    fake.say('relance');
    await until(() => states.includes('absent'));
    check('restart: every client is dropped, and the model expected it', [states[0], model.view().link], ['absent', 'starting']);
    await until(() => states.includes('connected') && model.view().link === 'linked' && !model.fixing, 8000);
    last = events.findLast(e => e.command === 'bt_connection_changed');
    check('engine back: services open again, nothing left to say',
        [last.map_open, last.map_error, model.view().card.fixAction, model.view().card.notices],
        [true, 'none', false, []]);
    fake.say('obex-tenace');
    await until(() => events.at(-1)?.map_error === 'other');
    skew += 20000;
    model.expire();
    states.length = 0;
    model.fixStarted();
    fake.say('relance');
    await until(() => states.includes('absent'));
    await until(() => states.includes('connected') && model.view().link === 'linked', 8000);
    skew += 20000;
    model.expire();
    check('a fault that a restart does not cure: forget and link again, no second Fix It',
        [model.view().card.fixAction, model.view().card.notices],
        [false, ['Restarting did not bring messages and contacts back. Forget this iPhone in Phone Settings, then link it again.']]);
    fake.say('obex-retabli');
    await until(() => model.view().card.notices?.length === 0);

    // --- forget the iPhone from here
    fake.say('refuse-oubli');
    model.startForget();
    model.forgetConfirm();
    await until(() => model.view().tour?.forget?.state === 'error');
    check('BlueZ refuses: said, still linked', [model.view().tour?.forget?.state, model.view().link], ['error', 'linked']);
    fake.say('accepte-oubli');
    await sleep(100);
    model.forgetConfirm();
    await until(() => model.view().tour?.forget?.state === 'phone' && model.view().link === 'setup');
    last = events.findLast(e => e.command === 'bt_unpair_result');
    check('forgotten: removed from the computer, selection cleared',
        [last.success, last.status, last.removed, last.selection_cleared, model.view().link],
        [true, 'unpaired', true, true, 'setup']);
    model.cancelTour();
    check('afterwards: Link iPhone', [model.view().pill.strong, model.view().tour], ['Link iPhone', null]);
    fake.say('relie');
    await until(() => model.view().link === 'linked');

    // --- notification of another app, cleared from here
    const shown = [];
    model.onNotification = (kind, n) => shown.push([kind, n.uid ?? n.uids]);
    fake.say('notification');
    await until(() => shown.length > 0);
    check('notification shown', shown[0][0], 'show');
    model.clearNotification(shown[0][1]);
    await until(() => shown.some(([k]) => k === 'remove'));
    check('clear is relayed and confirmed', shown.at(-1), ['remove', shown[0][1]]);

    // --- the classic link on demand, against an engine that knows it
    const lastConn = () => events.findLast(e => e.command === 'bt_connection_changed') ?? {};
    const leases = () => fake.received().filter(l => l.includes('"bt_lease"')).map(l => JSON.parse(l));
    const CAMILLE = 'tel:+41790000001';
    for (const delay of ['ouverture 0.4', 'mains-libres 0.5', 'maintien 0.5', 'composition 0.2'])
        fake.say(`delai ${delay}`);
    fake.say('sobre');
    client.send({command: 'bt_list_threads'});
    await until(() => model.view().card.notices?.includes('Messages and calls connect when needed.') &&
        model.view().card.threads.length > 0);
    check('light mode: linked on the notification link, classic link closed, everything on demand',
        [model.view().link, lastConn().classic_connected, lastConn().messages, lastConn().calls_available,
            model.view().card.composeAction, model.view().card.callAction],
        ['linked', false, 'on_demand', 'on_demand', true, true]);

    const pills = [];
    model.onChange = () => {
        const strong = model.view().pill.strong ?? '';
        if (pills.at(-1) !== strong)
            pills.push(strong);
    };
    model.cardOpened();
    model.openThread(CAMILLE);
    pills.length = 0;
    check('reply sent from cold', model.reply('À tout de suite'), true);
    await until(() => model.view().mode === 'sent');
    check('the wait is shown step by step', pills.filter(p => p), ['Connecting to send…', 'Sending…', 'Sent']);
    check('...as the engine said', events.filter(e => e.command === 'bt_send_progress').map(e => e.stage), ['connecting', 'sending']);
    check('reading lease taken with the card and handed back with it', leases().map(l => [l.name, l.held]),
        [['reading', true], ['reading', false]]);
    await until(() => lastConn().classic_state === 'idle');
    check('afterwards the classic link closes by itself, still linked', [lastConn().classic_connected, model.view().link], [false, 'linked']);

    model.cardOpened();
    model.startCompose();
    await until(() => lastConn().classic_state === 'open');
    check('writing a new message opens the link through the lease', lastConn().classic_leases, ['reading']);
    model.cardClosed();
    await until(() => lastConn().classic_state === 'idle');

    fake.say('refuse-ouverture');
    await sleep(100);
    model.cardOpened();
    model.openThread(CAMILLE);
    model.reply('Tu me reçois ?');
    await until(() => model.view().mode === 'notice');
    model.cardOpened();
    check('iPhone out of reach for the send: Not sent, the text kept, the engine’s reason',
        [model.view().card.draft, model.view().card.subtitle], ['Tu me reçois ?', 'Couldn’t reach the iPhone to send.']);
    model.cardClosed();
    fake.say('accepte-ouverture');
    await until(() => lastConn().classic_state === 'idle');

    // The mode, chosen in Phone Settings.
    model.wantSetting('link-mode', 'full');
    await until(() => lastConn().link_mode_effective === 'full');
    check('Always connected: bt_set_link_mode reaches the engine, both links up',
        [fake.received().some(l => l.includes('"bt_set_link_mode"') && l.includes('"full"')), lastConn().classic_connected,
            model.view().card.notices], [true, true, []]);
    fake.say('notifications-en-pause phone_silent');
    await until(() => model.view().card.paused !== null);
    check('notifications paused while messages work: said, with the last battery level',
        [model.view().link, model.view().icon.state, model.view().card.paused.cause, model.view().card.battery, model.view().card.batteryStale],
        ['linked', 'paused', 'phone_silent', 96, true]);
    fake.say('notifications-reprises');
    await until(() => model.view().card.paused === null && model.view().icon.state === 'linked');
    model.wantSetting('link-mode', 'lean');
    await until(() => lastConn().link_mode_effective === 'lean' && lastConn().classic_state === 'idle');
    fake.say('garde-le-lien');
    await until(() => lastConn().link_mode_reason === 'phone_keeps_link');
    check('the iPhone keeps the link: the mode in force is said to Phone Settings, not in the menu',
        [model._linkModeState(), model.view().card.notices],
        ['full:phone_keeps_link', []]);
    fake.say('rend-le-lien');
    await until(() => lastConn().link_mode_effective === 'lean');

    // A call announced before the hands-free link.
    events.length = 0;
    fake.say('appel');
    await until(() => model.view().mode === 'ring');
    check('incoming call: card at once, Answer here connecting, no bt_calls yet',
        [model.view().card.name, model.view().card.answerHere, events.some(e => e.command === 'bt_calls' && e.calls.length > 0)],
        ['Camille Martin', 'connecting', false]);
    await until(() => model.view().card.answerHere === 'ready');
    check('hands-free arrived: ready, same call', [model.view().mode, model.view().card.name], ['ring', 'Camille Martin']);
    model.answer('computer');
    await until(() => model.view().mode === 'call' && model.view().card.audio === 'computer' && !model.view().card.moving);
    check('Answer here: on a call, sound on this computer', model.view().card.audio, 'computer');
    fake.say('raccroche');
    await until(() => model.view().mode === 'idle');

    fake.say('mains-libres jamais');
    fake.say('delai sonnerie 0.6');
    await until(() => lastConn().classic_state === 'idle');
    fake.say('appel-inconnu');
    await until(() => model.view().mode === 'ring' && model.view().card.answerHere === 'unavailable');
    check('hands-free never comes: unavailable, the caller shown by number', model.view().card.name, '+41 22 555 01 99');
    model.decline();
    await until(() => model.view().mode === 'idle');
    check('Decline went by the notification', JSON.parse(fake.received().findLast(l => l.includes('"decline"'))).path, undefined);
    fake.say('mains-libres normal');
    await until(() => lastConn().classic_state === 'idle');
    shown.length = 0;
    fake.say('appel');
    await until(() => model.view().mode === 'ring');
    fake.say('raccroche');
    await until(() => model.view().mode === 'idle' && shown.some(([k]) => k === 'show'));
    check('the caller gives up: the missed call is a desktop notification', shown.filter(([k]) => k === 'show').length, 1);
    await until(() => lastConn().classic_state === 'idle');

    // Placing a call.
    events.length = 0;
    model.cardOpened();
    model.startDial();
    model.dialQuery('cam');
    await until(() => model.view().card.results?.length > 0);
    check('Call…: the contact found by its number', model.view().card.results[0].number, '+41790000001');
    model.dialPick(0);
    client.send({command: 'bt_status'});
    check('bt_call_dial does not hold the queue: the next command is answered meanwhile',
        [await until(() => pending(client) === 0, 300), model.view().mode], [true, 'dialing']);
    await until(() => model.view().mode === 'call' && events.some(e => e.stage === 'audio_here'));
    check('the call went through its steps, then the usual card, the sound here as asked',
        [events.filter(e => e.command === 'bt_call_progress').map(e => e.stage), model.view().card.kind,
            JSON.parse(fake.received().findLast(l => l.includes('"bt_call_dial"'))).audio,
            model.view().card.audio, model.view().card.moving, lastConn().calls.audio_wanted],
        [['connecting', 'hands_free', 'dialing', 'audio_here'], 'call', 'here', 'computer', false, 'here']);
    fake.say('raccroche');
    await until(() => model.view().mode === 'idle' && lastConn().classic_state === 'idle');

    model.cardOpened();
    model.startDial({number: '+41790000002', name: 'Léo'});
    model.dialPick(0);
    await until(() => events.some(e => e.command === 'bt_call_progress' && e.stage === 'connecting'));
    events.length = 0;
    model.cancelDial();
    await until(() => events.some(e => e.command === 'bt_call_result'));
    check('Cancel: bt_call_cancel, the engine gives up, back to the menu',
        [events.find(e => e.command === 'bt_call_result').reason, model.view().mode, model.view().card.kind],
        ['cancelled', 'idle', 'menu']);
    await sleep(900);
    check('...and no call appears afterwards', model.view().mode, 'idle');
    model.cardClosed();

    for (const [mode, reason] of [['echec-lien', 'no_link'], ['echec-mains-libres', 'no_hands_free']]) {
        fake.say(`composition ${mode}`);
        await until(() => lastConn().classic_state === 'idle');
        events.length = 0;
        model.cardOpened();
        model.startDial({number: '+41790000002', name: 'Léo'});
        model.dialPick(0);
        await until(() => model.view().card.failed === true);
        check(`call that fails (${reason}): said on the card`,
            [events.find(e => e.command === 'bt_call_result').reason, model.view().card.subtitle],
            [reason, 'Couldn’t reach the iPhone.']);
        model.cardClosed();
    }
    fake.say('composition normale');

    // The sound asked here does not come: said, the call goes on.
    fake.say('son-appel echec');
    await until(() => lastConn().classic_state === 'idle');
    model.cardOpened();
    model.startDial({number: '+41790000002', name: 'Léo'});
    model.dialPick(0);
    await until(() => model.view().mode === 'call');
    check('sound asked here, not here yet: on its way', [model.view().card.audio, model.view().card.moving, model.view().card.isError],
        ['computer', true, false]);
    await until(() => model.view().card.isError === true);
    check('audio_failed: said, the call goes on with the iPhone',
        [model.view().mode, model.view().card.subtitle, model.view().card.audio, lastConn().calls.audio_failed],
        ['call', 'Couldn’t bring the call sound to this computer', 'phone', true]);
    fake.say('raccroche');
    fake.say('son-appel normal');
    await until(() => model.view().mode === 'idle' && lastConn().classic_state === 'idle');
    model.cardClosed();

    // Accepted by the iPhone, and no call ever shows up.
    fake.say('composition sans-appel');
    events.length = 0;
    model.cardOpened();
    model.startDial({number: '+41790000002', name: 'Léo'});
    model.dialPick(0);
    await until(() => model.view().card.failed === true);
    check('no_call: the call didn’t start, with a way to try again',
        [events.filter(e => e.command === 'bt_call_progress').map(e => e.stage).pop(), model.view().card.subtitle],
        ['no_call', 'The call didn’t start.']);
    model.cardClosed();
    fake.say('composition normale');
    await until(() => lastConn().classic_state === 'idle');

    // The same gestures against the engine of before.
    fake.say('protocole-ancien');
    await until(() => lastConn().link_mode === undefined && lastConn().classic_connected === true);
    check('older engine again: no on-demand line, messages by the open session',
        [model.view().card.notices, model.view().card.composeAction, model._linkModeState()], [[], true, '']);
    model.cardOpened();
    model.startDial({number: '+41790000002', name: 'Léo'});
    model.dialPick(0);
    await until(() => model.view().mode === 'call');
    check('older engine: bt_call_dial places the call, as it always could', model.view().card.kind, 'call');
    fake.say('raccroche');
    await until(() => model.view().mode === 'idle');
    model.onChange = null;

    // --- an engine from before the daily-life commands
    fake.say('quotidien-ancien');
    fake.say('relance');
    await until(() => states[states.length - 1] !== 'connected');
    await until(() => states[states.length - 1] === 'connected' && model.view().link === 'linked');
    await until(() => pending(client) === 0);
    check('older engine: bt_audio and bt_presence are answered by a bare OK, nothing waits',
        [pending(client), model.view().card.phoneAudioHere], [0, null]);
    fake.say('batterie-faible');
    await until(() => model.view().icon.state === 'battery');
    check('older engine: the battery is said by the icon too', model.view().mode, 'idle');

    // --- engine dies: absent, then back
    states.length = 0;
    fake.stop();
    await until(() => states.includes('absent'));
    check('engine stops: reported absent', [states[0], model.view().link], ['absent', 'engine']);
    await sleep(700);
    fake = new Fake(['--etat', 'hors-portee']);
    await fake.ready();
    await until(() => model.view().link === 'away');
    check('engine back: state read again', [states.at(-1), model.view().link], ['connected', 'away']);
    check('start-up sequence replayed', fake.receivedCommands().slice(0, 2), ['subscribe', 'bt_status']);

    // --- stop: nothing left
    states.length = 0;
    client.stop();
    fake.stop();
    await sleep(500);
    check('stop: no callback afterwards', states, []);
    check('stop: nothing pending', pending(client), 0);
}

main().catch(e => {
    failed++;
    printerr(`${e}\n${e.stack}`);
}).finally(() => {
    // Never leave a test engine behind, whatever happened.
    for (const fake of fakes)
        fake.stop();
    GLib.spawn_command_line_sync(`rm -rf ${dir}`);
    print(`\n${count - failed}/${count} checks passed`);
    loop.quit();
});
loop.run();
if (failed > 0)
    imports.system.exit(1);
