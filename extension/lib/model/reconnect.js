// SPDX-License-Identifier: GPL-2.0-or-later
// "Reconnecting": the state between away and linked.
//
// 18 November, on the way back home: the iPhone tried to link several times
// over three minutes, and the top bar only said "not linked". Something is
// under way when the iPhone is seen without a useful link, when a link is up
// and the iPhone has not served it yet, when links keep failing and the
// engine calls the iPhone again, and right after this computer woke up.
// Never when Bluetooth is off on the iPhone, nor when it has been out of
// range for a while without any sign.
//
// reconnectReason() decides from facts alone; the model gathers the facts
// (_reconnectFacts()), caps how long it is said (RECONNECT_MAX_MS) and the
// indicator shows it through _settle() like any other state.

import {
    RECONNECT_CALM_MS, RECONNECT_HOLD_MS, RECONNECT_MAX_MS, RECONNECT_SETUP_MS, RECONNECT_SLEEP_MS,
} from './constants.js';
import {FIELD, LE_CAUSE, LE_STATE} from './protocol.js';

// What is going on, in the words of the model:
//   'back'        the iPhone is seen, no useful link yet
//   'setting_up'  a link is up again, the iPhone has not served it yet
//   'retrying'    the engine says links died unused and it calls again
//   'dropping'    links come up and die unused (seen here, not said)
//   'sleep'       this computer just woke up
export const RECONNECT_REASONS = ['back', 'setting_up', 'retrying', 'dropping', 'sleep'];
// Said by the engine itself: they outweigh the "Link again" latch of links
// that keep dropping. The others are guesses, and give way to it.
export const ENGINE_REASONS = ['retrying', 'sleep'];

// facts:
//   link       the link state this instant (_link()): only 'away' and
//              'linked' may become reconnecting
//   confirmed  linked and one service really working (_linkConfirmed())
//   shown      what the indicator shows now
//   presence   the last bt_presence, or null
//   leState    le_state, leCause le_cause (null with an older engine)
//   upFor      ms since the useful link came up, or null when it is down
//   signAgo    ms since the last sign of the iPhone, or null
//   fails      links that died unused within RECONNECT_HOLD_MS
//   tour       the setup tour is under way (it says itself how links come)
//   enabled    false when notifications are paused in the engine (the
//              menu says Paused, nothing is under way)
export function reconnectReason(facts) {
    const {link, presence: p, leState, leCause} = facts;
    if (link !== 'away' && link !== 'linked')
        return null;
    // Paused on purpose: the menu says so, the icon must not move (#42).
    if (facts.enabled === false)
        return null;
    // Turned off on purpose, or this computer's Bluetooth: nothing to wait for.
    if (p?.state === 'away' && (p.reason === 'switched_off' || p.reason === 'bluetooth_off'))
        return null;
    if (link === 'linked') {
        // Only on the way back: a served link that blinks stays linked, and
        // a link just paired is the tour's business.
        if (facts.confirmed || facts.tour || !['away', 'reconnecting'].includes(facts.shown))
            return null;
        return facts.upFor !== null && facts.upFor < RECONNECT_SETUP_MS ? 'setting_up' : null;
    }
    // The engine has waited long enough to call it a pause: the menu says
    // why, nothing is under way.
    if (leState === LE_STATE.paused)
        return null;
    if (leCause === LE_CAUSE.holdingOff)
        return 'retrying';
    if (leState === LE_STATE.waiting && leCause === LE_CAUSE.afterResume)
        return 'sleep';
    const recent = (ago, ms) => ago !== null && ago !== undefined && ago < ms;
    const seen = recent(facts.signAgo, RECONNECT_HOLD_MS);
    // Links died unused and the engine keeps calling the iPhone (it says so
    // for 180 s after the last one); without a sign of the iPhone for a
    // minute, it may as well have walked away.
    if (leState === LE_STATE.waiting && leCause === LE_CAUSE.unsteady && seen)
        return 'retrying';
    // One link that died at the edge of range is walking away; two, or one
    // with the iPhone still seen, is the iPhone trying.
    if (facts.fails >= 2 || (facts.fails >= 1 && p?.state === 'here'))
        return 'dropping';
    if (p?.state === 'here')
        return 'back';
    if (!leState && p?.state === 'away' && p.reason === 'computer_suspended' &&
        recent(facts.signAgo, RECONNECT_SLEEP_MS))
        return 'sleep';
    return null;
}

// How long an engine that holds off leaves the iPhone alone after LE links
// died unused, by their count (le_strikes.hpp before #29: 15 s after one,
// 30 s after two, then twice as long each time, 120 s at most). Since #29 the
// advertisement stays on air and `holding_off` only comes back when the
// pauses are restored through the environment: then, and only then, the
// menu says when the next try comes. Nothing else promises a delay.
export function holdOffSeconds(strikes) {
    if (!Number.isInteger(strikes) || strikes < 1)
        return null;
    if (strikes < 2)
        return 15;
    return Math.min(120, 30 * 2 ** (strikes - 2));
}

export const withReconnect = Base => class extends Base {
    // Called with each bt_connection_changed: signs of the iPhone, links
    // that died before they served, the engine holding off.
    _reconnectTrack(conn) {
        const now = this._now();
        const any = conn.le_connected === true || conn.classic_connected === true;
        if (any && !this._anyUp)
            this._signAt = now;
        this._anyUp = any;
        const up = this._linked();
        if (up && this._upAt === null) {
            this._upAt = now;
            this._upServed = false;
        }
        if (up && this._linkConfirmed())
            this._upServed = true;
        if (!up && this._upAt !== null) {
            if (!this._upServed)
                this._fails.push(now);
            this._upAt = null;
        }
        this._fails = this._fails.filter(t => now - t < RECONNECT_HOLD_MS);
        const holding = conn[FIELD.leCause] === LE_CAUSE.holdingOff;
        const strikes = conn[FIELD.leStrikes];
        if (holding && (!this._holdOff || this._holdOff.strikes !== strikes))
            this._holdOff = {since: now, strikes};
        else if (!holding && this._holdOff) {
            // The engine calls the iPhone again: a sign, as good as a try.
            this._holdOff = null;
            this._signAt = now;
        }
    }

    // bt_presence: the iPhone seen, or this computer just back from sleep.
    _reconnectPresence(event) {
        if (event.state === 'here' || (event.state === 'away' && event.reason === 'computer_suspended'))
            this._signAt = this._now();
    }

    _reconnectFacts() {
        const now = this._now();
        const conn = this._conn ?? {};
        const ago = at => at === null ? null : now - at;
        return {
            link: this._link(),
            confirmed: this._linkConfirmed(),
            shown: this._shown,
            presence: this._presence,
            leState: typeof conn[FIELD.leState] === 'string' ? conn[FIELD.leState] : null,
            leCause: typeof conn[FIELD.leCause] === 'string' ? conn[FIELD.leCause] : null,
            upFor: ago(this._upAt),
            signAgo: ago(this._signAt),
            fails: this._fails.filter(t => now - t < RECONNECT_HOLD_MS).length,
            tour: this._tour !== null,
            enabled: this._status?.enabled !== false,
        };
    }

    // Before each _settle(): whether the link is reconnecting, and since
    // when. Said RECONNECT_MAX_MS at most in a row; then not again until a
    // link serves or nothing has been under way for RECONNECT_CALM_MS. A
    // reason that goes away for a pass or two neither ends the time already
    // spent nor starts it again (#42): `calm` dates the first pass without
    // one, and only a calm that lasts forgets it.
    _reconnectWatch() {
        const now = this._now();
        if (this._linkConfirmed())
            this._reconnect = null;
        const reason = reconnectReason(this._reconnectFacts());
        let was = this._reconnect;
        if (was?.calm !== undefined && was.calm !== null && now - was.calm >= RECONNECT_CALM_MS)
            was = this._reconnect = null;
        if (!reason) {
            if (was)
                this._reconnect = {...was, reason: null, calm: was.calm ?? now};
        } else if (was?.spent) {
            this._reconnect = {...was, calm: null};
        } else if (was && now - was.since >= RECONNECT_MAX_MS) {
            this._reconnect = {spent: true, reason: null, calm: null};
        } else {
            this._reconnect = {since: was?.since ?? now, reason, calm: null};
        }
    }

    // The reason said now, or null.
    _reconnecting() {
        return this._reconnect?.reason ?? null;
    }

    // When the engine calls the iPhone again, if it said it holds off and
    // the pause it takes is known: ms, or null.
    _retryAt() {
        const hold = this._holdOff;
        const seconds = hold ? holdOffSeconds(hold.strikes) : null;
        return seconds === null ? null : hold.since + seconds * 1000;
    }

    // The moments something said here changes by itself.
    _reconnectDeadlines() {
        const r = this._reconnect;
        if (!r?.reason)
            return [];
        const now = this._now();
        const times = [r.since + RECONNECT_MAX_MS];
        if (this._signAt !== null)
            times.push(this._signAt + RECONNECT_HOLD_MS, this._signAt + RECONNECT_SLEEP_MS);
        if (this._upAt !== null)
            times.push(this._upAt + RECONNECT_SETUP_MS);
        for (const t of this._fails)
            times.push(t + RECONNECT_HOLD_MS);
        // The countdown of the menu, every 5 s, while it is open.
        const retryAt = this._retryAt();
        if (this._cardOpen && retryAt !== null && now < retryAt) {
            const left = Math.ceil((retryAt - now) / 5000) * 5;
            times.push(retryAt - (left - 5) * 1000);
        }
        return times.filter(t => t > now);
    }

    // What is going on, in a short sentence for the menu.
    _reconnectLine() {
        const _ = this._;
        const reason = this._reconnecting();
        if (reason === 'retrying' || reason === 'dropping') {
            const retryAt = reason === 'retrying' ? this._retryAt() : null;
            const now = this._now();
            if (retryAt !== null && now < retryAt) {
                const seconds = Math.ceil((retryAt - now) / 5000) * 5;
                // Translators: %d is a number of seconds, like 30.
                return _('The link keeps dropping, trying again in %d s').replace('%d', seconds);
            }
            return _('The link keeps dropping, trying again…');
        }
        if (reason === 'sleep')
            return _('Reconnecting after sleep…');
        if (reason === 'setting_up')
            return _('Almost there, getting notifications ready…');
        return _('Your iPhone is back, setting up the link…');
    }
};
