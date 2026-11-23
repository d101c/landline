// SPDX-License-Identifier: GPL-2.0-or-later
// view(): everything the top bar, the card and the setup dialog must show,
// computed from the state. Nothing here changes the state.

import {formatDuration, formatNumber, sender} from './format.js';
import {AUDIO_REASON, DIAL_STAGE, FIELD, LE_CAUSE, SEND_STAGE} from './protocol.js';

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
        // The drawing at the head of the pill: {state, moving, dim}. `state`
        // is one of 'bare', 'linked', 'away', 'unread', 'ringing', 'call',
        // 'missed', 'voicemail', 'paused', 'battery', 'reconnecting',
        // 'internet', 'internet_failed', 'attention' (see _iconAtRest()).
        let icon = {state: 'bare', moving: false};
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
            const moving = (this._audioMove !== null || this._audioFollowed()) && this._callRoute() !== where;
            pill = {tone: 'call', strong: `${this._short(name)} · ${clock}`};
            icon = {state: 'call', moving: true};
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
                // The hands-free link is there to bring the sound here.
                computerReady: this._computerReady(),
                isError: this._callError !== '',
                // Answered from this computer with the sound left on the
                // iPhone: the iPhone may wait for a tap before it plays.
                hint: this._audioFailed === 'phone'
                    ? _('The sound is still on this computer. To move it, tap Audio on the iPhone and choose iPhone.')
                    : this._audioFailed === 'computer' && this._audioReason === AUDIO_REASON.voiceLinkUnused
                        // The iPhone sent the sound and nothing here plays it.
                        ? _('The call may be silent on both sides. Choose iPhone to hear it there.')
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
            icon = {state: 'ringing', moving: true};
            pill = {
                tone: 'accent',
                // A bare number is long enough by itself.
                strong: name === _('Unknown caller') ? _('Incoming call')
                    : sender(name).initials ? _('%s is calling').replace('%s', this._short(name))
                        : name,
            };
            card = {
                kind: 'ring',
                name,
                initials: sender(name).initials,
                subtitle: this._callError || (this._incoming?.answered === 'phone'
                    ? _('Answering on the iPhone…') : _('Calling your iPhone')),
                isError: this._callError !== '',
                // Two ways to answer when the sound can come here; else one.
                answerChoice: this._incoming ? true : this._canChooseCallAudio(),
                // Announced by its notification, before the hands-free link
                // is up: null with an engine that does not do that, else
                // whether "Answer here" is 'connecting', 'ready' or
                // 'unavailable'.
                answerHere: this._answerHere(),
                answerOnPhone: !this._incoming || this._incoming.canAnswerOnPhone || !!ringing.path,
                canDecline: !this._incoming || this._incoming.canDecline || !!ringing.path,
            };
            a11y = pill.strong;
        } else if (this._pairing) {
            mode = 'pair';
            pill = {tone: 'accent', strong: _('Confirm link')};
            card = {kind: 'pair', code: this._pairing.code};
            a11y = _('Confirm the link with your iPhone');
        } else if (this._thread) {
            mode = 'message';
            // Shown from its notification, the conversation not known yet.
            const provisional = this._provisional.get(this._thread);
            const thread = this._threads.get(this._thread) ?? {};
            const who = provisional
                ? sender(provisional.message.name, '', '')
                : sender(thread.name, thread.address, this._thread);
            const sending = this._pending?.thread === this._thread;
            // A conversation just started from "New Message…" has nothing yet.
            const shown = this._threadMessages?.length
                ? this._threadMessages
                : thread.preview || thread.timestamp
                    ? [{body: thread.preview ?? '', timestamp: thread.timestamp ?? 0}] : [];
            const last = shown[shown.length - 1] ?? {body: '', timestamp: 0};
            pill = this._messagePill(who, last.body);
            // Being read: nothing to draw the eye to.
            icon = {state: 'unread', moving: false};
            if (shown.length === 0)
                pill.text = _('New message');
            card = {
                kind: 'reply',
                thread: this._thread,
                name: who.name,
                initials: who.initials,
                subtitle: sending ? this._sendingText() : this._replyError,
                isError: !sending && this._replyError !== '',
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
                repliable: !provisional && thread.repliable !== false,
                // The message is here, its conversation is on its way.
                arriving: !!provisional,
                // A reply is on its way: nothing else can be sent meanwhile.
                busy: this._pending !== null,
                // The number can be called from here.
                callable: !provisional && this._canDial() ? this._dialable(this._thread) : '',
            };
            a11y = _('Message from %s').replace('%s', who.name);
        } else if (this._dial?.pending) {
            mode = 'dialing';
            const pending = this._dial.pending;
            const stage = this._dialText();
            // The top bar has room for a word; the card says it in full.
            pill = {
                tone: 'accent',
                strong: this._dialCancellable() ? _('Connecting…') : _('Calling…'),
            };
            // A call on its way looks like a call: the same slow wave.
            icon = {state: 'call', moving: true};
            card = {
                kind: 'dialing',
                name: pending.name,
                initials: sender(pending.name).initials,
                subtitle: stage,
                isError: false,
                failed: false,
                cancellable: this._dialCancellable(),
            };
            a11y = _('Calling %s').replace('%s', pending.name);
        } else if (this._noteCard !== null && this._phoneNotes.has(this._noteCard)) {
            // The card of one notification of another app.
            mode = 'note';
            const note = this._noteView(this._phoneNotes.get(this._noteCard));
            pill = this._notePill([note]);
            icon = {state: 'unread', moving: false};
            card = {kind: 'note', ...note, suggestion: this._suggestionView(this._noteCard)};
            a11y = _('Notification from %s').replace('%s', note.appName);
        } else if (this._pending) {
            mode = 'sending';
            pill = {tone: 'neutral', strong: this._sendingText()};
            a11y = pill.strong;
        } else if (t?.kind === 'message') {
            mode = 'message';
            const who = sender(t.message.name, t.message.address, t.message.thread);
            pill = this._messagePill(who, t.message.body);
            icon = {state: 'unread', moving: true};
            a11y = _('Message from %s').replace('%s', who.name);
        } else if (this._pillNotes().length > 0) {
            mode = 'note';
            const notes = this._pillNotes().map(note => this._noteView(note));
            pill = this._notePill(notes);
            icon = {state: 'unread', moving: true};
            a11y = notes.length === 1 ? _('Notification from %s').replace('%s', notes[0].appName) : pill.strong;
        } else if (t?.kind === 'summary') {
            mode = 'summary';
            pill = {
                tone: 'accent', strong: _('While you were away:'), text: this._summaryText(t.apps), wide: true,
                // The icons of the apps the sentence names.
                apps: t.appIds ?? [],
            };
            icon = {state: 'linked', moving: false};
            a11y = `${pill.strong} ${pill.text}`;
        } else if (t?.kind === 'sent') {
            mode = 'sent';
            pill = {tone: 'neutral', check: true, strong: _('Sent')};
            a11y = pill.strong;
        } else if (t?.kind === 'done') {
            mode = 'sent';
            pill = {tone: 'neutral', check: true, strong: t.text};
            a11y = pill.strong;
        } else if (t?.kind === 'failed') {
            mode = 'notice';
            pill = {tone: 'accent', strong: _('Not sent')};
            a11y = pill.strong;
        } else if (t?.kind === 'notice') {
            mode = 'notice';
            pill = {tone: 'accent', strong: t.text};
            a11y = pill.strong;
        }

        // A phone link to confirm, linked or not: the card says why it
        // cannot be called. Opened during a call, or while one is being
        // placed, it shows over that card, which comes back once it is
        // closed (#56).
        if (this._phoneAsk && (!card || ['call', 'ring', 'dialing'].includes(card.kind)))
            card = this._phoneAskCard();

        // Silence: whatever would have grown in the top bar does not. The
        // card is kept, for a menu the user opens on purpose.
        const hush = this._hush();
        const call = mode === 'call' || mode === 'ring';
        if (hush && pill && mode !== 'pair' && (!call || this._hushCalls())) {
            pill = undefined;
            a11y = undefined;
            mode = 'idle';
        }
        if (!pill)
            icon = {state: 'bare', moving: false};

        const tour = this._tour;
        if (!pill && tour && !tour.open) {
            // The tour stepped aside: one click brings it back where it was.
            pill = {
                tone: 'accent',
                strong: tour.code && !tour.answered && tour.step !== 'failed'
                    ? _('Confirm link') : _('Continue setup'),
            };
            a11y = pill.strong;
            if (!card)
                card = {kind: 'direct', action: 'link'};
        }

        if (!pill) {
            // At rest the icon is all there is: no dot, no count beside it.
            const rest = this._iconAtRest(link);
            icon = {state: rest, moving: ['unread', 'voicemail', 'paused'].includes(rest) && this._iconMoves(now)};
            // Something under way moves as long as it lasts (RECONNECT_MAX_MS
            // at most); in silence it is said still.
            if (rest === 'reconnecting')
                icon.moving = hush === null;
            // On its way to the iPhone's Internet: the arrows take turns.
            if (rest === 'internet')
                icon.moving = hush === null && this._hotspot?.state === 'connecting';
            switch (link) {
            case 'linked': {
                pill = {tone: 'quiet'};
                const paused = this._notificationsPaused();
                a11y = {
                    missed: _('iPhone linked, missed call'),
                    battery: _('iPhone linked, battery low'),
                    voicemail: _('iPhone linked, new voicemail'),
                    unread: paused ? _('iPhone linked, unread messages, notifications paused')
                        : _('iPhone linked, unread messages'),
                    paused: _('iPhone linked, notifications paused'),
                    internet: this._hotspot?.state === 'connecting'
                        ? _('iPhone linked, connecting to its Internet')
                        : _('iPhone linked, online through the iPhone'),
                    internet_failed: _('iPhone linked, its Internet did not connect'),
                    attention: _('iPhone linked, something needs your attention'),
                }[rest] ?? _('iPhone linked');
                break;
            }
            case 'away':
                pill = {tone: 'quiet'};
                a11y = rest === 'internet' ? _('Online through the iPhone') : _('iPhone out of range');
                break;
            case 'reconnecting':
                pill = {tone: 'quiet'};
                a11y = rest === 'internet' ? _('Online through the iPhone') : _('Reconnecting to the iPhone');
                break;
            case 'permission':
                pill = {tone: 'accent', strong: _('Permission needed')};
                a11y = pill.strong;
                break;
            case 'setup':
                pill = {tone: 'accent', strong: _('Link iPhone')};
                a11y = pill.strong;
                break;
            case 'relink':
            case 'broken':
                pill = {tone: 'accent', strong: _('Link again')};
                a11y = link === 'broken' ? _('The link with your iPhone is broken') : pill.strong;
                // The link would not hold: the click goes to the repair.
                if (link === 'broken' && !card)
                    card = {kind: 'direct', action: 'link'};
                break;
            case 'bluetooth':
                pill = {tone: 'quiet', dim: true, strong: _('Bluetooth is off')};
                icon.dim = true;
                a11y = pill.strong;
                break;
            case 'engine':
                pill = {tone: 'quiet', strong: _('Engine not running')};
                a11y = _('Phone link is not running');
                break;
            default:
                pill = {tone: 'quiet'};
                icon.dim = true;
                a11y = _('iPhone');
            }
        }

        if (!card && this._compose && link === 'linked')
            card = {kind: 'compose', query: this._compose.query, results: this._compose.results};
        if (!card && this._dial?.failed) {
            const failed = this._dial.failed;
            card = {
                kind: 'dialing', name: failed.name, initials: sender(failed.name).initials,
                subtitle: failed.error, isError: true, failed: true, cancellable: false,
            };
        } else if (!card && this._dial && link === 'linked') {
            card = {
                kind: 'dial', query: this._dial.query, results: this._dial.results,
                // null with an engine that cannot say where the sound goes.
                audio: this._dialAudioChoice() ? this._dial.where : null,
            };
        }
        if (!card)
            card = this._restCard(link, deviceName);

        return {
            mode, link, unread, pill, icon, card, a11y, cardOpen: this._cardOpen, tour: this._tourView(),
            // 'flapping' or 'repair' while the indicator is held on "Link again".
            unsteady: this.steady && link === 'broken' ? this._unsteady?.reason ?? null : null,
            // 'muted', 'sharing' or 'dnd' while nothing may pop up.
            quiet: hush,
        };
    }

    // A reply on its way: the classic link is being opened for it, then it
    // is sent.
    _sendingText() {
        return this._pending?.stage === SEND_STAGE.connecting
            ? this._('Connecting to send…') : this._('Sending…');
    }

    // A call being placed, step by step. An engine that says nothing of the
    // steps is simply calling.
    _dialText() {
        const _ = this._;
        return {
            [DIAL_STAGE.connecting]: _('Connecting to iPhone…'),
            [DIAL_STAGE.handsFree]: _('Getting the call ready…'),
        }[this._dial?.pending?.stage] ?? _('Calling…');
    }

    _messagePill(who, body) {
        const pill = {tone: 'accent', strong: who.short, text: this._preview(body)};
        if (who.initials)
            pill.avatar = who.initials;
        return pill;
    }

    // One notification: the app, then who or what and the first words.
    // Several: how many, and from which apps.
    _notePill(notes) {
        if (notes.length > 1) {
            const apps = [...new Set(notes.map(note => note.appName))];
            return {
                tone: 'accent',
                // Translators: %d is a number of notifications, 2 or more.
                strong: this._('%d notifications').replace('%d', notes.length),
                text: apps.slice(0, 2).join(', ') + (apps.length > 2 ? '…' : ''),
                // The icons of the first apps, in the order of the text.
                apps: [...new Set(notes.map(note => note.appId))].slice(0, 3),
            };
        }
        const [note] = notes;
        const words = [note.title, note.body.replace(/\s+/g, ' ').trim()].filter(s => s).join(': ');
        // The icon of the app when there is one, else its initials.
        const pill = {tone: 'accent', strong: note.appName, text: words, apps: [note.appId]};
        if (note.initials)
            pill.avatar = note.initials;
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
            // And what it is wished for in the same pass: 'le_params', or
            // nothing. Turned down, the tour goes on.
            permissionWishes: tour.step === 'permission' && !this._wishesDeclined ? this._systemWishes() : [],
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
            // Not installed: the way to install it comes first. Installed
            // but silent: Phone Settings › Engine can update it, go back to
            // the previous one, or show the steps by hand.
            if (this._engineInstall?.installed === false) {
                return {
                    kind: 'problem',
                    problem: 'engine',
                    title: _('Landline needs its engine'),
                    text: _('A small background service that talks to your iPhone. It installs in one click, from Phone Settings.'),
                    action: {id: 'install-engine', label: _('Install the Engine…')},
                    secondary: {id: 'retry', label: _('Try Again')},
                };
            }
            return {
                kind: 'problem',
                problem: 'engine',
                title: _('The engine is not running'),
                text: _('The background service that talks to your iPhone has stopped. It is tried again every few seconds.'),
                action: {id: 'retry', label: _('Try Again')},
                secondary: {id: 'install-engine', label: _('Engine Settings…')},
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
        // When all is well the menu says nothing of the link: the icon does.
        // Something not well is one short line here, with what to do about
        // it, in the order of the marks of the icon (_iconAtRest()).
        const notices = [];
        // Since the controller of this computer was reset, the iPhone has not
        // come back (#59). The engine restarts the adapter itself when nothing
        // else is connected; otherwise, or when that did not help, this says
        // whose Bluetooth to turn off and on, and only that.
        const afterReset = conn[FIELD.leAfterReset];
        if (link !== 'linked' && afterReset === 'computer')
            notices.push(_('Turn Bluetooth off and on on this computer to link again.'));
        else if (link !== 'linked' && afterReset === 'phone')
            notices.push(_('Turn Bluetooth off and on on the iPhone to link again.'));
        else if (link === 'reconnecting')
            notices.push([this._reconnectLine(), this._lockNote()].filter(t => t).join(' '));
        else if (link !== 'linked' && this._status?.enabled === false)
            notices.push(_('Paused'));
        else if (link !== 'linked')
            notices.push(this._awayLine());
        // Notifications do not arrive. Out of range says it all by itself.
        let paused = this._notificationsPaused();
        if (paused && link !== 'linked' && paused.cause === LE_CAUSE.phoneAway)
            paused = null;
        const attention = link === 'linked' && !paused ? this._attention() : null;
        const obex = this._obexState();
        if (attention === 'permission') {
            notices.push(this._system?.ready
                ? _('The Bluetooth settings of this computer will not survive a restart yet.')
                : _('Some features are off until a Bluetooth setting of this computer is changed.'));
        } else if (attention === 'messages') {
            notices.push({
                restarting: _('Restarting the phone link…'),
                still: _('Restarting did not bring messages and contacts back. Forget this iPhone in Phone Settings, then link it again.'),
                failed: _('The phone link could not be restarted.'),
            }[obex] ?? (obex ? _('This computer can’t offer messages and contacts right now.')
                : _('Another computer is reading the messages of this iPhone.')));
        } else if (attention === 'switches') {
            notices.push(_('Some sharing switches are still off on the iPhone.'));
        } else if (attention === 'engine') {
            notices.push(_('A newer engine is ready to install.'));
        }
        // Do Not Disturb and a shared screen are not said here: GNOME shows
        // both itself, and the menu stays quiet when the link is fine
        // (decided on 19 November).

        const canDial = this._canDial();
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
                    // The number its call button offers to call.
                    callable: canDial ? this._dialable(t.thread) : '',
                };
            });

        const history = this._history.slice(0, 5).map(c => ({
            name: c.name || formatNumber(c.number) || _('Unknown caller'),
            type: c.type ?? '',
            timestamp: c.timestamp ?? 0,
            // The number a click on the line offers to call.
            number: canDial ? this._dialable(c.number) : '',
            contact: c.name || '',
        }));
        const battery = this._batteryView(link);

        return {
            kind: 'menu',
            name: deviceName,
            linked: link === 'linked',
            // The top bar has no count any more: the menu says how many.
            unread: this._unread.size,
            battery: battery ? battery.level : null,
            // The last level known, the link that carries it being down;
            // and how old it is, in minutes.
            batteryStale: battery?.stale === true,
            batteryMinutes: battery?.minutes ?? 0,
            // Drawn red: what the engine calls low, else 20 % or less.
            batteryLow: battery !== null && !battery.stale && this._batteryIsLow(battery.level),
            // Charging: true or false when the engine says, else null (it
            // cannot tell yet: docs/moteur-api.md, "Batterie").
            batteryCharging: battery && !battery.stale && typeof conn.battery_charging === 'boolean'
                ? conn.battery_charging : null,
            // {cause, text, remedy} when notifications do not arrive.
            paused,
            notices,
            // "Mute iPhone on this computer".
            muted: this._quiet.muted,
            // "Internet from iPhone": null when NetworkManager has no
            // profile for this iPhone (model/internet.js).
            internet: this._internetView(battery && !battery.stale ? battery.level : null),
            // "Play iPhone audio on this computer": true, false, or null
            // with an engine that cannot say.
            phoneAudioHere: this._phoneAudioHere(),
            // ...and where it stands: {state, computer, hint}, see
            // _phoneAudioState().
            phoneAudio: this._phoneAudioView(),
            // The one thing to do about the line of `notices` that says
            // what is not well (see _attention()):
            // finish setting up this computer, with the system helper;
            permissionAction: attention === 'permission' && this._system !== null,
            // continue the setup, for the switches on the iPhone;
            setupAction: attention === 'switches',
            // restart the engine, for messages and contacts;
            fixAction: attention === 'messages' && (obex === 'down' || obex === 'failed'),
            // update the engine (Phone Settings › Engine).
            updateAction: attention === 'engine',
            // Messages can be sent: offer to write a new one.
            composeAction: link === 'linked' && this._messagesUsable(),
            // A call can be placed: offer "Call…".
            callAction: canDial,
            // "Use Landline for phone links?", once ever (#50).
            phoneLinksOffer: this._phoneLinksOffering,
            threads,
            history,
            // "On your iPhone": what waits there right now, or null.
            phone: this._phoneView(),
            // "Earlier": the ones that left it, when they are kept; else null.
            earlier: this._earlierView(),
        };
    }

    // Why the iPhone is not linked, from what the engine says of its presence
    // (bt_presence); "Out of range" when it does not say.
    // Something under way is "reconnecting" (_reconnectLine()): what is
    // left here is the iPhone that does not come.
    _awayLine() {
        const _ = this._;
        const p = this._presence;
        if (p?.state !== 'away')
            return p?.state === 'here' ? _('Nearby, not linked') : _('Out of range');
        const line = {
            out_of_range: _('Out of range'),
            switched_off: _('Bluetooth is off on the iPhone'),
            computer_suspended: _('Not back since this computer slept'),
        }[p.reason] ?? _('Not connected');
        if (p.lock === 'locked')
            return `${line} · ${_('this computer was locked')}`;
        if (p.lock === 'pending')
            return `${line} · ${_('this computer locks shortly')}`;
        return line;
    }

    // What the lock does while the iPhone is gone, or ''.
    _lockNote() {
        const _ = this._;
        const p = this._presence;
        if (p?.state !== 'away')
            return '';
        return p.lock === 'locked' ? _('This computer was locked when the iPhone left.')
            : p.lock === 'pending' ? _('This computer locks shortly.') : '';
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
