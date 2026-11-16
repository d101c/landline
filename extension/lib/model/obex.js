// SPDX-License-Identifier: GPL-2.0-or-later
// Messages and contacts not on offer.
//
// The engine reaches the iPhone but cannot open messages nor contacts, and
// not because a switch is off there ('forbidden'): this computer is not
// offering those services, so the iPhone shows no switch for them and
// waiting will not help. Restarting the engine does.

import {FIX_TIMEOUT_MS, OBEX_STUCK_MS, RETRY_PROFILES_MS} from './constants.js';

export const withObex = Base => class extends Base {
    // The engine names the fault itself: it lost the session bus, and with
    // it the system service that carries messages and contacts.
    _busClosed() {
        const conn = this._conn;
        return this._engine === 'connected' && !!conn &&
            (conn.map_error === 'bus_closed' || conn.pbap_error === 'bus_closed');
    }

    // Without that word (an older engine), the same fault is recognised by
    // its looks.
    _obexFailing() {
        const conn = this._conn;
        if (this._busClosed())
            return true;
        if (this._engine !== 'connected' || !conn || conn.classic_connected !== true)
            return false;
        if (conn.map_open === true || conn.pbap_open === true || conn.map_held || conn.pbap_held)
            return false;
        const bad = error => typeof error === 'string' && error !== '' &&
            !['none', 'forbidden', 'busy'].includes(error);
        return bad(conn.map_error) && bad(conn.pbap_error) && !this._brokenPairing();
    }

    _obexStuck() {
        if (this._busClosed())
            return true;
        return this._obexSince !== null && this._now() - this._obexSince >= OBEX_STUCK_MS &&
            this._obexFailing();
    }

    // null, 'down' (seen for long enough), 'restarting', 'fixed', 'still'
    // (restarted, no better), 'failed' (could not restart).
    _obexState() {
        if (this._fix)
            return this._fix.state;
        if (!this._obexStuck())
            return null;
        // Back so soon after a restart that looked fine: it did not help.
        return this._fixedAt !== null && this._now() - this._fixedAt < 3 * FIX_TIMEOUT_MS ? 'still' : 'down';
    }

    _obexWatch() {
        const now = this._now();
        const failing = this._obexFailing();
        if (!failing)
            this._obexSince = null;
        else if (this._obexSince === null)
            this._obexSince = now;
        const fix = this._fix;
        if (!fix)
            return;
        const conn = this._conn ?? {};
        const linked = this._engine === 'connected' && conn.classic_connected === true;
        if (conn.map_open === true || conn.pbap_open === true) {
            // A switch came on: nothing left to say.
            this._fix = null;
        } else if (fix.state === 'restarting') {
            if (this._obexStuck())
                fix.state = 'still';
            else if (fix.gone && linked && !failing && this._tour)
                fix.state = 'fixed';
            else if (fix.gone && linked && !failing) {
                this._fix = null;
                this._fixedAt = now;
            }
            else if (now >= fix.until)
                fix.state = this._engine === 'connected' ? 'still' : 'failed';
        } else if (fix.state === 'fixed') {
            if (this._obexStuck())
                fix.state = 'still';
            else if (!this._tour)
                this._fix = null;
        } else if (linked && !failing && !this._tour) {
            this._fix = null;
        }
    }

    // While the step of the switches is on screen and one is still off, the
    // engine is asked every few seconds to try again right now
    // (bt_retry_profiles; an engine that does not know it just says OK).
    _tourRetry() {
        const tour = this._tour;
        if (!tour)
            return;
        const due = tour.step === 'switches' && tour.open && this._engine === 'connected' &&
            this._conn?.classic_connected === true && !this._allOn() && !this._obexState();
        if (!due) {
            tour.retryAt = null;
            return;
        }
        const now = this._now();
        if (tour.retryAt === null || tour.retryAt === undefined || now >= tour.retryAt) {
            tour.retryAt = now + RETRY_PROFILES_MS;
            this._send({command: 'bt_retry_profiles'});
        }
    }

    // "Fix It" was clicked: the engine is being restarted.
    get fixing() {
        return this._fix?.state === 'restarting';
    }

    fixStarted() {
        this._fixedAt = null;
        this._fix = {state: 'restarting', until: this._now() + FIX_TIMEOUT_MS, gone: false};
        this._obexSince = null;
        this._cardOpen = false;
        this._changed();
    }

    // The restart itself did not work.
    fixFailed() {
        if (this._fix?.state !== 'restarting')
            return;
        this._fix.state = 'failed';
        this._changed();
    }
};
