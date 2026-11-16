// SPDX-License-Identifier: GPL-2.0-or-later
// Notifications of the other iPhone apps, as ordinary desktop notifications:
// one source per app, named after it. Dismissing one here clears it on the
// iPhone; one cleared on the iPhone disappears here.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {LOG_PREFIX} from './names.js';

export class NotificationBridge {
    // onDismissed(uid): the user dismissed that notification by hand.
    // replyLabel: the words of the "Reply in app" action.
    constructor({onDismissed, replyLabel}) {
        this._onDismissed = onDismissed;
        this._replyLabel = replyLabel ?? 'Reply in app';
        this._sources = new Map();        // app id -> MessageTray.Source
        this._notifications = new Map();  // uid -> {notification, canClear}
        this._quiet = false;
    }

    handle(kind, payload) {
        if (kind === 'show')
            this._show(payload);
        else if (kind === 'remove')
            this._remove(payload.uid);
        else if (kind === 'reset')
            this._keepOnly(new Set(payload.uids));
        else if (kind === 'reply')
            this._offerReply(payload);
    }

    // payload: {uid, uri, scheme}. The action is only added when something
    // on this computer opens that kind of link (a web link always does).
    _offerReply({uid, uri, scheme}) {
        const entry = this._notifications.get(uid);
        if (!entry || entry.replyUri || !uri)
            return;
        if (scheme !== 'https' && !Gio.AppInfo.get_default_for_uri_scheme(scheme))
            return;
        entry.replyUri = uri;
        entry.notification.addAction(this._replyLabel, () => {
            // Opening the conversation is not dismissing the notification:
            // nothing is cleared on the iPhone.
            entry.activated = true;
            try {
                Gio.AppInfo.launch_default_for_uri(uri, global.create_app_launch_context(0, -1));
            } catch (e) {
                console.warn(`${LOG_PREFIX}: could not open the app to reply: ${e.message}`);
            }
        });
    }

    _source(appId, appName) {
        let source = this._sources.get(appId);
        if (source) {
            // The engine first guesses the app name, then corrects it.
            if (source.title !== appName)
                source.title = appName;
            return source;
        }
        source = new MessageTray.Source({title: appName, iconName: 'phone-symbolic'});
        source.connect('destroy', () => {
            if (this._sources.get(appId) === source)
                this._sources.delete(appId);
        });
        this._sources.set(appId, source);
        Main.messageTray.add(source);
        return source;
    }

    _show(n) {
        const known = this._notifications.get(n.uid);
        if (known) {
            known.canClear = n.canClear;
            known.notification.set({title: n.title, body: n.body});
            this._source(n.appId, n.appName);
            return;
        }
        const source = this._source(n.appId, n.appName);
        const params = {
            source,
            title: n.title,
            body: n.body,
            // Already on the iPhone when it connected: listed, not popped up.
            urgency: n.silent ? MessageTray.Urgency.LOW : MessageTray.Urgency.NORMAL,
        };
        if (n.timestamp > 0)
            params.datetime = GLib.DateTime.new_from_unix_local(n.timestamp);
        const notification = new MessageTray.Notification(params);
        const entry = {notification, canClear: n.canClear, activated: false};
        notification.connect('activated', () => {
            entry.activated = true;
        });
        notification.connect('destroy', (_n, reason) => {
            this._notifications.delete(n.uid);
            if (this._quiet || entry.activated || !entry.canClear)
                return;
            if (reason === MessageTray.NotificationDestroyedReason.DISMISSED)
                this._onDismissed?.(n.uid);
        });
        this._notifications.set(n.uid, entry);
        source.addNotification(notification);
    }

    // Something of our own to say, quietly: in the list of GNOME, with a
    // banner unless Do Not Disturb is on. One at a time: a newer one
    // replaces the previous.
    notice(title, body) {
        const source = this._source('phone', 'iPhone');
        this._notice?.destroy(MessageTray.NotificationDestroyedReason.REPLACED);
        const notification = new MessageTray.Notification({
            source, title, body,
            urgency: MessageTray.Urgency.NORMAL,
        });
        notification.connect('destroy', () => {
            if (this._notice === notification)
                this._notice = null;
        });
        this._notice = notification;
        source.addNotification(notification);
    }

    _remove(uid) {
        const entry = this._notifications.get(uid);
        if (!entry)
            return;
        this._quiet = true;
        entry.notification.destroy(MessageTray.NotificationDestroyedReason.SOURCE_CLOSED);
        this._quiet = false;
    }

    _keepOnly(uids) {
        for (const uid of [...this._notifications.keys()]) {
            if (!uids.has(uid))
                this._remove(uid);
        }
    }

    destroy() {
        this._keepOnly(new Set());
        this._quiet = true;
        for (const source of [...this._sources.values()])
            source.destroy();
        this._sources.clear();
        this._onDismissed = null;
    }
}
