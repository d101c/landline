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

import {FLAP_LIMIT, FLAP_WINDOW_MS, STEADY_MS} from './constants.js';

export const withLink = Base => class extends Base {
    _watchLinks() {
        const now = this._now();
        const up = this._linkedEither();
        if (this._wasUp && !up)
            this._drops.push(now);
        this._drops = this._drops.filter(t => now - t < FLAP_WINDOW_MS);
        this._wasUp = up;
        const sound = up && this._needsRepair() !== true;
        if (!sound)
            this._steadySince = null;
        else if (this._steadySince === null)
            this._steadySince = now;
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

    // Links up and one service really working: nothing to wait for.
    _linkConfirmed() {
        const conn = this._conn ?? {};
        return this._linkedEither() &&
            (conn.map_open === true || conn.pbap_open === true || conn.ancs_ready === true);
    }

    // What the indicator should show once things hold.
    _targetLink() {
        const link = this._link();
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
        } else if (now - this._candidate.since >= this.settleMs) {
            this._shown = target;
            this._candidate = null;
        }
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
        if (this._status.capability?.powered === false && !this._linkedEither())
            return 'bluetooth';
        // A pairing that cannot work is never shown as a link, even while
        // one half of it happens to be up.
        // The engine's word is final; a guess gives way to a working LE link.
        const broken = this._brokenPairing() &&
            (this._needsRepair() === true || conn.le_connected !== true);
        if (this._linkedEither() && !broken)
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
