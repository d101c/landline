// SPDX-License-Identifier: GPL-2.0-or-later
// Conversations: incoming messages, the reply card, "New Message…".

import {
    COMPOSE_RESULTS,
    FRESH_MESSAGES,
    MAX_SEEN,
    MESSAGE_PILL_MS,
    NOTICE_PILL_MS,
    PROVISIONAL_MS,
    RECENT_MESSAGE_MS,
    SEND_TIMEOUT_MS,
    SENT_PILL_MS,
} from './constants.js';
import {digits, formatNumber, sender} from './format.js';
import {FIELD, SEND_STAGE, SERVICE} from './protocol.js';

// The handle of a message known by its notification only.
const PROVISIONAL = 'ancs:';
const squeeze = text => String(text ?? '').replace(/\s+/g, ' ').trim();

export const withMessages = Base => class extends Base {
    _preview(body) {
        return String(body ?? '').replace(/\s+/g, ' ').trim() || this._('Attachment');
    }

    _dropFresh(handles) {
        for (const [thread, list] of [...this._fresh]) {
            const left = list.filter(m => !handles.includes(m.handle));
            if (left.length > 0)
                this._fresh.set(thread, left);
            else
                this._fresh.delete(thread);
        }
    }

    _onThreads(event) {
        this._threads = new Map();
        for (const thread of event.threads ?? []) {
            if (thread && typeof thread.thread === 'string')
                this._threads.set(thread.thread, {...thread});
        }
    }

    _touchThread(message) {
        if (typeof message.thread !== 'string')
            return;
        const thread = this._threads.get(message.thread) ?? {
            thread: message.thread,
            name: message.name || message.address || '',
            address: message.address ?? '',
            timestamp: 0,
            repliable: true,
        };
        if ((message.timestamp ?? 0) >= (thread.timestamp ?? 0)) {
            thread.preview = String(message.body ?? '').replace(/\s+/g, ' ').trim();
            thread.timestamp = message.timestamp ?? 0;
        }
        if (!thread.name && message.name)
            thread.name = message.name;
        this._threads.set(message.thread, thread);
    }

    _busy() {
        return this._activeCall() || this._ringingCall() || this._pairing;
    }

    _onMessage(message) {
        if (typeof message.handle !== 'string')
            return;
        const known = this._seen.has(message.handle);
        this._seen.add(message.handle);
        if (this._seen.size > MAX_SEEN)
            this._seen = new Set([...this._seen].slice(MAX_SEEN / 2));
        this._touchThread(message);

        // The first read of a session replays the whole history: only what
        // is unread, incoming and recent may show up.
        if (known || message.outgoing)
            return;
        if (message.read) {
            // Read on the iPhone before it got here: its notification goes.
            const key = this._provisionalOf(message);
            if (key !== null && this._thread !== this._provisional.get(key).message.thread)
                this._forgetProvisional(key, this._provisional.get(key));
            return;
        }
        if (this._now() - (message.timestamp ?? 0) * 1000 > RECENT_MESSAGE_MS)
            return;

        const entry = {handle: message.handle, body: String(message.body ?? ''), timestamp: message.timestamp ?? 0};
        this._tourProof(message.name || message.address, message.body);
        // Already shown from its notification: it takes that place.
        if (this._mergeProvisional(message, entry))
            return;
        if (this._cardOpen && this._thread === message.thread) {
            // The card is open on this conversation: the message joins it.
            this._threadMessages = [...this._threadMessages ?? [], entry].slice(-FRESH_MESSAGES);
            this._send({command: 'bt_mark_read', handles: [message.handle], read: true});
            return;
        }
        this._fresh.set(message.thread, [...this._fresh.get(message.thread) ?? [], entry].slice(-FRESH_MESSAGES));
        // In silence a message only adds to the count. So does one that came
        // before this session with the engine began (the Shell was locked,
        // or just started): it is told by the count, not played again.
        const before = (message.timestamp ?? 0) * 1000 < (this._connectedAt ?? 0) - 5000;
        // Just arrived: the icon moves for a minute.
        if (!before && !this._hush())
            this._attentionAt = this._now();
        if (this._busy() || this._cardOpen || this._pending || this._hush() || before) {
            this._unread.set(message.handle, message.thread);
            return;
        }
        const shown = this._transient;
        if (shown?.kind === 'message')
            this._unread.set(shown.message.handle, shown.message.thread);
        this._transient = {kind: 'message', message, until: this._now() + MESSAGE_PILL_MS};
    }

    // --- shown from the notification, before the message itself ----------------

    // With the classic link on demand, the message itself comes a few
    // seconds after its notification: the pill shows the notification at
    // once, and the message takes its place when it arrives, bringing the
    // conversation with it. True when the pill took the notification.
    _provisionalFrom(n) {
        if (this._service(FIELD.messages) === null || !this._messagesUsable())
            return false;
        const key = `${PROVISIONAL}${n.uid}`;
        if (this._seen.has(key))
            return true;
        this._seen.add(key);
        const name = squeeze(n.title);
        const message = {
            handle: key, thread: this._threadNamed(name) ?? key, name, address: '',
            body: String(n.body ?? ''), timestamp: n.timestamp || Math.floor(this._now() / 1000),
        };
        this._provisional.set(key, {
            message, note: n, at: this._now(), seen: false, group: squeeze(n.subtitle) !== '',
        });
        const entry = {handle: key, body: message.body, timestamp: message.timestamp};
        this._fresh.set(message.thread, [...this._fresh.get(message.thread) ?? [], entry].slice(-FRESH_MESSAGES));
        if (!this._hush())
            this._attentionAt = this._now();
        if (this._busy() || this._cardOpen || this._pending || this._hush()) {
            this._unread.set(key, message.thread);
            return true;
        }
        const shown = this._transient;
        if (shown?.kind === 'message')
            this._unread.set(shown.message.handle, shown.message.thread);
        this._transient = {kind: 'message', message, until: this._now() + MESSAGE_PILL_MS};
        return true;
    }

    // The one conversation with someone of that name, or with that number.
    _threadNamed(name) {
        if (!name)
            return null;
        const number = /^\+?[\d\s().\-/]{5,}$/.test(name) ? digits(name) : '';
        const found = [...this._threads.values()].filter(t =>
            t.name === name || (number.length >= 5 && digits(t.address).endsWith(number.slice(-9))));
        return found.length === 1 ? found[0].thread : null;
    }

    _provisionalOf(message) {
        const body = squeeze(message.body);
        for (const [key, entry] of this._provisional) {
            const shown = squeeze(entry.message.body);
            // A notification may be cut short: the beginning decides.
            const length = Math.min(shown.length, body.length, 60);
            if (length === 0 ? shown.length !== body.length : shown.slice(0, length) !== body.slice(0, length))
                continue;
            const title = entry.message.name;
            const number = digits(title);
            if (entry.group || entry.message.thread === message.thread || title === squeeze(message.name) ||
                (number.length >= 5 && digits(message.address).endsWith(number.slice(-9))))
                return key;
        }
        return null;
    }

    _mergeProvisional(message, entry) {
        const key = this._provisionalOf(message);
        if (key === null)
            return false;
        const provisional = this._provisional.get(key);
        this._provisional.delete(key);
        const was = provisional.message.thread;
        const others = (this._fresh.get(was) ?? []).filter(m => m.handle !== key);
        this._fresh.delete(was);
        const open = this._thread === was;
        if (open) {
            // The card is showing it: it becomes the real conversation.
            this._thread = message.thread;
            this._threadMessages = (this._threadMessages ?? []).map(m => m.handle === key ? entry : m);
        } else if (!provisional.seen) {
            this._fresh.set(message.thread,
                [...this._fresh.get(message.thread) ?? [], ...others, entry].slice(-FRESH_MESSAGES));
        }
        if (this._transient?.kind === 'message' && this._transient.message.handle === key)
            this._transient.message = message;
        if (this._unread.delete(key))
            this._unread.set(message.handle, message.thread);
        if (open || provisional.seen)
            this._send({command: 'bt_mark_read', handles: [message.handle], read: true});
        return true;
    }

    // Read on the iPhone, or cleared there, before the message came.
    _dropProvisional(uid) {
        const key = `${PROVISIONAL}${uid}`;
        const provisional = this._provisional.get(key);
        if (!provisional || this._thread === provisional.message.thread)
            return;
        this._forgetProvisional(key, provisional);
    }

    _forgetProvisional(key, provisional) {
        this._provisional.delete(key);
        const thread = provisional.message.thread;
        const left = (this._fresh.get(thread) ?? []).filter(m => m.handle !== key);
        if (left.length > 0)
            this._fresh.set(thread, left);
        else
            this._fresh.delete(thread);
        this._unread.delete(key);
        if (this._transient?.kind === 'message' && this._transient.message.handle === key)
            this._transient = null;
    }

    // The message did not follow its notification. One that belongs to a
    // known conversation stays there; one that belongs nowhere, and that
    // nobody opened, goes to the desktop like any other notification.
    _provisionalExpire() {
        const now = this._now();
        for (const [key, provisional] of [...this._provisional]) {
            if (now - provisional.at < PROVISIONAL_MS || this._thread === provisional.message.thread)
                continue;
            if (provisional.message.thread !== key) {
                this._provisional.delete(key);
                continue;
            }
            this._forgetProvisional(key, provisional);
            if (!provisional.seen)
                this._desktopNotification(provisional.note, false, true);
        }
    }

    _onSendProgress(event) {
        const pending = this._pending;
        if (!pending || !Object.values(SEND_STAGE).includes(event.stage))
            return;
        if (event.operation_id !== undefined ? event.operation_id !== pending.operationId
            : event.thread !== pending.thread)
            return;
        pending.stage = event.stage;
        // The engine is at work: the wait starts over.
        pending.until = this._now() + SEND_TIMEOUT_MS;
    }

    _onSendResult(event) {
        const pending = this._pending;
        if (!pending)
            return;
        if (event.operation_id !== undefined && event.operation_id !== pending.operationId)
            return;
        if (event.operation_id === undefined && event.thread !== pending.thread)
            return;
        this._finishSend(event.success === true, event.message);
    }

    _finishSend(success, message) {
        const pending = this._pending;
        this._pending = null;
        if (success) {
            this._drafts.delete(pending.thread);
            this._replyError = '';
            this._transient = {kind: 'sent', until: this._now() + SENT_PILL_MS};
        } else {
            this._drafts.set(pending.thread, pending.body);
            this._replyError = message || this._('The iPhone did not take the message.');
            this._transient = {
                kind: 'failed',
                thread: pending.thread,
                until: this._now() + NOTICE_PILL_MS,
            };
        }
    }

    // "New Message…" in the menu: look a contact up, pick one, and the reply
    // card opens on that conversation, new or not.
    startCompose() {
        if (this._link() !== 'linked')
            return;
        this._compose = {query: '', results: []};
        this._cardOpen = true;
        this._changed();
    }

    composeQuery(text) {
        const compose = this._compose;
        const query = String(text ?? '').trim();
        if (!compose || query === compose.query)
            return;
        compose.query = query;
        compose.results = this._composeTyped(query);
        if (query)
            this._send({command: 'bt_list_contacts', query, limit: COMPOSE_RESULTS});
        this._changed();
    }

    // A number typed in full can be written to without being a contact.
    _composeTyped(query) {
        if (!/^\+?[\d\s().\-/]{6,}$/.test(query))
            return [];
        const number = `${query.startsWith('+') ? '+' : ''}${digits(query)}`;
        return [{thread: `tel:${number}`, name: formatNumber(number), detail: '', initials: ''}];
    }

    _composeResults(event) {
        const compose = this._compose;
        if (!compose || event.query !== compose.query)
            return;
        const results = this._composeTyped(compose.query);
        for (const contact of Array.isArray(event.contacts) ? event.contacts : []) {
            for (const address of Array.isArray(contact.addresses) ? contact.addresses : []) {
                const plain = String(address).replace(/^(tel|email):/, '');
                const who = sender(contact.name, plain, address);
                if (!results.some(r => r.thread === address)) {
                    results.push({
                        thread: String(address), name: who.name, initials: who.initials,
                        detail: who.name === formatNumber(plain) ? '' : formatNumber(plain),
                    });
                }
            }
        }
        compose.results = results.slice(0, COMPOSE_RESULTS);
    }

    composePick(index) {
        const picked = this._compose?.results[index];
        if (!picked)
            return;
        if (!this._threads.has(picked.thread)) {
            this._threads.set(picked.thread, {
                thread: picked.thread, name: picked.detail ? picked.name : '',
                address: picked.thread.replace(/^(tel|email):/, ''), timestamp: 0, repliable: true,
            });
        }
        this._compose = null;
        this._cardOpen = true;
        this._showThread(picked.thread, null);
        this._changed();
    }

    // A conversation picked in the menu.
    openThread(key) {
        if (!this._threads.has(key))
            return;
        this._cardOpen = true;
        this._showThread(key, null);
        this._changed();
    }

    _showThread(key, message) {
        this._thread = key;
        const fresh = this._fresh.get(key) ?? [];
        this._fresh.delete(key);
        if (message && !fresh.some(m => m.handle === message.handle)) {
            fresh.push({handle: message.handle, body: String(message.body ?? ''),
                timestamp: message.timestamp ?? 0});
        }
        this._threadMessages = fresh.slice(-FRESH_MESSAGES);
        const failed = this._transient?.kind === 'failed';
        if (failed || this._transient?.kind === 'message')
            this._transient = null;
        if (!failed)
            this._replyError = '';
        const all = [...this._unread].filter(([, thread]) => thread === key).map(([h]) => h);
        for (const m of fresh) {
            if (!all.includes(m.handle))
                all.push(m.handle);
        }
        for (const handle of all) {
            this._unread.delete(handle);
            // Known by its notification only: read here, marked on the
            // iPhone once the message itself is there.
            const provisional = this._provisional.get(handle);
            if (provisional)
                provisional.seen = true;
        }
        const handles = all.filter(handle => !handle.startsWith(PROVISIONAL));
        if (handles.length > 0)
            this._send({command: 'bt_mark_read', handles, read: true});
    }

    setDraft(text) {
        if (!this._thread)
            return;
        if (text)
            this._drafts.set(this._thread, text);
        else
            this._drafts.delete(this._thread);
    }

    reply(text) {
        const body = String(text ?? '').trim();
        const thread = this._thread;
        if (!body || !thread || this._pending)
            return false;
        const operationId = `reply-${++this._sequence}`;
        this._send({command: 'bt_send_message', thread, body, operation_id: operationId});
        this._pending = {
            operationId, thread, body, until: this._now() + SEND_TIMEOUT_MS,
            // The classic link is closed: the engine opens it first. Its
            // bt_send_progress says how far it is.
            stage: this._service(FIELD.messages) === SERVICE.onDemand ? SEND_STAGE.connecting : null,
        };
        this._drafts.delete(thread);
        for (const [handle, t] of [...this._unread]) {
            if (t === thread)
                this._unread.delete(handle);
        }
        this._thread = null;
        this._threadMessages = null;
        this._fresh.delete(thread);
        this._cardOpen = false;
        this._transient = null;
        this._changed();
        return true;
    }

    _short(name) {
        return name === this._('Unknown caller') ? name : sender(name).short;
    }

    get unreadCount() {
        return this._unread.size;
    }
};
