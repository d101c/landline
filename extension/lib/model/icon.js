// SPDX-License-Identifier: GPL-2.0-or-later
// The icon of the top bar: which drawing it shows at rest, and for how long
// it moves. The drawings themselves are in iconArt.js.

import {ATTENTION_MS} from './constants.js';
import {FIELD, LINK_MODE, LINK_MODE_REASON, SERVICE} from './protocol.js';

export const withIcon = Base => class extends Base {
    // What the icon says when nothing grows in the top bar. In silence
    // nothing that arrived is told: only how the link is.
    //
    // All is well: the bare drawing of "linked", nothing beside it, and
    // nothing about the link in the menu either. Anything that is not well
    // is a mark on that drawing, one at a time, the first that applies:
    //   1. online through the iPhone while the link is away: 'internet'
    //   2. link away or coming back: 'away' (dimmed), 'reconnecting'
    //      (moving); paused on purpose: 'paused'
    //   3. news, not in silence: 'missed', 'voicemail', 'unread', then the
    //      battery low until the menu is opened: 'battery'
    //   4. notifications paused: 'paused'
    //   5. something to do (_attention()): 'attention'
    //   6. online through the iPhone, or its Internet that did not
    //      connect: 'internet', 'internet_failed'
    //   7. 'linked'
    _iconAtRest(link) {
        const internet = this._internetIcon();
        // Online through the iPhone: it is here, whatever the other link says.
        if (internet === 'internet' && (link === 'away' || link === 'reconnecting'))
            return internet;
        if (link === 'away' || link === 'reconnecting')
            return this._status?.enabled === false ? 'paused' : link;
        if (link !== 'linked')
            return 'bare';
        if (this._hush() === null) {
            if (this._missed.size > 0)
                return 'missed';
            if (this._voicemail.size > 0)
                return 'voicemail';
            if (this._unread.size > 0)
                return 'unread';
            if (this.batteryAlerts && this._batteryLow && !this._batteryLow.seen)
                return 'battery';
        }
        if (this._notificationsPaused())
            return 'paused';
        if (this._attention())
            return 'attention';
        return internet ?? 'linked';
    }

    // While linked, what is still to be done for everything to work, or
    // null; the first that applies, the one the icon marks and the menu
    // says:
    //   'permission'  a Bluetooth setting of this computer is missing, or
    //                 would not survive a restart
    //   'messages'    messages and contacts are not on offer (the system's
    //                 OBEX service, or another computer reading them)
    //   'switches'    a sharing switch is still off on the iPhone
    //   'engine'      the engine is older than the one this version installs
    _attention() {
        const conn = this._conn ?? {};
        if (this._needsPermission())
            return 'permission';
        if (this._obexState() || conn.map_error === 'busy')
            return 'messages';
        if (conn.map_error === 'forbidden' || conn.pbap_error === 'forbidden' ||
            this._service(FIELD.messages) === SERVICE.forbidden ||
            this._service(FIELD.contacts) === SERVICE.forbidden ||
            (this._linkComplete() && !this._allOn()) || this._notificationsRefused())
            return 'switches';
        if (this._engineInstall?.outdated === true)
            return 'engine';
        return null;
    }

    // Notifications pausing is news, like a message: the ring blinks for a
    // while, then stays.
    _iconWatch() {
        const paused = this._notificationsPaused() !== null;
        if (paused && !this._wasPaused)
            this._attentionAt = this._now();
        this._wasPaused = paused;
    }

    // Something new arrived less than a minute ago, and it may be shown.
    _iconMoves(now) {
        return this._hush() === null && this._attentionAt !== null &&
            now < this._attentionAt + ATTENTION_MS;
    }

    // When the icon stops moving by itself, or null: nothing to wait for
    // when what it shows does not move.
    _iconSettlesAt() {
        if (!this._iconMoves(this._now()))
            return null;
        const waits = this._unread.size > 0 || this._voicemail.size > 0 ||
            this._notificationsPaused() !== null;
        return waits ? this._attentionAt + ATTENTION_MS : null;
    }

    // The light mode was chosen and cannot be kept because the iPhone does
    // not share its notifications (yet).
    _notificationsRefused() {
        return this._field(FIELD.linkMode) === LINK_MODE.lean && !this._lean() &&
            this._field(FIELD.linkModeReason) === LINK_MODE_REASON.notificationsOff;
    }
};
