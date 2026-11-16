// SPDX-License-Identifier: GPL-2.0-or-later
// Notifications of the other iPhone apps, on their way to the desktop.

import {replyApp, replySender, contactNumber} from '../replyLinks.js';
import {
    CATEGORY_INCOMING_CALL,
    CATEGORY_MISSED_CALL,
    MAX_SHOWN_NOTES,
    MESSAGES_APP,
    PHONE_APP,
} from './constants.js';

export const withNotifications = Base => class extends Base {
    _onNotification(n, fromList) {
        if (typeof n.uid !== 'number')
            return;
        // The notification of Messages comes up to 15 s before the message.
        if (!fromList && n.silent !== true && n.app_id === MESSAGES_APP)
            this._tourProof(n.title, n.body);
        if (!this._passesToDesktop(n))
            return;
        // The engine sends everything that is on the iPhone again at each
        // reconnection, sometimes under new uids. What was already there
        // when it connected (silent, or from the list) never reaches the
        // desktop, and what was shown once is not shown twice: both are
        // remembered by their content. One still on the desktop may be
        // corrected (the app name comes in two goes).
        const print = [n.app_id, n.timestamp ?? 0, n.title, n.subtitle, n.body].join('\n');
        const shownAs = this._shownNotes.get(print);
        if (fromList || n.silent === true) {
            if (shownAs === undefined)
                this._shownNotes.set(print, -1);
            return;
        }
        if (shownAs !== undefined && !(shownAs === n.uid && this._liveNotes.has(n.uid)))
            return;
        if (shownAs === undefined && this._liveNotes.has(n.uid)) {
            // The same notification with its texts corrected.
            for (const [key, uid] of [...this._shownNotes]) {
                if (uid === n.uid)
                    this._shownNotes.delete(key);
            }
        }
        this._shownNotes.set(print, n.uid);
        if (this._shownNotes.size > MAX_SHOWN_NOTES)
            this._shownNotes = new Map([...this._shownNotes].slice(MAX_SHOWN_NOTES / 2));
        this._liveNotes.add(n.uid);
        this.onNotification?.('show', {
            uid: n.uid,
            appId: n.app_id || 'unknown',
            appName: n.app_name || n.app_id || this._('iPhone'),
            title: n.title || n.app_name || this._('iPhone'),
            body: [n.subtitle, n.body].filter(s => s).join('\n'),
            timestamp: n.timestamp ?? 0,
            // In silence it is listed without a banner.
            silent: this._hush() !== null,
            canClear: n.negative_action === true,
        });
        this._replyOffer(n);
    }

    // "Reply in app" on a notification of WhatsApp, Telegram or Signal, when
    // the sender is one person whose number is known for certain: written in
    // the notification itself, or the only number of the only contact of
    // that name. Never for a group, never on a guess.
    _replyOffer(n) {
        if (!replyApp(n.app_id))
            return;
        const who = replySender(n.title, n.subtitle);
        if (!who)
            return;
        const offer = {uid: n.uid, appId: n.app_id};
        if (who.number) {
            this.onNotification?.('reply', {...offer, number: who.number});
            return;
        }
        const key = who.name.toLowerCase();
        if (this._replyNumbers.has(key)) {
            const number = this._replyNumbers.get(key);
            if (number)
                this.onNotification?.('reply', {...offer, number});
            return;
        }
        // The contacts must be readable to look the name up.
        if (this._conn?.pbap_open !== true)
            return;
        const waiting = this._replyWaiting.get(key);
        if (waiting) {
            waiting.push(offer);
            return;
        }
        this._replyWaiting.set(key, [offer]);
        this._send({command: 'bt_list_contacts', query: who.name, limit: 5});
    }

    _replyContacts(event) {
        const key = String(event.query ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
        const waiting = this._replyWaiting.get(key);
        if (!waiting)
            return;
        this._replyWaiting.delete(key);
        const number = contactNumber(key, event.contacts);
        this._replyNumbers.set(key, number);
        if (!number)
            return;
        for (const offer of waiting)
            this.onNotification?.('reply', {...offer, number});
    }

    _onNotificationList(event) {
        // The full list: what is no longer in it goes from the desktop;
        // nothing in it is new, so nothing is added.
        const list = Array.isArray(event.notifications) ? event.notifications : [];
        for (const n of list)
            this._onNotification(n, true);
        const uids = list.filter(n => this._passesToDesktop(n)).map(n => n.uid);
        this._liveNotes = new Set(uids.filter(uid => this._liveNotes.has(uid)));
        this.onNotification?.('reset', {uids});
    }

    // Messages and calls belong to the pill. Their notifications only reach
    // the desktop when the pill cannot show them: messages not readable, or
    // call control off. A missed call always does, the pill has no trace of it.
    _passesToDesktop(n) {
        if (n.app_id === MESSAGES_APP)
            return this._conn?.map_open !== true;
        if (n.category === CATEGORY_MISSED_CALL)
            return true;
        if (n.app_id === PHONE_APP || n.category === CATEGORY_INCOMING_CALL)
            return !this._conn?.calls;
        return true;
    }

    // A desktop notification was dismissed by hand: clear it on the iPhone.
    clearNotification(uid) {
        this._liveNotes.delete(uid);
        this._send({command: 'bt_notification_action', uid, action: 'negative'});
    }
};
