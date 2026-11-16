// SPDX-License-Identifier: GPL-2.0-or-later
// view(): everything the top bar, the card and the setup dialog must show,
// computed from the state. Nothing here changes the state.

import {formatDuration, formatNumber, sender} from './format.js';

export const withView = Base => class extends Base {
    view() {
        const _ = this._;
        const now = this._now();
        // What the indicator shows at rest follows the link with a delay
        // (_settle()); everything else here is immediate.
        const link = this.steady ? this._shown ?? this._link() : this._targetLink();
        const unread = this._unread.size;
        const active = this._activeCall();
        const ringing = active ? null : this._ringingCall();
        const t = this._transient;
        const deviceName = this._deviceName || _('iPhone');

        let mode = 'idle';
        let pill;
        let card;
        let a11y;

        if (active) {
            mode = 'call';
            const name = this._callName(active);
            const who = sender(name);
            const start = this._callStarts.get(active.path);
            const clock = start === undefined ? _('Calling…') : formatDuration(now - start);
            const where = this._audio();
            // On its way there, not there yet.
            const moving = this._audioMove !== null && this._callRoute() !== where;
            pill = {tone: 'call', icon: 'call', strong: `${this._short(name)} · ${clock}`};
            card = {
                kind: 'call',
                name,
                initials: who.initials,
                subtitle: this._callError || (moving
                    ? (where === 'computer'
                        // Translators: %s is the call duration, like 02:41.
                        ? _('%s · moving to this computer…').replace('%s', clock)
                        : _('%s · moving to iPhone…').replace('%s', clock))
                    : where === 'computer'
                        ? _('%s · sound on this computer').replace('%s', clock)
                        : _('%s · sound on iPhone').replace('%s', clock)),
                audio: where,
                moving,
                isError: this._callError !== '',
                // Answered from this computer with the sound left on the
                // iPhone: the iPhone may wait for a tap before it plays.
                hint: this._audioFailed === 'phone'
                    ? _('The sound is still on this computer. To move it, tap Audio on the iPhone and choose iPhone.')
                    : this._audioFailed === 'computer'
                        ? _('The sound is still on the iPhone.')
                    : this._answeredHere === active.path && where === 'phone' && !this._callError
                        ? _('No sound on the iPhone? Tap Audio there and choose iPhone.') : '',
            };
            a11y = _('On a call with %s').replace('%s', name);
        } else if (ringing) {
            mode = 'ring';
            const name = this._callName(ringing);
            // Translators: %s is a first name.
            pill = {
                tone: 'accent',
                icon: 'call',
                // A bare number is long enough by itself.
                strong: name === _('Unknown caller') ? _('Incoming call')
                    : sender(name).initials ? _('%s is calling').replace('%s', this._short(name))
                        : name,
            };
            card = {
                kind: 'ring',
                name,
                initials: sender(name).initials,
                subtitle: this._callError || _('Calling your iPhone'),
                isError: this._callError !== '',
                // Two ways to answer when the sound can come here; else one.
                answerChoice: this._canChooseCallAudio(),
            };
            a11y = pill.strong;
        } else if (this._pairing) {
            mode = 'pair';
            pill = {tone: 'accent', icon: 'phone', strong: _('Confirm link')};
            card = {kind: 'pair', code: this._pairing.code};
            a11y = _('Confirm the link with your iPhone');
        } else if (this._thread) {
            mode = 'message';
            const thread = this._threads.get(this._thread) ?? {};
            const who = sender(thread.name, thread.address, this._thread);
            // A conversation just started from "New Message…" has nothing yet.
            const shown = this._threadMessages?.length
                ? this._threadMessages
                : thread.preview || thread.timestamp
                    ? [{body: thread.preview ?? '', timestamp: thread.timestamp ?? 0}] : [];
            const last = shown[shown.length - 1] ?? {body: '', timestamp: 0};
            pill = this._messagePill(who, last.body);
            if (shown.length === 0)
                pill.text = _('New message');
            card = {
                kind: 'reply',
                thread: this._thread,
                name: who.name,
                initials: who.initials,
                subtitle: this._replyError,
                isError: this._replyError !== '',
                // Oldest first. An empty text is something Bluetooth cannot
                // carry: a photo, a voice message.
                messages: shown.map(m => ({
                    body: String(m.body ?? '').trim(),
                    timestamp: m.timestamp ?? 0,
                })),
                timestamp: last.timestamp ?? 0,
                // Nobody answers "On my way" to a verification code.
                quick: who.shortCode ? [] : this.quickReplies,
                draft: this._drafts.get(this._thread) ?? '',
                repliable: thread.repliable !== false,
            };
            a11y = _('Message from %s').replace('%s', who.name);
        } else if (this._pending) {
            mode = 'sending';
            pill = {tone: 'neutral', strong: _('Sending…')};
            a11y = pill.strong;
        } else if (t?.kind === 'message') {
            mode = 'message';
            const who = sender(t.message.name, t.message.address, t.message.thread);
            pill = this._messagePill(who, t.message.body);
            a11y = _('Message from %s').replace('%s', who.name);
        } else if (t?.kind === 'sent') {
            mode = 'sent';
            pill = {tone: 'neutral', icon: 'check', strong: _('Sent')};
            a11y = pill.strong;
        } else if (t?.kind === 'done') {
            mode = 'sent';
            pill = {tone: 'neutral', icon: 'check', strong: t.text};
            a11y = pill.strong;
        } else if (t?.kind === 'failed') {
            mode = 'notice';
            pill = {tone: 'accent', icon: 'phone', strong: _('Not sent')};
            a11y = pill.strong;
        } else if (t?.kind === 'notice') {
            mode = 'notice';
            pill = {tone: 'accent', icon: 'phone', strong: t.text};
            a11y = pill.strong;
        }

        // Silence: whatever would have grown in the top bar does not. The
        // card is kept, for a menu the user opens on purpose.
        const hush = this._hush();
        const call = mode === 'call' || mode === 'ring';
        if (hush && pill && mode !== 'pair' && (!call || this._hushCalls())) {
            pill = undefined;
            a11y = undefined;
            mode = 'idle';
        }

        const tour = this._tour;
        if (!pill && tour && !tour.open) {
            // The tour stepped aside: one click brings it back where it was.
            pill = {
                tone: 'accent',
                icon: 'phone',
                strong: tour.code && !tour.answered && tour.step !== 'failed'
                    ? _('Confirm link') : _('Continue setup'),
            };
            a11y = pill.strong;
            if (!card)
                card = {kind: 'direct', action: 'link'};
        }

        if (!pill) {
            switch (link) {
            case 'linked':
                pill = unread > 0
                    ? {tone: 'quiet', icon: 'phone', count: unread}
                    : {tone: 'quiet', icon: 'phone', dot: 'on'};
                a11y = unread > 0 ? _('iPhone linked, unread messages') : _('iPhone linked');
                break;
            case 'away':
                pill = {tone: 'quiet', dim: true, icon: 'phone', dot: 'off'};
                a11y = _('iPhone out of range');
                break;
            case 'permission':
                pill = {tone: 'accent', icon: 'phone', strong: _('Permission needed')};
                a11y = pill.strong;
                break;
            case 'setup':
                pill = {tone: 'accent', icon: 'phone', strong: _('Link iPhone')};
                a11y = pill.strong;
                break;
            case 'relink':
            case 'broken':
                pill = {tone: 'accent', icon: 'phone', strong: _('Link again')};
                a11y = link === 'broken' ? _('The link with your iPhone is broken') : pill.strong;
                // The link would not hold: the click goes to the repair.
                if (link === 'broken' && !card)
                    card = {kind: 'direct', action: 'link'};
                break;
            case 'bluetooth':
                pill = {tone: 'quiet', dim: true, icon: 'phone', dot: 'off', strong: _('Bluetooth is off')};
                a11y = pill.strong;
                break;
            case 'engine':
                pill = {tone: 'quiet', icon: 'phone', dot: 'warn', strong: _('Engine not running')};
                a11y = _('Phone link is not running');
                break;
            default:
                pill = {tone: 'quiet', dim: true, icon: 'phone'};
                a11y = _('iPhone');
            }
        }

        if (!card && this._compose && link === 'linked')
            card = {kind: 'compose', query: this._compose.query, results: this._compose.results};
        if (!card)
            card = this._restCard(link, deviceName);

        return {
            mode, link, unread, pill, card, a11y, cardOpen: this._cardOpen, tour: this._tourView(),
            // 'flapping' or 'repair' while the indicator is held on "Link again".
            unsteady: this.steady && link === 'broken' ? this._unsteady?.reason ?? null : null,
            // 'muted', 'sharing' or 'dnd' while nothing may pop up.
            quiet: hush,
        };
    }

    _messagePill(who, body) {
        const pill = {tone: 'accent', strong: who.short, text: this._preview(body)};
        if (who.initials)
            pill.avatar = who.initials;
        else
            pill.icon = 'person';
        return pill;
    }

    _tourView() {
        const tour = this._tour;
        if (!tour)
            return null;
        const conn = this._conn ?? {};
        const at = tour.step === 'failed' ? tour.failedAt : tour.step;
        const switches = this._switches();
        return {
            open: tour.open,
            step: tour.step,
            at,
            // "Step n of N", over the steps this tour really goes through.
            index: Math.max(1, tour.plan.indexOf(at) + 1),
            // Forgetting is not a walk: no "Step n of N" for it.
            total: tour.step === 'forget' ? 0 : tour.plan.length,
            forget: tour.forget ? {state: tour.forget.state, back: !!tour.forget.from} : null,
            // Messages and contacts are not on offer: see _obexState().
            obex: this._obexState(),
            hostName: this._hostName,
            deviceName: tour.name || this._deviceName || '',
            permission: tour.permission,
            // No password window: the commands to run by hand instead.
            permissionManual: this._permissionManual(),
            permissionCommands: this._permissionManual() ? this._permissionCommands() : [],
            // What the password is asked for, on this computer: 'experimental',
            // 'ertm', both, or nothing.
            permissionNeeds: this._systemNeeds(),
            // Turning on the newer Bluetooth features restarts Bluetooth.
            restartsBluetooth: this._system?.experimental === false &&
                this._systemNeeds().includes('experimental'),
            candidates: tour.candidates,
            found: tour.running || tour.code !== '',
            code: tour.code,
            secondsLeft: tour.code && !tour.answered ? Math.max(0, Math.ceil((tour.until - this._now()) / 1000)) : 0,
            answered: tour.answered,
            checking: tour.checking,
            links: {classic: conn.classic_connected === true, le: conn.le_connected === true},
            repair: tour.repair?.state ?? null,
            error: tour.error,
            canRetry: !tour.running,
            switches,
            allOn: this._allOn(),
            // This computer cannot carry notifications at all.
            noNotifications: this._noNotifications(),
            reviewed: tour.reviewed,
            proof: tour.proof,
            canBack: (tour.step === 'done' && tour.plan.includes('try')) ||
                (tour.step === 'try' && tour.plan.includes('switches')),
            battery: typeof conn.battery === 'number' ? conn.battery : null,
            contacts: this._contactCount,
            conversations: this._threads.size,
            calls: this._status?.calls_enabled === true,
        };
    }

    _restCard(link, deviceName) {
        const _ = this._;
        switch (link) {
        case 'engine':
            return {
                kind: 'problem',
                problem: 'engine',
                title: _('The engine is not running'),
                text: _('The background service that talks to your iPhone has stopped or is not installed. It is tried again every few seconds.'),
                action: {id: 'retry', label: _('Try Again')},
            };
        case 'bluetooth':
            return {
                kind: 'problem',
                problem: 'bluetooth',
                title: _('Bluetooth is off'),
                text: _('Turn Bluetooth on in the system menu. The iPhone links again by itself.'),
            };
        case 'starting':
            return {
                kind: 'problem',
                problem: 'starting',
                title: this.fixing ? _('Restarting…') : _('Starting…'),
                text: _('Waiting for the phone link to answer.'),
            };
        case 'permission':
            // With the system helper, a click goes straight to the dialog.
            if (this._system)
                return {kind: 'direct', action: 'link'};
            return {
                kind: 'problem',
                problem: 'permission',
                title: _('Let this computer talk to your iPhone'),
                text: _('One or two Bluetooth settings of this computer need to change. It is done once and needs an administrator password.'),
                steps: this._setupSteps().map(step => this._plainStep(step)),
                commands: this._setupSteps().map(step => step.command).filter(c => c),
                action: {id: 'copy-commands', label: _('Copy Commands')},
            };
        case 'setup':
        case 'relink':
        case 'broken':
            return {kind: 'direct', action: 'link'};
        default:
            break;
        }

        const conn = this._conn ?? {};
        const operator = conn.calls?.operator;
        // `state` is the word on the first line of the menu, beside the
        // name; `statusLine` says it in full (read aloud, and written under
        // the first line when it says more than the word).
        let statusLine;
        let state;
        if (link === 'linked') {
            statusLine = operator ? `${_('Linked')} · ${operator}` : _('Linked');
            state = _('Linked');
        } else if (this._status?.enabled === false) {
            statusLine = state = _('Paused');
        } else {
            statusLine = this._awayLine();
            state = this._awayWord();
        }

        const notices = [];
        if (link !== 'linked' && statusLine !== state)
            notices.push(this._awayNote());
        const hush = this._hush();
        if (hush === 'dnd')
            notices.push(_('Do Not Disturb is on: nothing pops up here.'));
        else if (hush === 'sharing')
            notices.push(_('Your screen is shared: nothing pops up here.'));
        const obex = link === 'linked' ? this._obexState() : null;
        const missing = link === 'linked' && !obex && conn.map_error !== 'busy' && (
            conn.map_error === 'forbidden' || conn.pbap_error === 'forbidden' ||
            (this._linkComplete() && !this._allOn()));
        if (link === 'linked') {
            if (this._needsPermission()) {
                notices.push(this._system?.ready
                    ? _('The Bluetooth settings of this computer will not survive a restart yet.')
                    : _('Some features are off until a Bluetooth setting of this computer is changed.'));
            }
            if (obex === 'restarting')
                notices.push(_('Restarting the phone link…'));
            else if (obex === 'still')
                notices.push(_('Restarting did not bring messages and contacts back. Forget this iPhone in Phone Settings, then link it again.'));
            else if (obex === 'failed')
                notices.push(_('The phone link could not be restarted.'));
            else if (obex)
                notices.push(_('This computer can’t offer messages and contacts right now.'));
            else if (conn.map_error === 'busy')
                notices.push(_('Another computer is reading the messages of this iPhone.'));
            else if (missing)
                notices.push(_('Some sharing switches are still off on the iPhone.'));
        }

        const threads = [...this._threads.values()]
            .filter(t => t.preview || t.timestamp)
            .sort((a, b) => (b.timestamp ?? 0) - (a.timestamp ?? 0))
            .slice(0, 3)
            .map(t => {
                const who = sender(t.name, t.address, t.thread);
                const fresh = [...this._unread.values()].includes(t.thread);
                return {
                    thread: t.thread,
                    name: who.name,
                    initials: who.initials,
                    preview: this._preview(t.preview),
                    timestamp: t.timestamp ?? 0,
                    unread: fresh,
                };
            });

        const history = this._history.slice(0, 5).map(c => ({
            name: c.name || formatNumber(c.number) || _('Unknown caller'),
            type: c.type ?? '',
            timestamp: c.timestamp ?? 0,
        }));

        return {
            kind: 'menu',
            name: deviceName,
            state,
            statusLine,
            linked: link === 'linked',
            battery: link === 'linked' && typeof conn.battery === 'number' ? conn.battery : null,
            notices,
            // "Mute iPhone on this computer".
            muted: this._quiet.muted,
            // "Use iPhone's Internet Connection": null when NetworkManager
            // has no profile for this iPhone, else {state, failed}.
            hotspot: this._hotspot && this._hotspot.state !== 'unavailable' && this._status?.device_address
                ? {state: this._hotspot.state, failed: this._hotspot.failed === true} : null,
            // "Play iPhone audio on this computer": true, false, or null
            // with an engine that cannot say.
            phoneAudioHere: this._engineSetting('phone-audio-here'),
            // Linked, but the system helper still has something to do.
            permissionAction: link === 'linked' && this._system !== null && this._needsPermission(),
            // Linked, but the switches on the iPhone are not all on.
            setupAction: missing,
            // Messages and contacts are not on offer: restart the engine.
            fixAction: obex === 'down' || obex === 'failed',
            // Messages can be sent: offer to write a new one.
            composeAction: link === 'linked' && conn.map_open === true,
            threads,
            history,
        };
    }

    // Why the iPhone is not linked, from what the engine says of its presence
    // (bt_presence); "Out of range" when it does not say.
    _awayLine() {
        const _ = this._;
        const p = this._presence;
        if (p?.state !== 'away')
            return p?.state === 'here' ? _('Reconnecting…') : _('Out of range');
        const line = {
            out_of_range: _('Out of range'),
            switched_off: _('Bluetooth is off on the iPhone'),
            computer_suspended: _('Reconnecting after sleep…'),
        }[p.reason] ?? _('Not connected');
        if (p.lock === 'locked')
            return `${line} · ${_('this computer was locked')}`;
        if (p.lock === 'pending')
            return `${line} · ${_('this computer locks shortly')}`;
        return line;
    }

    // The same in a word or two, for the first line of the menu.
    _awayWord() {
        const _ = this._;
        const p = this._presence;
        if (p?.state !== 'away')
            return p?.state === 'here' ? _('Reconnecting…') : _('Out of range');
        return {
            out_of_range: _('Out of range'),
            switched_off: _('Bluetooth off'),
            computer_suspended: _('Reconnecting…'),
        }[p.reason] ?? _('Not connected');
    }

    // What the word leaves out, as a sentence under the first line.
    _awayNote() {
        const _ = this._;
        const p = this._presence;
        const line = {
            switched_off: _('Bluetooth is off on the iPhone.'),
            computer_suspended: _('Reconnecting after sleep…'),
        }[p?.reason] ?? '';
        const lock = p?.lock === 'locked' ? _('This computer was locked when the iPhone left.')
            : p?.lock === 'pending' ? _('This computer locks shortly.') : '';
        return [line, lock].filter(t => t).join(' ');
    }

    // The engine words its prerequisites for a terminal. Say the same in
    // plain words when the step is one of the two known ones.
    _plainStep(step) {
        const _ = this._;
        const what = String(step.what ?? '');
        if (/experimental/i.test(what) || /experimental/i.test(step.command ?? ''))
            return _('Turn on newer Bluetooth features (needed for notifications and calls)');
        if (/ERTM/i.test(what))
            return _('Allow reliable data transfer (needed to read messages)');
        return what;
    }
};
