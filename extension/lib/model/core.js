// SPDX-License-Identifier: GPL-2.0-or-later
// The heart of the state model: what the engine says comes in through
// handleEvent(), time through nextDeadline() and expire(). No Shell or GLib
// import: time comes from the `now` function, commands leave through `send`.

import {
    ANSWER_HERE_MS,
    FIX_TIMEOUT_MS,
    LINK_SETTLE_MS,
    LOW_BATTERY,
    NOTICE_PILL_MS,
    OBEX_STUCK_MS,
    PROVISIONAL_MS,
    STEADY_MS,
} from './constants.js';
import {ANSWER_HERE, EVENT, RING_FIELD} from './protocol.js';

export class ModelCore {
    constructor({send, now, translate, quickReplies, hostName, callControl, steady} = {}) {
        this._send = send ?? (() => {});
        this._now = now ?? (() => Date.now());
        this._ = translate ?? (s => s);
        this.quickReplies = quickReplies ?? [];
        this._hostName = hostName ?? '';
        // true / false: what Phone Settings wants for call control; null:
        // leave the engine as it is.
        this.callControl = callControl ?? null;
        // What the system helper says about the Bluetooth settings of this
        // computer: null while unknown or when there is no helper.
        this._system = null;
        // The wished settings (see _systemWishes()) were turned down, or
        // could not be made, in this session: not offered again by the tour.
        this._wishesDeclined = false;

        // Called with no argument after anything view() depends on changed.
        this.onChange = null;
        // Called with (name, value) when the engine holds another value than
        // the last one given through wantSetting(): 'phone-audio-here',
        // 'lock-on-away', 'lock-away-seconds'. Phone Settings shows them.
        this.onSetting = null;
        // "Tell me when the iPhone battery is low": the icon and the menu
        // say it, nothing pops up.
        this.batteryAlerts = true;
        // The Internet connection of the iPhone, as NetworkManager sees it:
        // {state, failed, reason}, given by the extension (lib/hotspot.js);
        // and what the switch of the menu wants of it (model/internet.js).
        this._hotspot = null;
        this._internetWish = false;       // on, waiting for the iPhone to be back
        this._internetLost = null;        // {at, away}: the connection fell
        this._internetFailure = null;     // {seen}: a failure the icon tells
        this._internetGuide = null;       // the drawing asked for (true) or put away (false)
        this._internetWorked = false;     // it has worked once
        this._wanted = new Map();          // setting -> {value, until}
        // How to choose this computer on the iPhone has been read once.
        this._phoneAudioHintSeen = false;
        this._linkModeTold = '';           // what Phone Settings was last told of the mode
        // The last battery level the engine gave, kept while the link that
        // carries it is down: {level, at}.
        this._lastBattery = null;
        // Kept for the whole session of the Shell, whatever the engine and
        // the link do: what was already shown is never shown a second time.
        this._seen = new Set();            // message handles
        this._shownNotes = new Map();      // notification fingerprint -> uid it was shown under
        this._liveNotes = new Set();       // uids shown on the desktop right now
        // Called with ('show', notification) for an iPhone notification that
        // is new, ('remove', {uid}) when it left the iPhone, ('reset', {uids})
        // when the engine gives the full list again, and with
        // ('reply', {uid, appId, number}) when the sender of one of them is
        // known for sure (its conversation can be opened, see _handoff()).
        this.onNotification = null;
        // The official apps of this computer (lib/officialApps.js), given by
        // the extension: appRoute(bundleId) says how a notification of that
        // app is handed over, appRunning(bundleId) whether its desktop app
        // is open now. null: none known.
        this.appRoute = null;
        this.appRunning = null;
        // "Don't double the notifications of an app open on this computer"
        // (Phone Settings, off by default).
        this.skipRunningApps = false;
        // A missing official app is suggested once per service, never again
        // once "Don't Suggest Apps" was clicked (see setSuggestedApps()).
        this.suggestApps = true;
        this._suggested = new Set();       // services already suggested
        // "Earlier": notifications that left the iPhone, when Phone Settings
        // asks for them to be kept. In memory only, for this session of the
        // Shell at most (see setKeepHistory()).
        this.keepHistory = false;
        this._earlier = new Map();         // fingerprint -> {appName, title, body, timestamp, at}
        // Apps that never grow the pill (see setQuietApps()).
        this._quietApps = new Set();

        this._engine = 'connecting';
        // What is installed: {installed, outdated}, or null before the
        // extension has looked (see setEngineInstall()).
        this._engineInstall = null;
        // "Fix It" under way or just done: {state, until, gone}. It outlives
        // the engine, which is restarted on purpose.
        this._fix = null;
        this._fixedAt = null;              // when a restart last seemed to work
        // The steady indicator (see _settle()). false: every change shows at
        // once and nothing latches.
        this.steady = steady ?? true;
        this.settleMs = LINK_SETTLE_MS;
        this._shown = null;                // the link state the indicator shows
        this._candidate = null;            // {link, since}: waiting to be shown
        this._drops = [];                  // when the links last dropped
        this._unsteady = null;             // {reason}: settled on "Link again"
        this._steadySince = null;          // since when the link holds, sound
        // When nothing may pop up (see _hush()). The first three are facts
        // the extension reports, the last three are choices of Phone Settings.
        this._quiet = {
            muted: false, dnd: false, sharing: false,
            followDnd: true, whileSharing: true, callsThroughDnd: false,
        };
        this._reset();
        this._settle();
    }

    _reset() {
        this._status = null;
        this._conn = null;
        this._deviceName = '';
        this._threads = new Map();
        this._history = [];
        this._liveNotes = new Set();
        this._phoneNotes = new Map();      // uid -> what waits on the iPhone right now
        this._notePrints = new Map();      // fingerprint -> uid, for those
        this._noteCard = null;             // uid of the notification its card shows
        this._showAllNotes = false;        // "Show all" was asked in the open menu
        this._leUp = false;                // the link that carries notifications is up
        this._awayNew = new Set();         // uids that came while the iPhone was away
        this._awayAt = null;               // when their summary is due
        this._batteryLow = null;           // {level, seen}: the battery went low
        this._unread = new Map();          // message handle -> thread
        this._calls = [];
        this._callNames = new Map();       // call path -> name
        this._callStarts = new Map();      // call path -> ms
        this._callAsked = new Set();       // call paths whose name was asked
        this._callError = '';
        this._audioChoice = null;          // 'phone' | 'computer' | null: asked of an engine that does not say where the sound is
        this._pairing = null;              // {code, operationId, until}
        this._transient = null;            // {kind, until, ...}
        this._pending = null;              // reply on its way
        this._thread = null;               // thread shown in the reply card
        this._cardOpen = false;
        this._drafts = new Map();
        this._replyError = '';
        this._lowBatteryTold = false;
        this._sequence = this._sequence ?? 0;
        this._devices = [];
        this._tour = null;                 // setup tour, see startTour()
        this._fresh = new Map();           // thread -> unread messages just received
        this._threadMessages = null;       // what the reply card shows
        this._callsAsked = null;
        this._contactCount = null;
        this._abandoned = null;
        this._obexSince = null;            // since when messages and contacts both fail
        this._compose = null;              // "New Message…": {query, results}
        this._phoneAsk = null;             // a phone link to confirm: {number, name, where, calls, error}
        this._phoneLinksOffer = false;     // "Use Landline for phone links" may be offered (#50)
        this._phoneLinksOffering = false;  // ...and the open menu shows it
        this._phoneLinksOffered = false;   // ...and it was shown once
        this._suggestion = null;           // {uid, key}: the card of that notification suggests an app
        this._replyNumbers = new Map();    // sender name -> number, or null: not sure
        this._replyWaiting = new Map();    // sender name -> [{uid, appId}] awaiting the contacts
        this._audioState = null;           // last bt_audio, null with an older engine
        this._presence = null;             // last bt_presence
        this._batteryTold = new Set();     // thresholds already announced
        this._answeredHere = null;         // path of a call answered from this computer
        this._audioMove = null;            // {where, until}: a move of the call sound under way
        this._audioFailed = null;          // 'phone' | 'computer': a move that did not happen, shown until it does or another is asked
        this._audioReason = null;          // why the sound did not come here, when the engine says
        this._incoming = null;             // the call that rings, as bt_incoming_call announced it
        this._dial = null;                 // "Call…": {query, results, where, pending, failed}
        this._leases = new Map();          // lease on the classic link -> when to renew it
        this._provisional = new Map();     // messages shown from their notification only
        this._missed = new Set();          // uids of missed-call notifications not looked at yet
        this._voicemail = new Set();       // uids of voicemail notifications not looked at yet
        this._attentionAt = null;          // when the icon last had something new to say
        this._wasPaused = false;           // notifications were paused at the last change
        // An engine that goes away is not the link dropping.
        this._wasUp = false;
        this._steadySince = null;
        // Reconnecting (model/reconnect.js).
        this._reconnect = null;            // {since, reason, calm} while said; {spent} once said too long
        this._signAt = null;               // the last sign of the iPhone
        this._anyUp = false;               // a link, any link, is up
        this._upAt = null;                 // since when the useful link is up
        this._upServed = false;            // ... and whether the iPhone served it
        this._fails = [];                  // when links died before they served
        this._holdOff = null;              // {since, strikes}: the engine leaves the iPhone alone
        this._phoneAudioWait = null;       // {since}: the sound of the iPhone wanted here, not here yet
        this._phoneAudioPlayedOpen = false; // it played here while the menu was open
    }

    _changed() {
        this._iconWatch();
        this._syncLeases();
        this._tourRetry();
        this._steadyCheck();
        this._reconnectWatch();
        this._internetWatch();
        this._settle();
        this._phoneAudioWatch();
        this.onChange?.();
    }

    // --- engine side -------------------------------------------------------

    // Whether the engine is installed in the home folder, and older than
    // the release this extension installs: {installed, outdated}.
    setEngineInstall(info) {
        const old = this._engineInstall;
        if (old?.installed === info?.installed && old?.outdated === info?.outdated)
            return;
        this._engineInstall = info ?? null;
        this._changed();
    }

    // 'connecting' (first try), 'connected', 'absent' (the socket does not
    // answer, the client keeps trying).
    setEngineState(state) {
        if (state === this._engine)
            return;
        const busClosed = this._busClosed();
        this._engine = state;
        if (state === 'connected')
            this._connectedAt = this._now();
        if (state !== 'connected') {
            // Notification identifiers do not survive the engine.
            this.onNotification?.('reset', {uids: []});
            const drafts = this._drafts;
            // A restart asked for by "Fix It" is no reason to drop the tour.
            // An engine that lost its session bus says so, then leaves to be
            // started again by systemd: the same thing, done by itself.
            if (!this._fix && busClosed)
                this._fix = {state: 'restarting', until: this._now() + FIX_TIMEOUT_MS, gone: true};
            const fixing = this._fix?.state === 'restarting';
            const tour = fixing && this._tour?.step === 'switches' ? this._tour : null;
            this._reset();
            this._drafts = drafts;
            this._tour = tour;
            if (fixing)
                this._fix.gone = true;
        }
        this._changed();
    }

    handleEvent(event) {
        if (!event || typeof event.command !== 'string')
            return;
        switch (event.command) {
        case 'bt_status':
            this._status = event;
            this._syncCalls();
            this._reportSettings();
            break;
        case 'bt_audio':
            this._audioState = event;
            this._audioMoved();
            this._reportSettings();
            break;
        case 'bt_presence':
            this._presence = event;
            this._reconnectPresence(event);
            break;
        case 'bt_battery_low':
            this._onBatteryLow(event);
            break;
        case 'bt_phone_audio_result':
        case 'bt_call_audio_result':
            // The engine answers with what happened, not with what was asked:
            // a refusal is final, no need to wait for the delay.
            if (event.success === false && event.command === 'bt_call_audio_result' && this._audioMove)
                this._audioMoveFailed();
            break;
        case 'bt_devices':
            this._onDevices(event);
            break;
        case 'bt_connection':
        case 'bt_connection_changed':
            this._onConnection(event);
            break;
        case 'bt_threads':
            this._onThreads(event);
            break;
        case 'bt_message':
            this._onMessage(event);
            break;
        case 'bt_message_read':
            if (event.read !== false) {
                for (const handle of event.handles ?? [])
                    this._unread.delete(handle);
            }
            this._dropFresh(event.handles ?? []);
            break;
        case EVENT.sendProgress:
            this._onSendProgress(event);
            break;
        case 'bt_send_result':
            this._onSendResult(event);
            break;
        case 'bt_calls':
            this._onCalls(event);
            // Read on demand, the list also says which call rings right now.
            if (RING_FIELD.listed in event)
                this._onIncomingCall(event[RING_FIELD.listed]);
            break;
        case EVENT.incomingCall:
            this._onIncomingCall(event);
            break;
        case EVENT.callProgress:
            this._onCallProgress(event);
            break;
        case EVENT.callResult:
            if (this._dialResult(event))
                break;
            if (event.success === false) {
                this._callError = event.message || this._('The iPhone did not do that.');
                // Not answered: no sound on its way anywhere.
                if (event.action === 'answer')
                    this._audioMove = null;
                this._audioFailed = null;
            } else if (!this._audioFailed) {
                // A move that failed stays said until it happens.
                this._callError = '';
            }
            break;
        case 'bt_call_history':
            this._history = Array.isArray(event.calls) ? event.calls : [];
            break;
        case 'bt_notification':
            this._onNotification(event, false);
            break;
        case 'bt_notifications':
            this._onNotificationList(event);
            break;
        case 'bt_notification_removed':
            this._liveNotes.delete(event.uid);
            // Looked at on the iPhone: nothing left to say here.
            this._missed.delete(event.uid);
            this._voicemail.delete(event.uid);
            this._dropProvisional(event.uid);
            this._unmirror(event.uid);
            this.onNotification?.('remove', {uid: event.uid});
            break;
        case 'bt_scan_result':
            this._tourScanEnded();
            break;
        case 'bt_contacts':
            if (!event.query)
                this._contactCount = Array.isArray(event.contacts) ? event.contacts.length : null;
            else {
                this._composeResults(event);
                this._dialResults(event);
                this._replyContacts(event);
            }
            break;
        case 'bt_pair_confirm_request':
            // The assistant was closed while its attempt was still running:
            // the number has nobody to show itself to.
            if (this._abandoned && event.operation_id === this._abandoned) {
                this._send({command: 'bt_pair_confirm', accept: false, operation_id: this._abandoned});
                break;
            }
            if (this._tour?.running && this._tour.step !== 'failed') {
                this._tourConfirmRequest(event);
                break;
            }
            this._pairing = {
                code: String(event.code ?? ''),
                operationId: event.operation_id,
                until: this._now() + (Number(event.timeout_seconds) || 60) * 1000,
            };
            break;
        case 'bt_pair_progress':
            if (['confirmed', 'declined', 'unanswered'].includes(event.step))
                this._pairing = null;
            if (event.step === 'already_paired' && this._tour?.running &&
                (event.operation_id === undefined || event.operation_id === this._tour.operationId))
                this._tour.already = true;
            break;
        case 'bt_unpair_result':
            this._tourUnpairResult(event);
            this._forgetResult(event);
            break;
        case 'bt_pair_result':
            this._pairing = null;
            if (this._abandoned && event.operation_id === this._abandoned) {
                this._abandoned = null;
                break;
            }
            if (this._tour?.operationId) {
                this._tourResult(event);
                break;
            }
            // A success is not announced on the word of the engine: the
            // indicator turns "linked" when the links are really up.
            if (!event.success)
                this._transient = {kind: 'notice', text: this._('Linking failed'), until: this._now() + NOTICE_PILL_MS};
            break;
        default:
            return;
        }
        if (this._tour)
            this._tourPump();
        this._changed();
    }

    _onDevices(event) {
        const address = this._status?.device_address;
        const devices = Array.isArray(event.devices) ? event.devices : [];
        const chosen = devices.find(d => address && d.address === address);
        this._deviceName = chosen?.name ?? '';
        this._devices = devices;
        this._tourDevices();
    }

    _onConnection(event) {
        this._conn = event;
        // Notifications travel on the LE link. When it drops, what waits on
        // the iPhone is no longer known; the engine sends all of it again
        // when the link is back.
        const le = event.le_connected === true;
        if (this._leUp && !le) {
            for (const uid of [...this._phoneNotes.keys()])
                this._unmirror(uid);
        }
        this._leUp = le;
        this._noteBattery(event);
        this._reportLinkMode();
        this._watchLinks();
        this._reconnectTrack(event);
        this._tourRescue();
        this._tourVerify();
        this._tourSwitches();
        this._obexWatch();
        this._syncCalls();
        this._audioMoved();
        const battery = event.battery;
        if (typeof battery === 'number') {
            for (const threshold of [...this._batteryTold]) {
                if (battery >= threshold + 5)
                    this._batteryTold.delete(threshold);
            }
            if (battery >= LOW_BATTERY + 5)
                this._lowBatteryTold = false;
            // An engine that announces the low battery itself (bt_battery_low)
            // is told by its battery_state; the pill is for the older ones.
            const announced = typeof event.battery_state === 'string';
            if (!announced && battery < LOW_BATTERY && !this._lowBatteryTold && this._link() === 'linked') {
                this._lowBatteryTold = true;
                if (this.batteryAlerts)
                    this._batteryLow = {level: battery, seen: false};
            }
            // Charged again, or still falling: the menu follows.
            if (this._batteryLow)
                this._batteryLow = battery >= 25 ? null : {...this._batteryLow, level: battery};
        }
    }

    // --- time --------------------------------------------------------------

    // The next moment expire() has something to do, in ms, or null.
    nextDeadline() {
        const times = [];
        if (this._transient?.until)
            times.push(this._transient.until);
        if (this._pending)
            times.push(this._pending.until);
        if (this._pairing)
            times.push(this._pairing.until);
        if (this._awayAt !== null)
            times.push(this._awayAt);
        const tour = this._tour;
        if (tour?.code && !tour.answered && !tour.paired && tour.step !== 'failed') {
            // One tick per second for the countdown.
            const left = Math.max(0, tour.until - this._now());
            times.push(tour.until - Math.floor((left - 1) / 1000) * 1000);
        }
        if (tour?.running && tour.runningUntil)
            times.push(tour.runningUntil);
        if (tour && tour.queue.length > 0)
            times.push(this._tourPumpAt());
        if (tour?.paired && !tour.verified && tour.step !== 'failed' && tour.step !== 'repair')
            times.push(tour.verifyUntil);
        if (tour?.repair?.state === 'removing')
            times.push(tour.repair.until);
        if (tour?.forget?.state === 'removing')
            times.push(tour.forget.until);
        if (tour?.retryAt)
            times.push(tour.retryAt);
        if (this._obexSince !== null && this._now() < this._obexSince + OBEX_STUCK_MS)
            times.push(this._obexSince + OBEX_STUCK_MS);
        if (this._fix?.state === 'restarting')
            times.push(this._fix.until);
        if (this._audioMove)
            times.push(this._audioMove.until);
        times.push(...this._phoneAudioDeadlines());
        for (const renewAt of this._leases.values())
            times.push(renewAt);
        for (const provisional of this._provisional.values()) {
            // Not while its card is open: it is let go when the card closes.
            if (this._thread !== provisional.message.thread)
                times.push(provisional.at + PROVISIONAL_MS);
        }
        if (this._incoming?.answerHere === ANSWER_HERE.connecting && this._now() < this._incoming.seenAt + ANSWER_HERE_MS)
            times.push(this._incoming.seenAt + ANSWER_HERE_MS);
        if (this._dial?.pending)
            times.push(this._dial.pending.until);
        if (this._candidate)
            times.push(this._candidate.since + this._settleWait(this._candidate.link));
        times.push(...this._reconnectDeadlines());
        // The icon stops moving.
        if (this._iconSettlesAt() !== null)
            times.push(this._iconSettlesAt());
        if (this._unsteady && this._steadySince !== null)
            times.push(this._steadySince + STEADY_MS);
        const call = this._activeCall();
        const start = call && this._callStarts.get(call.path);
        if (start !== undefined && start !== null && call) {
            const elapsed = Math.max(0, this._now() - start);
            times.push(start + (Math.floor(elapsed / 1000) + 1) * 1000);
        }
        return times.length > 0 ? Math.min(...times) : null;
    }

    expire() {
        const now = this._now();
        const t = this._transient;
        if (t && now >= t.until) {
            // An unopened message pill shrinks back and counts as unread.
            if (t.kind === 'message')
                this._unread.set(t.message.handle, t.message.thread);
            this._transient = null;
        }
        if (this._pending && now >= this._pending.until)
            this._finishSend(false, this._('The iPhone did not answer.'));
        if (this._pairing && now >= this._pairing.until)
            this._pairing = null;
        this._audioMoved();
        this._awaySummary();
        this._provisionalExpire();
        this._dialExpire();
        const tour = this._tour;
        if (tour?.code && !tour.answered && !tour.paired && tour.step !== 'failed' && now >= tour.until) {
            // The engine declines by itself after its delay.
            this._tourFail(this._('The number was not confirmed in time.'));
            tour.runningUntil = now + 15000;
        }
        if (tour?.running && tour.runningUntil && now >= tour.runningUntil) {
            tour.running = false;
            tour.runningUntil = null;
        }
        if (tour?.repair?.state === 'removing' && now >= tour.repair.until)
            this._tourUnpairResult({success: false});
        if (tour?.forget?.state === 'removing' && now >= tour.forget.until)
            this._forgetResult({success: false});
        this._obexWatch();
        if (tour) {
            this._tourVerify();
            this._tourPump();
        }
        this._changed();
    }

    // --- user side ---------------------------------------------------------

    // The card under the pill just opened.
    cardOpened() {
        this._cardOpen = true;
        const t = this._transient;
        // Opened for a phone link: that card, and nothing else.
        if (!this._busy() && !this._phoneAsk) {
            if (t?.kind === 'message')
                this._showThread(t.message.thread, t.message);
            else if (t?.kind === 'failed')
                this._showThread(t.thread, null);
            else if (t?.kind === 'note') {
                // One notification: its card. Several: the menu lists them.
                const notes = this._pillNotes();
                this._noteCard = notes.length === 1 ? notes[0].uid : null;
                this._transient = null;
                if (this._noteCard !== null)
                    this._offerSuggestion(this._noteCard);
            } else if (t?.kind === 'summary') {
                // The menu says the rest, under "On your iPhone".
                this._transient = null;
            }
        }
        // The menu itself was opened: the recent calls are in it, the missed
        // call, the voicemail and the low battery have been seen.
        if (!this._thread && this._noteCard === null && !this._busy() && !this._phoneAsk) {
            this._phoneLinksOfferSeen();
            this._missed.clear();
            this._voicemail.clear();
            if (this._batteryLow)
                this._batteryLow.seen = true;
            this._internetSeen();
        }
        this._changed();
    }

    cardClosed() {
        if (!this._cardOpen && !this._thread && this._noteCard === null && !this._phoneAsk)
            return;
        this._cardOpen = false;
        this._phoneAsk = null;
        this._phoneLinksOffering = false;
        this._suggestion = null;
        this._phoneAudioCardClosed();
        this._thread = null;
        this._threadMessages = null;
        this._noteCard = null;
        this._showAllNotes = false;
        this._compose = null;
        // A call on its way stays; the card to place one does not.
        if (this._dial && !this._dial.pending)
            this._dial = null;
        this._changed();
    }

    refreshLists() {
        if (this._engine !== 'connected')
            return;
        this._send({command: 'bt_list_threads'});
        this._send({command: 'bt_list_call_history'});
    }
}
