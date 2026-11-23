// SPDX-License-Identifier: GPL-2.0-or-later
// Notifications of the other iPhone apps. Nothing of them reaches the
// notifications of GNOME: a new one grows the pill, and everything that
// waits on the iPhone is listed in the menu ("On your iPhone"). All of it is
// kept in memory only.

import {replyApp, replySender, contactNumber} from '../replyLinks.js';
import {conversationLink, mediaKind} from '../officialApps.js';
import {
    AWAY_SETTLE_MS,
    CATEGORY_INCOMING_CALL,
    CATEGORY_MISSED_CALL,
    CATEGORY_VOICEMAIL,
    EARLIER_KEEP_MS,
    EARLIER_MAX,
    EARLIER_SHOWN,
    MAX_SHOWN_NOTES,
    MESSAGES_APP,
    NOTE_BURST_MS,
    NOTE_PILL_MS,
    PHONE_APP,
    PHONE_NOTES_SHOWN,
    SUMMARY_APPS,
    SUMMARY_PILL_MS,
} from './constants.js';
import {initials} from './format.js';

export const withNotifications = Base => class extends Base {
    _onNotification(n, fromList) {
        if (typeof n.uid !== 'number')
            return;
        // The notification of Messages comes up to 15 s before the message.
        if (!fromList && n.silent !== true && n.app_id === MESSAGES_APP) {
            this._tourProof(n.title, n.body);
            if (this._provisionalFrom(n))
                return;
        }
        this._desktopNotification(n, fromList);
    }

    // force: a message the pill gave up on, which has no other trace.
    _desktopNotification(n, fromList, force = false) {
        if (!force && !this._passesToDesktop(n))
            return;
        // The engine sends everything that is on the iPhone again at each
        // reconnection, sometimes under new uids. What was already there
        // when it connected (silent, or from the list) never reaches the
        // desktop, and what was shown once is not shown twice: both are
        // remembered by their content. One still on the desktop may be
        // corrected (the app name comes in two goes).
        const print = [n.app_id, n.timestamp ?? 0, n.title, n.subtitle, n.body].join('\n');
        const shownAs = this._shownNotes.get(print);
        const old = fromList || n.silent === true;
        this._mirror(n, print);
        // Already there when the iPhone connected, and never seen before: it
        // came while the iPhone was away. Counted for the summary.
        if (old && shownAs === undefined && !this._appQuiet(n)) {
            this._awayNew.add(n.uid);
            this._awayAt = this._now() + AWAY_SETTLE_MS;
        }
        // New, and received live: the only kind the pill may grow for.
        const fresh = !old && shownAs === undefined && !this._liveNotes.has(n.uid);
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
        // A call that was missed, a voicemail left: the icon says so until
        // the menu is opened. Only when new, never when sent again.
        if (shownAs === undefined && n.category === CATEGORY_MISSED_CALL) {
            this._missed.add(n.uid);
            this._attentionAt = this._now();
        } else if (shownAs === undefined && n.category === CATEGORY_VOICEMAIL) {
            this._voicemail.add(n.uid);
            this._attentionAt = this._now();
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
        // One the message pill gave up on was already shown once.
        if (fresh && !force)
            this._announce(n);
    }

    // --- what waits on the iPhone ------------------------------------------

    // Everything the iPhone says it holds right now, by uid. The engine may
    // send the same one again under another uid: the newer takes its place.
    _mirror(n, print) {
        const twin = this._notePrints.get(print);
        if (twin !== undefined && twin !== n.uid)
            this._phoneNotes.delete(twin);
        for (const [key, uid] of [...this._notePrints]) {
            if (uid === n.uid && key !== print)
                this._notePrints.delete(key);
        }
        this._notePrints.set(print, n.uid);
        const known = this._phoneNotes.get(n.uid);
        const appName = n.app_name || n.app_id || this._('iPhone');
        this._phoneNotes.set(n.uid, {
            uid: n.uid,
            print,
            appId: n.app_id || 'unknown',
            appName,
            title: n.title || '',
            body: [n.subtitle, n.body].filter(s => s).join('\n'),
            // Seconds. One that does not say when counts from now.
            timestamp: n.timestamp || known?.timestamp || Math.floor(this._now() / 1000),
            category: n.category ?? 0,
            canClear: n.negative_action === true,
            // The sender's number, once it is known for sure (_replyOffer()).
            replyNumber: known?.replyNumber ?? null,
            // A voice message, a photo…: what Bluetooth did not carry.
            media: mediaKind(n.body),
        });
        this._remember(this._phoneNotes.get(n.uid));
    }

    // --- "Earlier": what was on the iPhone, in memory only --------------------

    // Off by default. Turned off, everything kept is dropped at once.
    setKeepHistory(keep) {
        this.keepHistory = keep === true;
        if (!this.keepHistory)
            this._earlier.clear();
        this._changed();
    }

    // "Clear history".
    clearHistory() {
        this._earlier.clear();
        this._changed();
    }

    // Remembered by its fingerprint, the latest seen last. Never written
    // anywhere: it lives in this object and goes with it.
    _remember(note) {
        if (!this.keepHistory)
            return;
        const now = this._now();
        this._earlier.delete(note.print);
        this._earlier.set(note.print, {
            appId: note.appId, appName: note.appName, title: note.title, body: note.body, timestamp: note.timestamp, at: now,
        });
        for (const [print, entry] of this._earlier) {
            if (this._earlier.size <= EARLIER_MAX && now < entry.at + EARLIER_KEEP_MS)
                break;
            this._earlier.delete(print);
        }
    }

    // What "Earlier" lists: the ones that are no longer on the iPhone, the
    // latest first. null when history is not kept.
    _earlierView() {
        if (!this.keepHistory)
            return null;
        const now = this._now();
        return [...this._earlier]
            .filter(([print, entry]) => !this._notePrints.has(print) && now < entry.at + EARLIER_KEEP_MS)
            .map(([, entry]) => ({
                appId: entry.appId,
                appName: entry.appName,
                initials: initials(entry.appName).slice(0, 2),
                title: entry.title && entry.title !== entry.appName ? entry.title : '',
                body: entry.body,
                timestamp: entry.timestamp,
            }))
            .sort((a, b) => b.timestamp - a.timestamp)
            .slice(0, EARLIER_SHOWN);
    }

    _unmirror(uid) {
        const note = this._phoneNotes.get(uid);
        if (!note)
            return;
        this._phoneNotes.delete(uid);
        if (this._notePrints.get(note.print) === uid)
            this._notePrints.delete(note.print);
        if (this._noteCard === uid)
            this._noteCard = null;
    }

    // The apps of this list never grow the pill: they are only listed in
    // the menu. Told by name or by identifier, whatever the case.
    setQuietApps(names) {
        this._quietApps = new Set((names ?? []).map(s => String(s).trim().toLowerCase()).filter(s => s));
    }

    _appQuiet(n) {
        return this._quietApps.has(String(n.app_name ?? '').toLowerCase()) ||
            this._quietApps.has(String(n.app_id ?? '').toLowerCase());
    }

    // A new notification, received live: the pill grows for it, as for a
    // message, unless something that matters more is in the top bar. A call
    // comes first, then a message, then this. Several within a few seconds
    // share one pill.
    _announce(n) {
        // A missed call, a voicemail: the icon says so, and the menu.
        if (n.category === CATEGORY_MISSED_CALL || n.category === CATEGORY_VOICEMAIL)
            return;
        if (this._appQuiet(n) || this._doubled(n) || this._hush() || this._busy() || this._cardOpen || this._pending)
            return;
        const now = this._now();
        const t = this._transient;
        if (t && t.kind !== 'note' && t.kind !== 'summary')
            return;
        if (t?.kind === 'note' && now < t.since + NOTE_BURST_MS) {
            t.uids.push(n.uid);
            t.until = now + NOTE_PILL_MS;
            return;
        }
        this._transient = {kind: 'note', uids: [n.uid], since: now, until: now + NOTE_PILL_MS};
    }

    // The iPhone is back, or the Shell just started, and notifications were
    // waiting: said once, in one pill, without playing each of them again.
    // Nothing in silence, nothing over a call, a message or an open card.
    _awaySummary() {
        if (this._awayAt === null || this._now() < this._awayAt)
            return;
        const counts = new Map();
        // The identifier of each app named, for its icon.
        const ids = new Map();
        for (const uid of this._awayNew) {
            const note = this._phoneNotes.get(uid);
            if (note) {
                counts.set(note.appName, (counts.get(note.appName) ?? 0) + 1);
                if (!ids.has(note.appName))
                    ids.set(note.appName, note.appId);
            }
        }
        this._awayNew.clear();
        this._awayAt = null;
        if (counts.size === 0 || this._hush() || this._busy() || this._cardOpen || this._pending || this._transient)
            return;
        // The apps with the most notifications first.
        const apps = [...counts].sort((a, b) => b[1] - a[1]);
        this._transient = {
            kind: 'summary',
            apps,
            appIds: apps.slice(0, SUMMARY_APPS).map(([name]) => ids.get(name)),
            until: this._now() + SUMMARY_PILL_MS,
        };
    }

    // "5 WhatsApp, 2 Mail and 3 more": three apps at most, then how many
    // notifications are left.
    _summaryText(apps) {
        const named = apps.slice(0, SUMMARY_APPS).map(([name, count]) => `${count} ${name}`).join(', ');
        const rest = apps.slice(SUMMARY_APPS).reduce((sum, [, count]) => sum + count, 0);
        // Translators: %s is a list like "5 WhatsApp, 2 Mail", %d a number of notifications.
        return rest > 0 ? this._('%s and %d more').replace('%s', named).replace('%d', rest) : named;
    }

    // The notifications the pill is showing that are still on the iPhone.
    _pillNotes() {
        const t = this._transient;
        return t?.kind === 'note' ? t.uids.map(uid => this._phoneNotes.get(uid)).filter(note => note) : [];
    }

    // How one notification is shown, in the pill, in its card and in the menu.
    _noteView(note) {
        return {
            uid: note.uid,
            appId: note.appId,
            appName: note.appName,
            initials: initials(note.appName).slice(0, 2),
            title: note.title && note.title !== note.appName ? note.title : '',
            body: note.body,
            timestamp: note.timestamp,
            canClear: note.canClear,
            handoff: this._handoff(note),
        };
    }

    // --- the official app of this computer (#49) --------------------------------

    // How the card hands the notification over to the official app of its
    // service, or null for an app that has none listed:
    //   {label, open, conversation, media}
    // label: "Open in Signal", "Open in WhatsApp Web", "Open Discord in the
    // Browser", '' when nothing here opens it; open: {uri} or {app} (a
    // desktop file id), null with label ''; conversation: open goes to the
    // sender's conversation; media: "Voice message — play it in Signal".
    _handoff(note) {
        const route = this.appRoute?.(note.appId) ?? null;
        if (!route)
            return null;
        const _ = this._;
        const uri = conversationLink(note.appId, route, note.replyNumber);
        let label = '';
        let open = null;
        if (route.app) {
            label = _('Open in %s').replace('%s', route.via);
            open = uri ? {uri} : {app: route.app};
        } else if (route.web) {
            label = route.webName ? _('Open in %s').replace('%s', route.webName)
                // Translators: %s is the name of an app, like Discord.
                : _('Open %s in the Browser').replace('%s', route.service);
            open = {uri: uri ?? route.home};
        }
        return {label, open, conversation: !!uri, media: this._mediaText(note.media, route)};
    }

    // "Voice message — play it in Signal", or on the iPhone when nothing
    // here opens it.
    _mediaText(kind, route) {
        if (!kind)
            return '';
        const _ = this._;
        const where = route.missing ? null : route.webName ?? route.via;
        const texts = where ? {
            // Translators: %s is the name of an app, like Signal or WhatsApp Web.
            voice: _('Voice message — play it in %s'),
            photo: _('Photo — see it in %s'),
            video: _('Video — watch it in %s'),
            file: _('Attachment — open it in %s'),
            sticker: _('Sticker — see it in %s'),
        } : {
            voice: _('Voice message — play it on the iPhone'),
            photo: _('Photo — see it on the iPhone'),
            video: _('Video — watch it on the iPhone'),
            file: _('Attachment — open it on the iPhone'),
            sticker: _('Sticker — see it on the iPhone'),
        };
        return (texts[kind] ?? '').replace('%s', where ?? '');
    }

    // "Don't double": the desktop app of that service is open here and
    // shows its own notifications.
    _doubled(n) {
        return this.skipRunningApps && this.appRunning?.(n.app_id) === true;
    }

    // The extension found the apps again (installed, removed, another
    // default): the cards say it at once.
    officialAppsChanged() {
        this._changed();
    }

    // From Phone Settings: the services already suggested once.
    setSuggestedApps(keys) {
        this._suggested = new Set((keys ?? []).map(String));
    }

    // The card of a notification opens: when the official app of its
    // service is not on this computer, a line says so, once per service
    // ever. Remembered at once, through onSetting.
    _offerSuggestion(uid) {
        this._suggestion = null;
        const note = this._phoneNotes.get(uid);
        const route = note ? this.appRoute?.(note.appId) : null;
        if (!this.suggestApps || !route?.get || route.app || this._suggested.has(route.key))
            return;
        this._suggestion = {uid, key: route.key};
        this._suggested.add(route.key);
        this.onSetting?.('suggested-apps', [...this._suggested]);
    }

    // {text, get, url} for the card of that notification, or null.
    _suggestionView(uid) {
        const s = this._suggestion;
        const note = s?.uid === uid ? this._phoneNotes.get(uid) : null;
        const route = note ? this.appRoute?.(note.appId) : null;
        if (!route?.get || !this.suggestApps)
            return null;
        const _ = this._;
        return {
            // Translators: %s is the name of an app, like Signal Desktop.
            text: _('%s isn’t on this computer. The official app shows voice messages and media here.')
                .replace('%s', route.get.name),
            // Translators: %s is the name of an app, like Signal Desktop.
            get: _('Get %s').replace('%s', route.get.name),
            url: route.get.url,
        };
    }

    // "Don't Suggest Apps".
    stopSuggesting() {
        this.suggestApps = false;
        this._suggestion = null;
        this.onSetting?.('suggest-apps', false);
        this._changed();
    }

    // "On your iPhone": what waits there right now, the latest first, by
    // app; the app with the latest notification comes first. null when
    // nothing waits. Only the first few unless "Show all" was asked.
    _phoneView() {
        const notes = [...this._phoneNotes.values()]
            .sort((a, b) => b.timestamp - a.timestamp || b.uid - a.uid);
        if (notes.length === 0)
            return null;
        const shown = this._showAllNotes ? notes : notes.slice(0, PHONE_NOTES_SHOWN);
        const groups = new Map();
        for (const note of shown) {
            const view = this._noteView(note);
            const group = groups.get(note.appId) ??
                {appId: note.appId, appName: note.appName, initials: view.initials, notes: []};
            group.notes.push(view);
            groups.set(note.appId, group);
        }
        return {
            total: notes.length,
            hidden: notes.length - shown.length,
            clearable: notes.some(note => note.canClear),
            groups: [...groups.values()],
        };
    }

    // "Show all", until the menu closes.
    showAllNotes() {
        this._showAllNotes = true;
        this._changed();
    }

    // "Clear All": everything the iPhone lets be cleared goes, there too.
    clearAllNotifications() {
        for (const note of [...this._phoneNotes.values()]) {
            if (!note.canClear)
                continue;
            this._liveNotes.delete(note.uid);
            this._missed.delete(note.uid);
            this._voicemail.delete(note.uid);
            this._send({command: 'bt_notification_action', uid: note.uid, action: 'negative'});
            this._unmirror(note.uid);
        }
        this._changed();
    }

    // A line of the menu was picked: its card.
    openNote(uid) {
        if (!this._phoneNotes.has(uid))
            return;
        this._cardOpen = true;
        this._noteCard = uid;
        this._offerSuggestion(uid);
        if (this._transient?.kind === 'note' || this._transient?.kind === 'summary')
            this._transient = null;
        this._changed();
    }

    // The sender of that notification is known for sure: its card can open
    // the conversation.
    _sureSender(offer) {
        const note = this._phoneNotes.get(offer.uid);
        if (note && note.replyNumber !== offer.number) {
            note.replyNumber = offer.number;
            this._changed();
        }
        this.onNotification?.('reply', offer);
    }

    // The conversation of a notification of WhatsApp, Telegram or Signal is
    // opened only when the sender is one person whose number is known for
    // certain: written in the notification itself, or the only number of
    // the only contact of that name. Never for a group, never on a guess.
    _replyOffer(n) {
        if (!replyApp(n.app_id))
            return;
        const who = replySender(n.title, n.subtitle);
        if (!who)
            return;
        const offer = {uid: n.uid, appId: n.app_id};
        if (who.number) {
            this._sureSender({...offer, number: who.number});
            return;
        }
        const key = who.name.toLowerCase();
        if (this._replyNumbers.has(key)) {
            const number = this._replyNumbers.get(key);
            if (number)
                this._sureSender({...offer, number});
            return;
        }
        // The contacts must be readable to look the name up.
        if (!this._contactsUsable())
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
            this._sureSender({...offer, number});
    }

    _onNotificationList(event) {
        // The full list: what is no longer in it goes from the desktop;
        // nothing in it is new, so nothing is added.
        const list = Array.isArray(event.notifications) ? event.notifications : [];
        for (const n of list)
            this._onNotification(n, true);
        const uids = list.filter(n => this._passesToDesktop(n)).map(n => n.uid);
        for (const uid of [...this._phoneNotes.keys()]) {
            if (!uids.includes(uid))
                this._unmirror(uid);
        }
        this._liveNotes = new Set(uids.filter(uid => this._liveNotes.has(uid)));
        this.onNotification?.('reset', {uids});
    }

    // Messages and calls have their own pill and their own cards. Their
    // notifications are only handled here when those cannot show them:
    // messages not readable, or call control off. A missed call and a
    // voicemail always are: the icon only says there is one, not from whom.
    _passesToDesktop(n) {
        if (n.app_id === MESSAGES_APP)
            return !this._messagesUsable();
        if (n.category === CATEGORY_MISSED_CALL || n.category === CATEGORY_VOICEMAIL)
            return true;
        if (n.app_id === PHONE_APP || n.category === CATEGORY_INCOMING_CALL)
            return !this._conn?.calls;
        return true;
    }

    // "Dismiss": gone from here, and cleared on the iPhone when the iPhone
    // lets it be.
    clearNotification(uid) {
        const note = this._phoneNotes.get(uid);
        if (!note)
            return;
        this._liveNotes.delete(uid);
        this._missed.delete(uid);
        this._voicemail.delete(uid);
        if (note.canClear)
            this._send({command: 'bt_notification_action', uid, action: 'negative'});
        this._unmirror(uid);
        this._changed();
    }
};
