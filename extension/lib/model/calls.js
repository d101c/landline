// SPDX-License-Identifier: GPL-2.0-or-later
// Calls: ringing, answering, hanging up, and where the sound goes.

import {AUDIO_ANSWER_MS, AUDIO_MOVE_MS} from './constants.js';
import {digits, formatNumber} from './format.js';

export const withCalls = Base => class extends Base {
    _onCalls(event) {
        const calls = Array.isArray(event.calls) ? event.calls : [];
        const paths = new Set(calls.map(c => c.path));
        for (const path of [...this._callNames.keys()]) {
            if (!paths.has(path))
                this._callNames.delete(path);
        }
        for (const path of [...this._callStarts.keys()]) {
            if (!paths.has(path))
                this._callStarts.delete(path);
        }
        for (const path of [...this._callAsked]) {
            if (!paths.has(path))
                this._callAsked.delete(path);
        }
        let ask = false;
        for (const call of calls) {
            if (call.name)
                this._callNames.set(call.path, call.name);
            if (call.state === 'active' && !this._callStarts.has(call.path))
                this._callStarts.set(call.path, this._now());
            // The broadcast has no contact name, the list read on demand
            // does: ask once per call.
            if (!call.name && !call.withheld && !this._callNames.has(call.path) &&
                !this._callAsked.has(call.path)) {
                this._callAsked.add(call.path);
                ask = true;
            }
        }
        const hadCalls = this._calls.length > 0;
        this._calls = calls;
        if (!paths.has(this._answeredHere))
            this._answeredHere = null;
        if (calls.length === 0) {
            this._callError = '';
            this._audioChoice = null;
            this._audioMove = null;
            this._audioFailed = null;
            if (hadCalls && this._cardOpen && !this._thread)
                this._cardOpen = false;
        }
        if (ask)
            this._send({command: 'bt_list_calls'});
    }

    // where: 'computer' (the sound comes here) or 'phone' (it stays on the
    // iPhone); nothing with an engine that cannot choose.
    answer(where) {
        const call = this._ringingCall();
        if (!call)
            return;
        const command = {command: 'bt_call_action', action: 'answer', path: call.path};
        if (this._canChooseCallAudio() && (where === 'computer' || where === 'phone')) {
            command.audio = where === 'computer' ? 'here' : 'phone';
            // Shown as on its way until the engine says the sound is there.
            this._audioMove = {where, until: this._now() + AUDIO_ANSWER_MS};
            this._audioFailed = null;
            this._answeredHere = call.path;
        }
        this._send(command);
    }

    decline() {
        const call = this._ringingCall();
        if (call)
            this._send({command: 'bt_call_action', action: 'hangup', path: call.path});
    }

    hangUp() {
        const call = this._activeCall();
        if (call)
            this._send({command: 'bt_call_action', action: 'hangup', path: call.path});
    }

    // 'phone' or 'computer', for the call in progress only: the setting of
    // the engine takes over again when the call ends.
    setCallAudio(where) {
        if (!this._activeCall() || where === this._audio())
            return;
        this._callError = '';
        this._audioFailed = null;
        if (this._audioState) {
            this._audioMove = {where, until: this._now() + AUDIO_MOVE_MS};
            this._send({command: 'bt_set_call_audio', route: where === 'computer' ? 'here' : 'phone', persist: false});
        } else {
            // An engine from before bt_set_call_audio: it does not say where
            // the sound is, so what was asked is all there is to show.
            this._audioChoice = where;
            this._send({
                command: 'bt_call_action',
                action: where === 'computer' ? 'audio_here' : 'audio_phone',
            });
        }
        this._changed();
    }

    // This computer can carry the sound of a call, and the engine can be
    // told where to put it call by call.
    _canChooseCallAudio() {
        return this._audioState?.call_audio_switchable === true;
    }

    // Where the engine says the sound of the call is: 'computer', 'phone',
    // or null when it does not say.
    _callRoute() {
        const calls = this._conn?.calls;
        const route = calls?.audio_route ?? this._audioState?.call_audio_route;
        if (route === 'here')
            return 'computer';
        if (route === 'phone')
            return 'phone';
        return null;
    }

    // A move of the call sound was asked for: done, or not in time. What the
    // card shows afterwards is where the engine says the sound is, never
    // what was asked.
    _audioMoved() {
        const route = this._callRoute();
        // A move given up on that happens after all: the error goes.
        if (this._audioFailed && route === this._audioFailed) {
            this._audioFailed = null;
            this._callError = '';
        }
        const move = this._audioMove;
        if (!move)
            return;
        if (route === move.where)
            this._audioMove = null;
        else if (this._now() >= move.until)
            this._audioMoveFailed();
    }

    _audioMoveFailed() {
        const where = this._audioMove.where;
        this._audioMove = null;
        this._audioChoice = null;
        this._audioFailed = where;
        this._callError = where === 'phone'
            ? this._('Couldn’t move the call to the iPhone')
            : this._('Couldn’t move the call to this computer');
    }

    // Call control is off in the engine until someone turns it on. Do it as
    // soon as an iPhone is linked, unless Phone Settings says otherwise.
    setCallControl(wanted) {
        this.callControl = wanted;
        this._callsAsked = null;
        this._syncCalls();
    }

    _syncCalls() {
        const wanted = this.callControl;
        if (typeof wanted !== 'boolean' || !this._status || typeof this._status.calls_enabled !== 'boolean')
            return;
        if (this._status.calls_enabled === wanted) {
            this._callsAsked = null;
            return;
        }
        if (!this._linkedEither() || this._callsAsked === wanted)
            return;
        this._callsAsked = wanted;
        this._send({command: 'bt_set_calls', enabled: wanted});
    }

    _ringingCall() {
        return this._calls.find(c => c.state === 'incoming' || c.state === 'waiting') ?? null;
    }

    _activeCall() {
        return this._calls.find(c => ['active', 'held', 'dialing', 'alerting'].includes(c.state)) ?? null;
    }

    // Where the sound of the call is: where the engine says it is. While a
    // move is under way, where it is going (the card says "moving").
    _audio() {
        if (this._audioMove)
            return this._audioMove.where;
        const route = this._callRoute();
        if (route === null && this._audioChoice && !this._callError)
            return this._audioChoice;
        return route ?? (this._conn?.calls?.audio === 'active' ? 'computer' : 'phone');
    }

    _callName(call) {
        const name = call.name || this._callNames.get(call.path);
        if (name)
            return name;
        if (call.withheld || !call.number)
            return this._('Unknown caller');
        const wanted = digits(call.number);
        for (const thread of this._threads.values()) {
            if (thread.name && wanted.length >= 6 && digits(thread.address).endsWith(wanted.slice(-9)))
                return thread.name;
        }
        return formatNumber(call.number);
    }
};
