// SPDX-License-Identifier: GPL-2.0-or-later
// Placing a call: "Call…" in the menu, the call button of a conversation,
// a line of the recent calls.
//
// A contact is looked up the way "New Message…" does it, or a number is
// typed; the sound goes to this computer or stays on the iPhone. With the
// classic link on demand the engine first has to connect: it says how far it
// is (bt_call_progress), and the call can be given up until it is dialled.

import {COMPOSE_RESULTS, DIAL_PLACED_MS, DIAL_TIMEOUT_MS, NOTICE_PILL_MS} from './constants.js';
import {digits, formatNumber, sender} from './format.js';
import {CALLS, CALL_AUDIO, CALL_STAGE, COMMAND, DIAL_REASON, DIAL_STAGE} from './protocol.js';

// What can be dialled as typed: digits, spaces, + - ( ).
const TYPED = /^\+?[\d\s()-]*$/;

export const withDial = Base => class extends Base {
    // "Call…". With {number, name}: the card opens on that one, ready to be
    // called; nothing is dialled yet.
    startDial(prefill = null) {
        if (!this._canDial())
            return;
        this._thread = null;
        this._threadMessages = null;
        this._compose = null;
        this._dial = {query: '', results: [], where: this._dialDefault(), pending: null, failed: null};
        const number = this._dialable(prefill?.number);
        if (number) {
            this._dial.query = prefill.name || formatNumber(number);
            this._dial.results = [this._dialEntry(number, prefill.name)];
            this._dial.fixed = true;
        }
        this._cardOpen = true;
        this._changed();
    }

    // Linked, and calls can be placed, now or after a short connection.
    _canDial() {
        return this._link() === 'linked' && this._callsUsable() && !this._activeCall() &&
            !this._ringingCall() && !this._dial?.pending;
    }

    // Where the sound of a call placed from here goes unless told otherwise:
    // here. Whoever calls from this computer is sitting at it. (The setting
    // the engine keeps is for the calls that come in.)
    _dialDefault() {
        return 'computer';
    }

    // The card offers "This computer" / "iPhone" with an engine that can be
    // told. While the classic link is closed the engine cannot say yet
    // whether this computer can carry the sound: the choice is offered, and
    // sent, all the same. With the hands-free link up, only if it can.
    _dialAudioChoice() {
        return this._knowsCallAudio() && (this._canChooseCallAudio() || this._callsState() !== CALLS.ready);
    }

    // The number as the engine wants it, or '' for what cannot be called
    // (a sender known by letters, an e-mail address).
    _dialable(text) {
        const typed = String(text ?? '').trim().replace(/^tel:/, '');
        if (!TYPED.test(typed) || digits(typed).length < 3)
            return '';
        return `${typed.startsWith('+') ? '+' : ''}${digits(typed)}`;
    }

    _dialEntry(number, name) {
        const who = sender(name, number, `tel:${number}`);
        const shown = formatNumber(number);
        return {number, name: who.name, initials: who.initials, detail: who.name === shown ? '' : shown};
    }

    dialQuery(text) {
        const dial = this._dial;
        const query = String(text ?? '').trim();
        if (!dial || dial.pending || query === dial.query)
            return;
        dial.query = query;
        dial.fixed = false;
        dial.failed = null;
        dial.results = this._dialTyped(query);
        if (query)
            this._send({command: 'bt_list_contacts', query, limit: COMPOSE_RESULTS});
        this._changed();
    }

    // A number typed by hand is offered as it is, first.
    _dialTyped(query) {
        const number = this._dialable(query);
        return number ? [this._dialEntry(number, '')] : [];
    }

    // Contacts that have a number; their other addresses cannot be called.
    _dialResults(event) {
        const dial = this._dial;
        if (!dial || dial.pending || dial.fixed || event.query !== dial.query)
            return;
        const results = this._dialTyped(dial.query);
        for (const contact of Array.isArray(event.contacts) ? event.contacts : []) {
            for (const address of Array.isArray(contact.addresses) ? contact.addresses : []) {
                if (!String(address).startsWith('tel:'))
                    continue;
                const number = this._dialable(address);
                if (number && !results.some(r => r.number === number))
                    results.push(this._dialEntry(number, contact.name));
            }
        }
        dial.results = results.slice(0, COMPOSE_RESULTS);
    }

    // 'computer' or 'phone', for the call about to be placed.
    setDialAudio(where) {
        if (!this._dial || this._dial.pending || !['computer', 'phone'].includes(where))
            return;
        this._dial.where = where;
        this._changed();
    }

    dialPick(index) {
        const picked = this._dial?.results[index];
        if (picked)
            this.dial(picked.number, this._dial.where, picked.name);
    }

    // where: 'computer' or 'phone'; left out with an engine that cannot
    // choose, which then follows its own setting.
    dial(number, where, name = '') {
        const wanted = this._dialable(number);
        if (!wanted || !this._callsUsable() || this._dial?.pending || this._activeCall())
            return false;
        const operationId = `dial-${++this._sequence}`;
        const command = {command: COMMAND.callDial, number: wanted, operation_id: operationId};
        if (this._dialAudioChoice() && ['computer', 'phone'].includes(where))
            command.audio = where === 'computer' ? CALL_AUDIO.here : CALL_AUDIO.phone;
        const dial = this._dial ?? {query: '', results: [], where: this._dialDefault()};
        this._dial = {
            ...dial,
            failed: null,
            pending: {
                operationId, number: wanted, name: name || this._dialName(wanted),
                where: command.audio ? where : null,
                // The classic link is closed: the engine connects first, and
                // says so. An engine that says nothing is simply calling.
                stage: this._callsState() === CALLS.onDemand ? DIAL_STAGE.connecting : null,
                until: this._now() + DIAL_TIMEOUT_MS, placed: false,
            },
        };
        this._callError = '';
        this._send(command);
        this._changed();
        return true;
    }

    _dialName(number) {
        for (const thread of this._threads.values()) {
            if (thread.name && number.length >= 6 && digits(thread.address).endsWith(digits(number).slice(-9)))
                return thread.name;
        }
        return formatNumber(number);
    }

    // The engine can be told to give up, until the number is dialled; after
    // that the call is ended like any other.
    _dialCancellable() {
        const pending = this._dial?.pending;
        return !!pending && this._demandAware() && !pending.placed && pending.stage !== DIAL_STAGE.dialing;
    }

    cancelDial() {
        const dial = this._dial;
        if (!dial)
            return;
        if (dial.pending) {
            if (!this._dialCancellable())
                return;
            this._send({command: COMMAND.callCancel, operation_id: dial.pending.operationId});
        }
        // Back to the menu.
        this._dial = null;
        this._changed();
    }

    // "Try Again" after a call that could not be placed.
    retryDial() {
        const failed = this._dial?.failed;
        if (failed)
            this.dial(failed.number, failed.where, failed.name);
    }

    _onCallProgress(event) {
        const pending = this._dial?.pending;
        if (event.stage === CALL_STAGE.audioHere || event.stage === CALL_STAGE.audioFailed) {
            // About the call itself, placed from here or answered.
            this._onCallAudioProgress(event);
            return;
        }
        if (event.stage === CALL_STAGE.noCall) {
            // The iPhone took the number, and no call ever showed up.
            if (pending && (event.operation_id === undefined || event.operation_id === pending.operationId))
                this._dialFailed(this._('The call didn’t start.'));
            return;
        }
        if (!pending || !Object.values(DIAL_STAGE).includes(event.stage))
            return;
        if (event.operation_id !== undefined && event.operation_id !== pending.operationId)
            return;
        pending.stage = event.stage;
        pending.until = this._now() + DIAL_TIMEOUT_MS;
    }

    // bt_call_result for a call placed from here. It comes twice (answer
    // and broadcast). True when the event was about dialling.
    _dialResult(event) {
        if (event.action !== 'dial')
            return false;
        const pending = this._dial?.pending;
        if (!pending || (event.operation_id !== undefined && event.operation_id !== pending.operationId))
            return true;
        if (event.success === true) {
            if (this._activeCall()) {
                this._dial = null;
            } else if (!pending.placed) {
                // Placed: the call itself shows up in a moment.
                pending.placed = true;
                pending.stage = DIAL_STAGE.dialing;
                pending.until = this._now() + DIAL_PLACED_MS;
            }
            return true;
        }
        if (event.reason === DIAL_REASON.cancelled)
            this._dial = null;
        else
            this._dialFailed(this._dialReason(event));
        return true;
    }

    _dialReason(event) {
        const _ = this._;
        return {
            [DIAL_REASON.noLink]: _('Couldn’t reach the iPhone.'),
            [DIAL_REASON.noHandsFree]: _('Couldn’t reach the iPhone.'),
            [DIAL_REASON.refused]: _('The iPhone did not place the call.'),
            [DIAL_REASON.busy]: _('Another call is already being placed.'),
            [DIAL_REASON.callsOff]: _('Calls from this computer are turned off in Phone Settings.'),
        }[event.reason] ?? (event.message || _('The iPhone did not place the call.'));
    }

    _dialFailed(error) {
        const {number, name, where} = this._dial.pending;
        this._dial.pending = null;
        this._dial.failed = {number, name, where, error};
        // Nobody is looking at the card: say it in the top bar for a moment.
        if (!this._cardOpen && !this._hush())
            this._transient = {kind: 'notice', text: this._('Call not placed'), until: this._now() + NOTICE_PILL_MS};
    }

    _dialExpire() {
        const pending = this._dial?.pending;
        if (!pending || this._now() < pending.until)
            return;
        if (pending.placed)
            this._dialFailed(this._('The call didn’t start.'));
        else
            this._dialFailed(this._('The iPhone did not answer.'));
    }
};
