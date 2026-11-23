// SPDX-License-Identifier: GPL-2.0-or-later
// Calls: ringing, answering, hanging up, and where the sound goes.

import {ANSWER_HERE_MS, AUDIO_ANSWER_MS, AUDIO_MOVE_MS} from './constants.js';
import {digits, formatNumber} from './format.js';
import {
    ANSWER_HERE, CALLS, CALLS_FIELD, CALL_ACTION, CALL_AUDIO, CALL_STAGE, COMMAND, RING_FIELD, RING_STATE,
} from './protocol.js';

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
        // The call placed from here has shown up: its own card takes over.
        if (this._dial?.pending && this._activeCall())
            this._dial = null;
        if (ask)
            this._send({command: 'bt_list_calls'});
        this._phoneAskCallsChanged();
    }

    // A call announced by its notification, before the hands-free link (and
    // with it bt_calls) knows about it. `state` 'ended', or nothing at all:
    // the ringing is over.
    _onIncomingCall(event) {
        const ring = this._incoming;
        const uid = event?.[RING_FIELD.uid];
        if (!event || event[RING_FIELD.state] !== RING_STATE.ringing) {
            if (!ring || (uid !== undefined && uid !== ring.uid))
                return;
            this._incoming = null;
            // Nothing took over: the card has nothing left to show.
            if (this._calls.length === 0) {
                this._callError = '';
                if (this._cardOpen && !this._thread)
                    this._cardOpen = false;
            }
            return;
        }
        const same = ring?.uid === uid;
        const state = event[RING_FIELD.answerHere];
        this._incoming = {
            uid,
            caller: String(event[RING_FIELD.caller] ?? ''),
            canAnswerOnPhone: event[RING_FIELD.canAnswerOnPhone] !== false,
            canDecline: event[RING_FIELD.canDecline] !== false,
            answerHere: Object.values(ANSWER_HERE).includes(state) ? state : ANSWER_HERE.unavailable,
            path: event[RING_FIELD.path] || null,
            seenAt: same ? ring.seenAt : this._now(),
            answered: same ? ring.answered : null,
        };
        this._phoneAskCallsChanged();
    }

    // null without bt_incoming_call; else 'connecting', 'ready' or
    // 'unavailable': whether this computer can take the sound of the call
    // that rings.
    _answerHere() {
        const ring = this._incoming;
        if (!ring)
            return null;
        // The hands-free link shows the call: nothing left to wait for.
        if (ring.answerHere === ANSWER_HERE.ready ||
            this._calls.some(c => c.state === 'incoming' || c.state === 'waiting'))
            return ANSWER_HERE.ready;
        if (ring.answerHere === ANSWER_HERE.connecting && this._now() < ring.seenAt + ANSWER_HERE_MS)
            return ANSWER_HERE.connecting;
        return ANSWER_HERE.unavailable;
    }

    // where: 'computer' (the sound comes here) or 'phone' (it stays on the
    // iPhone); nothing with an engine that cannot choose.
    answer(where) {
        const call = this._ringingCall();
        if (!call)
            return;
        const ring = this._incoming;
        if (ring) {
            // Announced by its notification: "on iPhone" works at once, "here"
            // once the hands-free link is there.
            const here = where === 'computer';
            if (here ? this._answerHere() !== ANSWER_HERE.ready : !(ring.canAnswerOnPhone || call.path))
                return;
            const answer = {
                command: COMMAND.callAction, action: CALL_ACTION.answer, uid: ring.uid,
                audio: here ? CALL_AUDIO.here : CALL_AUDIO.phone,
            };
            if (call.path) {
                answer.path = call.path;
                this._audioMove = {where: here ? 'computer' : 'phone', until: this._now() + AUDIO_ANSWER_MS};
                this._audioFailed = null;
                this._answeredHere = call.path;
            }
            ring.answered = here ? 'computer' : 'phone';
            this._callError = '';
            this._send(answer);
            this._changed();
            return;
        }
        const command = {command: 'bt_call_action', action: 'answer', path: call.path};
        if (this._knowsCallAudio() && (where === 'computer' || where === 'phone')) {
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
        if (!call)
            return;
        const ring = this._incoming;
        if (!ring) {
            this._send({command: 'bt_call_action', action: 'hangup', path: call.path});
            return;
        }
        if (!ring.canDecline && !call.path)
            return;
        const decline = {command: COMMAND.callAction, action: CALL_ACTION.decline, uid: ring.uid};
        if (call.path)
            decline.path = call.path;
        this._send(decline);
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
        if (where === 'computer' && !this._computerReady())
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

    // The engine can be told where the sound of a call goes, whether or not
    // this computer can carry it at this instant.
    _knowsCallAudio() {
        return this._audioState !== null;
    }

    // What the engine was asked for the call, and is still working on: the
    // sound here. It asks the iPhone for it several times before it gives up.
    _audioFollowed() {
        const calls = this._conn?.calls;
        return !!this._activeCall() && calls?.[CALLS_FIELD.audioWanted] === CALL_AUDIO.here &&
            calls[CALLS_FIELD.audioFailed] !== true && this._audioFailed !== 'computer' &&
            this._callRoute() !== 'computer';
    }

    // bt_call_progress about the sound of the call: it is here, or it did
    // not come and the call goes on with the iPhone.
    _onCallAudioProgress(event) {
        if (event.stage === CALL_STAGE.audioHere) {
            if (this._audioMove?.where === 'computer')
                this._audioMove = null;
            if (this._audioFailed === 'computer') {
                this._audioFailed = null;
                this._callError = '';
            }
            return;
        }
        this._audioSoundFailed(event.reason);
    }

    _audioSoundFailed(reason) {
        if (!this._activeCall())
            return;
        this._audioMove = null;
        this._audioChoice = null;
        this._audioFailed = 'computer';
        this._audioReason = reason ?? null;
        this._callError = this._('Couldn’t bring the call sound to this computer');
    }

    // The hands-free link is there to bring the sound of the call here. An
    // engine that opens it on demand says when; the others always have it.
    _computerReady() {
        const state = this._callsState();
        return state === null || state === CALLS.ready;
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
        // The engine keeps it in `calls` too, for a card built after the event.
        if (this._conn?.calls?.[CALLS_FIELD.audioFailed] === true && route !== 'computer' &&
            this._audioFailed !== 'computer')
            this._audioSoundFailed(this._conn.calls.reason || null);
        const move = this._audioMove;
        if (!move)
            return;
        if (route === move.where)
            this._audioMove = null;
        else if (this._now() >= move.until && this._audioFollowed())
            // The engine is still asking the iPhone: it says how it ends.
            this._audioMove = null;
        else if (this._now() >= move.until)
            this._audioMoveFailed();
    }

    _audioMoveFailed() {
        const where = this._audioMove.where;
        this._audioMove = null;
        this._audioChoice = null;
        this._audioFailed = where;
        this._audioReason = null;
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
        if (!this._linked() || this._callsAsked === wanted)
            return;
        this._callsAsked = wanted;
        this._send({command: 'bt_set_calls', enabled: wanted});
    }

    // The call that rings: the one the hands-free link shows, or, before
    // that link is up, the one announced by its notification.
    _ringingCall() {
        const call = this._calls.find(c => c.state === 'incoming' || c.state === 'waiting') ?? null;
        const ring = this._incoming;
        // The same call, seen by both: it keeps the name it was announced by
        // until the hands-free link gives one.
        if (call && ring && !call.name && !this._callNames.has(call.path))
            return {...call, name: ring.caller, announced: true};
        if (call || !ring || this._activeCall())
            return call;
        return {
            path: ring.path, number: '', name: ring.caller, withheld: false, state: 'incoming',
            announced: true,
        };
    }

    _activeCall() {
        return this._calls.find(c => ['active', 'held', 'dialing', 'alerting'].includes(c.state)) ?? null;
    }

    // Where the sound of the call is: where the engine says it is. While a
    // move is under way, where it is going (the card says "moving").
    _audio() {
        if (this._audioMove)
            return this._audioMove.where;
        if (this._audioFollowed())
            return 'computer';
        const route = this._callRoute();
        if (route === null && this._audioChoice && !this._callError)
            return this._audioChoice;
        return route ?? (this._conn?.calls?.audio === 'active' ? 'computer' : 'phone');
    }

    _callName(call) {
        const name = call.name || this._callNames.get(call.path);
        // A caller the iPhone does not know is announced by number.
        if (name && call.announced && /^\+?[\d\s().\-/]{5,}$/.test(name))
            return formatNumber(name.replace(/[\s().\-/]/g, ''));
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
