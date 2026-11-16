// SPDX-License-Identifier: GPL-2.0-or-later
// The heart of the state model: what the engine says comes in through
// handleEvent(), time through nextDeadline() and expire(). No Shell or GLib
// import: time comes from the `now` function, commands leave through `send`.

import {
    FIX_TIMEOUT_MS,
    LINK_SETTLE_MS,
    LOW_BATTERY,
    NOTICE_PILL_MS,
    OBEX_STUCK_MS,
    STEADY_MS,
} from './constants.js';

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

        // Called with no argument after anything view() depends on changed.
        this.onChange = null;
        // Called with (name, value) when the engine holds another value than
        // the last one given through wantSetting(): 'phone-audio-here',
        // 'lock-on-away', 'lock-away-seconds'. Phone Settings shows them.
        this.onSetting = null;
        // Called with {kind: 'battery', level, threshold} for what deserves a
        // quiet desktop notification.
        this.onAlert = null;
        this.batteryAlerts = true;
        // The Internet connection of the iPhone, as NetworkManager sees it:
        // {state, failed}, given by the extension (lib/hotspot.js).
        this._hotspot = null;
        this._wanted = new Map();          // setting -> {value, until}
        // Kept for the whole session of the Shell, whatever the engine and
        // the link do: what was already shown is never shown a second time.
        this._seen = new Set();            // message handles
        this._shownNotes = new Map();      // notification fingerprint -> uid it was shown under
        this._liveNotes = new Set();       // uids shown on the desktop right now
        // Called with ('show', notification) and ('remove', {uid}) for the
        // iPhone notifications that become desktop notifications, with
        // ('reset', {uids}) when the engine gives the full list again, and
        // with ('reply', {uid, appId, number}) when one of them can be
        // answered in the app's own client on this computer.
        this.onNotification = null;

        this._engine = 'connecting';
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
        this._replyNumbers = new Map();    // sender name -> number, or null: not sure
        this._replyWaiting = new Map();    // sender name -> [{uid, appId}] awaiting the contacts
        this._audioState = null;           // last bt_audio, null with an older engine
        this._presence = null;             // last bt_presence
        this._batteryTold = new Set();     // thresholds already announced
        this._answeredHere = null;         // path of a call answered from this computer
        this._audioMove = null;            // {where, until}: a move of the call sound under way
        this._audioFailed = null;          // 'phone' | 'computer': a move that did not happen, shown until it does or another is asked
        // An engine that goes away is not the link dropping.
        this._wasUp = false;
        this._steadySince = null;
    }

    _changed() {
        this._tourRetry();
        this._steadyCheck();
        this._settle();
        this.onChange?.();
    }

    // --- engine side -------------------------------------------------------

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
        case 'bt_send_result':
            this._onSendResult(event);
            break;
        case 'bt_calls':
            this._onCalls(event);
            break;
        case 'bt_call_result':
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
        this._watchLinks();
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
                if (!this._transient && !this._hush()) {
                    this._transient = {
                        kind: 'notice',
                        // Translators: %d is the iPhone battery level.
                        text: this._('iPhone %d %%').replace('%d', battery).replace('%%', '%'),
                        until: this._now() + NOTICE_PILL_MS,
                    };
                }
            }
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
        if (this._candidate)
            times.push(this._candidate.since + this.settleMs);
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
        if (!this._busy()) {
            if (t?.kind === 'message')
                this._showThread(t.message.thread, t.message);
            else if (t?.kind === 'failed')
                this._showThread(t.thread, null);
        }
        this._changed();
    }

    cardClosed() {
        if (!this._cardOpen && !this._thread)
            return;
        this._cardOpen = false;
        this._thread = null;
        this._threadMessages = null;
        this._compose = null;
        this._changed();
    }

    refreshLists() {
        if (this._engine !== 'connected')
            return;
        this._send({command: 'bt_list_threads'});
        this._send({command: 'bt_list_call_history'});
    }
}
