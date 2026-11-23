// SPDX-License-Identifier: GPL-2.0-or-later
// The classic link on demand.
//
// A recent engine keeps one link up, the one that carries notifications,
// and opens the other (messages, contacts, calls) only when something needs
// it. It then says so in its state: the mode in force, what can be expected
// of messages, contacts and calls, and whether notifications are arriving.
// Everything here reads those words; with an engine that does not send them
// each reader answers null and the older rules apply.

import {BATTERY_KEEP_MS, LEASE_RENEW_MS} from './constants.js';
import {
    CALLS, COMMAND, FIELD, LEASE, LE_CAUSE, LE_STATE, LINK_MODE, SERVICE,
} from './protocol.js';

export const withDemand = Base => class extends Base {
    // A field of the published state: the connection says it first, the
    // status as well. null when the engine does not publish it.
    _field(name) {
        for (const source of [this._conn, this._status]) {
            const value = source?.[name];
            if (value !== undefined && value !== null)
                return value;
        }
        return null;
    }

    // The engine knows about the two modes.
    _demandAware() {
        return typeof this._field(FIELD.linkModeEffective) === 'string' ||
            typeof this._field(FIELD.linkMode) === 'string';
    }

    // The mode in force is the light one: the notification link alone says
    // whether the iPhone is here.
    _lean() {
        return this._field(FIELD.linkModeEffective) === LINK_MODE.lean;
    }

    _linked() {
        if (this._lean())
            return this._conn?.le_connected === true || this._classicCarriesNotifications();
        return this._linkedEither();
    }

    // Lean mode, the LE link gone, the classic link still up and the
    // notifications live over it (#43, 18 November 18:18): the iPhone is
    // linked, whatever the notification link says. The engine closes the
    // classic link after a while for the LE link to come back.
    _classicCarriesNotifications() {
        const conn = this._conn;
        return this._lean() && conn?.le_connected !== true && conn?.classic_connected === true &&
            conn?.ancs_ready === true;
    }

    // 'ready', 'on_demand', 'forbidden', 'unavailable', 'unknown', or null.
    _service(field) {
        const value = this._conn?.[field];
        return typeof value === 'string' ? value : null;
    }

    _usable(field, open) {
        const state = this._service(field);
        if (state === null)
            return open === true;
        return state === SERVICE.ready || state === SERVICE.onDemand;
    }

    // Messages can be read and sent, now or after a short connection.
    _messagesUsable() {
        return this._usable(FIELD.messages, this._conn?.map_open);
    }

    _contactsUsable() {
        return this._usable(FIELD.contacts, this._conn?.pbap_open);
    }

    // 'ready', 'on_demand', 'control_only', 'off', or null.
    _callsState() {
        const value = this._conn?.[FIELD.callsAvailable];
        return typeof value === 'string' ? value : null;
    }

    // A call can be asked for from here: call control is on. Whether the
    // hands-free link is there this instant does not decide it: "Call…" must
    // not come and go with it, and a call that cannot be placed says why.
    _callsUsable() {
        const state = this._callsState();
        if (state === null)
            return !!this._conn?.calls;
        return state !== CALLS.off;
    }

    // --- leases ------------------------------------------------------------------

    // While the setup dialog is up, or a conversation is being read or
    // written, the engine is asked to keep the classic link open. A lease
    // lapses by itself in the engine: it is renewed while it is wanted.
    _leaseWanted(name) {
        if (name === LEASE.setup)
            return this._tour?.open === true && this._tour.step !== 'forget';
        return this._cardOpen && (this._thread !== null || this._compose !== null);
    }

    _syncLeases() {
        const asked = this._engine === 'connected' &&
            (this._lean() || this._field(FIELD.linkMode) === LINK_MODE.lean);
        if (!asked) {
            this._leases.clear();
            return;
        }
        const now = this._now();
        for (const name of [LEASE.setup, LEASE.reading]) {
            const wanted = this._leaseWanted(name);
            const renewAt = this._leases.get(name);
            if (wanted && (renewAt === undefined || now >= renewAt)) {
                this._leases.set(name, now + LEASE_RENEW_MS);
                this._send({command: COMMAND.lease, name, held: true});
            } else if (!wanted && renewAt !== undefined) {
                this._leases.delete(name);
                this._send({command: COMMAND.lease, name, held: false});
            }
        }
    }

    // --- the mode, as Phone Settings shows it -----------------------------------

    // '' with an engine that has one mode only; else "<mode in force>:<why
    // it is not the one chosen>", the reason empty when it is.
    _linkModeState() {
        if (!this._demandAware())
            return '';
        const effective = this._field(FIELD.linkModeEffective) ?? this._field(FIELD.linkMode);
        const reason = this._field(FIELD.linkModeReason) ?? '';
        return `${effective}:${effective === this._field(FIELD.linkMode) ? '' : reason}`;
    }

    // --- notifications that do not arrive ---------------------------------------

    // null, or {cause, text, remedy, onPhone}: the notification link has been down
    // for long enough that the engine calls it a pause.
    _notificationsPaused() {
        const conn = this._conn;
        if (!conn || conn[FIELD.leState] !== LE_STATE.paused || this._classicCarriesNotifications())
            return null;
        const _ = this._;
        const cause = String(conn[FIELD.leCause] ?? '');
        const known = {
            [LE_CAUSE.afterResume]: [
                _('This computer woke up and the iPhone has not come back for notifications yet.'),
                _('Unlock your iPhone. If nothing changes, turn Bluetooth off and on again on your iPhone.'), true],
            [LE_CAUSE.phoneSilent]: [
                _('The iPhone is within reach but has stopped sending notifications.'),
                _('Turn Bluetooth off and on again on your iPhone.'), true],
            [LE_CAUSE.phoneAway]: [
                _('The iPhone is out of range, or its Bluetooth is off.'),
                _('They resume by themselves when it is back.'), false],
            [LE_CAUSE.unresolvedAddress]: [
                _('An iPhone answered, but this computer did not recognise it as yours.'),
                _('Turn Bluetooth off and on again on your iPhone. If that does not help, forget the iPhone in Phone Settings and link it again.'), true],
            [LE_CAUSE.advertOffAir]: [
                _('This computer has stopped announcing itself to the iPhone.'),
                _('Turn Bluetooth off and on again on this computer.'), false],
        }[cause];
        // onPhone: the gesture is made in the Bluetooth settings of the
        // iPhone; the card says where they are, in the iPhone's own words.
        if (known)
            return {cause, text: known[0], remedy: known[1], onPhone: known[2]};
        // A cause from a newer engine: its own sentence, as it wrote it.
        return {
            cause,
            text: String(conn[FIELD.leDetail] ?? '') || _('The iPhone is not sending notifications right now.'),
            remedy: _('Turn Bluetooth off and on again on your iPhone.'),
            onPhone: true,
        };
    }

    // --- battery ------------------------------------------------------------------

    // The level comes over the notification link and goes when it does. The
    // last one known is kept, to be shown as such.
    _noteBattery(conn) {
        if (typeof conn.battery === 'number')
            this._lastBattery = {level: conn.battery, at: this._now()};
        else if (conn.device_paired === false || conn.device_present === false)
            this._lastBattery = null;
    }

    // {level, stale, minutes} or null. A stale level is only shown with an
    // engine that says how the notification link is doing.
    _batteryView(link) {
        const conn = this._conn ?? {};
        if (link === 'linked' && typeof conn.battery === 'number')
            return {level: conn.battery, stale: false, minutes: 0};
        const last = this._lastBattery;
        const age = last ? this._now() - last.at : 0;
        if (!last || typeof conn[FIELD.leState] !== 'string' || !['linked', 'away', 'reconnecting'].includes(link) ||
            age >= BATTERY_KEEP_MS)
            return null;
        return {level: last.level, stale: true, minutes: Math.floor(age / 60000)};
    }

    // The level is low: as the engine says (battery_state, 20 % or less),
    // else by the same threshold.
    _batteryIsLow(level) {
        const state = this._conn?.battery_state;
        if (typeof state === 'string')
            return state === 'low' || state === 'critical';
        return level <= 20;
    }
};
