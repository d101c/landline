// SPDX-License-Identifier: GPL-2.0-or-later
// Forgetting the iPhone.
//
// Asked from the menu, from Phone Settings or from a tour that is stuck:
// confirm, remove the pairing on this computer (bt_unpair), then show how
// to do the same on the iPhone, which nobody can do in its place.

import {UNPAIR_TIMEOUT_MS} from './constants.js';

export const withForget = Base => class extends Base {
    _forgettable() {
        return this._status?.device_address || this._knownPairing() || null;
    }

    // false: nothing can be asked of the engine right now.
    startForget() {
        if (this._engine !== 'connected' || !this._status)
            return false;
        this._cardOpen = false;
        this._steadyReset();
        const tour = this._tour;
        if (!tour) {
            this._newTour('forget', ['forget']);
            this._tour.forget = {state: 'confirm', from: null};
        } else if (['switches', 'repair', 'failed', 'try', 'done'].includes(tour.step)) {
            tour.forget = {state: 'confirm', from: tour.step};
            tour.open = true;
            this._tourJump('forget');
        } else {
            // Something is under way (a search, a number to check): show that.
            tour.open = true;
        }
        this._changed();
        return true;
    }

    forgetBack() {
        const tour = this._tour;
        if (tour?.step !== 'forget' || tour.forget.state === 'removing')
            return;
        if (tour.forget.from && tour.forget.state !== 'phone') {
            const from = tour.forget.from;
            tour.forget = null;
            this._tourJump(from);
            this._changed();
        } else {
            this.cancelTour();
        }
    }

    forgetConfirm() {
        const tour = this._tour;
        const f = tour?.forget;
        if (tour?.step !== 'forget' || !['confirm', 'error'].includes(f.state))
            return;
        if (tour.running && tour.operationId && !tour.paired)
            this._abandoned = tour.operationId;
        Object.assign(tour, {paired: false, verified: false, repair: null, running: false, searching: false});
        const address = this._forgettable();
        if (!address) {
            this._forgotten();
        } else {
            Object.assign(f, {
                state: 'removing', address, operationId: `unpair-${++this._sequence}`,
                until: this._now() + UNPAIR_TIMEOUT_MS,
            });
            this._send({command: 'bt_unpair', address, operation_id: f.operationId});
        }
        this._changed();
    }

    _forgetResult(event) {
        const f = this._tour?.forget;
        if (f?.state !== 'removing')
            return;
        if (event.operation_id !== undefined && event.operation_id !== f.operationId)
            return;
        // "Not found": this computer had already forgotten it.
        if (event.success === false && event.status !== 'not_found') {
            f.state = 'error';
            return;
        }
        this._forgotten();
    }

    _forgotten() {
        const tour = this._tour;
        const address = tour.forget.address;
        tour.forget = {state: 'phone', from: null};
        tour.plan = ['forget'];
        this._devices = this._devices.filter(d => d.address !== address);
        this._fix = null;
        this._obexSince = null;
        this._steadyReset();
    }

    // "Link Again": the pairing walk, from the search.
    forgetLinkAgain() {
        const tour = this._tour;
        if (tour?.step !== 'forget' || tour.forget.state !== 'phone')
            return;
        Object.assign(tour, {
            forget: null, plan: this._tourPlanFrom('find'), error: '', failedAt: null,
            reviewed: false, proof: null, checking: false,
            // As after a repair: ask for a pairing from scratch, and do not
            // send the user round in circles if it fails again.
            repairs: 1,
        });
        this._tourJump('find');
        this._tourEnter('find', true);
        this._changed();
    }
};
