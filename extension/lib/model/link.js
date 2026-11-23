// SPDX-License-Identifier: GPL-2.0-or-later
// The state of the link, and an indicator that does not blink with it.
//
// With a pairing broken on one side the links come up and fall every few
// seconds. Three rules keep the indicator from following them: a state is
// shown once it has held for LINK_SETTLE_MS (a confirmed link shows at once;
// messages and calls are not states and never wait); links that keep
// dropping, or a pairing the engine wants redone, settle the indicator on
// "Link again"; and it stays there until the user acts or the link has held
// for STEADY_MS.

import {FLAP_LIMIT, FLAP_WINDOW_MS, RECONNECT_AFTER_LINK_MS, STEADY_MS} from './constants.js';
import {ENGINE_REASONS} from './reconnect.js';

export const withLink = Base => class extends Base {
    _watchLinks() {
        const now = this._now();
        const up = this._linked();
        if (this._wasUp && !up)
            this._drops.push(now);
        this._drops = this._drops.filter(t => now - t < FLAP_WINDOW_MS);
        this._wasUp = up;
        const sound = up && this._needsRepair() !== true;
        if (!sound)
            this._steadySince = null;
        else if (this._steadySince === null)
            this._steadySince = now;
        // A link the iPhone really serves (notifications answered, or a
        // messages or contacts session) proves the pairing: drops before it
        // were range, not a broken pairing (18 November, 02:15: "Link again"
        // stayed a minute over a working link after a walk out and back).
        if (this._unsteady?.reason === 'flapping' && this._linkConfirmed())
            this._steadyReset();
        if (!this.steady || this._unsteady)
            return;
        if (this._needsRepair() === true)
            this._unsteady = {reason: 'repair'};
        else if (this._drops.length > FLAP_LIMIT)
            this._unsteady = {reason: 'flapping'};
    }

    _steadyCheck() {
        if (this._unsteady && this._steadySince !== null && this._now() - this._steadySince >= STEADY_MS)
            this._steadyReset();
    }

    // The user took it in hand, or the pairing is gone: start counting anew.
    _steadyReset() {
        this._unsteady = null;
        this._drops = [];
    }

    // Linked and one service really working: nothing to wait for.
    _linkConfirmed() {
        const conn = this._conn ?? {};
        return this._linked() &&
            (conn.map_open === true || conn.pbap_open === true || conn.ancs_ready === true);
    }

    // What the indicator should show once things hold: a state of _link(),
    // or 'reconnecting'.
    _targetLink() {
        const link = this._link();
        // Something is under way. A guess gives way to the latch of links
        // that keep dropping; what the engine says does not.
        const reconnect = this._reconnecting();
        const latch = this._unsteady?.reason;
        if (reconnect && latch !== 'repair' && (latch !== 'flapping' || ENGINE_REASONS.includes(reconnect)))
            return 'reconnecting';
        // Out of range, drops are what walking away looks like: shown as away.
        const outOfRange = this._presence?.state === 'away' && this._presence?.reason === 'out_of_range';
        if (this._unsteady?.reason === 'flapping' && link === 'away' && outOfRange)
            return 'away';
        if (this.steady && this._unsteady && (link === 'linked' || link === 'away'))
            return 'broken';
        return link;
    }

    _settle() {
        const target = this._targetLink();
        if (!this.steady || this._shown === null || target === this._shown) {
            this._shown = target;
            this._candidate = null;
            return;
        }
        const now = this._now();
        // Nothing was claimed yet, and this is not one of the two states a
        // shaky link swings between.
        const first = this._shown === 'starting' && target !== 'away' && target !== 'linked';
        if (first || (target === 'linked' && this._linkConfirmed())) {
            this._shown = target;
            this._candidate = null;
        } else if (this._candidate?.link !== target) {
            this._candidate = {link: target, since: now};
        } else if (now - this._candidate.since >= this._settleWait(target)) {
            this._shown = target;
            this._candidate = null;
        }
    }

    // How long `target` must hold before it is shown. From linked, a drop
    // that is reconnecting waits longer: a served link that blinks for a few
    // seconds stays linked.
    _settleWait(target) {
        return target === 'reconnecting' && this._shown === 'linked'
            ? Math.max(this.settleMs, RECONNECT_AFTER_LINK_MS) : this.settleMs;
    }

    // 'starting' | 'engine' | 'bluetooth' | 'permission' | 'setup' | 'relink' |
    // 'broken' | 'away' | 'linked'. The state as it is this instant; the
    // indicator shows it through _settle().
    _link() {
        if (this._engine === 'absent')
            return this.fixing ? 'starting' : 'engine';
        if (this._engine !== 'connected' || !this._status)
            return 'starting';
        const conn = this._conn ?? {};
        if (this._status.capability?.powered === false && !this._linked())
            return 'bluetooth';
        // A pairing that cannot work is never shown as a link, even while
        // one half of it happens to be up.
        // The engine's word is final; a guess gives way to a working LE link.
        const broken = this._brokenPairing() &&
            (this._needsRepair() === true || conn.le_connected !== true);
        if (this._linked() && !broken)
            return 'linked';
        if (this._needsPermission())
            return 'permission';
        if (broken)
            return 'broken';
        if (!this._status.device_address)
            return 'setup';
        if (this._conn && conn.device_paired === false)
            return 'relink';
        return 'away';
    }

    // With the system helper: a setting this computer needs is missing now,
    // or would not survive a restart. Without it: what the engine reports.
    _needsPermission() {
        if (!this._system)
            return this._setupSteps().length > 0;
        return this._systemNeeds().length > 0;
    }

    // Which settings the system helper still has to change, among
    // 'experimental' and 'ertm': none, one or both. The device class is not
    // one of them any more: an iPhone shares everything with the computer's
    // own class. A helper that says a setting is not required here
    // (`required`, BlueZ 5.88 and later for 'experimental') is believed.
    _systemNeeds() {
        const system = this._system;
        if (!system)
            return [];
        return ['experimental', 'ertm'].filter(key =>
            system.required?.[key] !== false &&
            (system[key] === false || system.persistent?.[key] === false));
    }

    // Settings that are wished for, never needed: 'le_params', the helper
    // that lets the engine ask the iPhone for a more tolerant LE link, when
    // the system helper says it is missing. Only offered where the password
    // window can be used: nothing to copy into a terminal for a wish.
    _systemWishes() {
        const system = this._system;
        if (!system || system.elevation !== true)
            return [];
        return system.le_params === false ? ['le_params'] : [];
    }

    // A wish the tour may still offer in this session.
    _wishPending() {
        return !this._wishesDeclined && this._systemWishes().length > 0;
    }

    _setupSteps() {
        const capability = this._status?.capability;
        // An older engine still lists the device class as a step: it is not.
        const steps = (Array.isArray(capability?.setup) ? capability.setup : [])
            .filter(step => !/hands-free|btclass|class 4 8/i.test(`${step?.what ?? ''} ${step?.command ?? ''}`));
        if (steps.length === 0 && capability?.mode === 'blocked')
            return [{what: (capability.reasons ?? [])[0] ?? '', command: ''}];
        return steps;
    }
};
