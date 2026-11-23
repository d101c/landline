// SPDX-License-Identifier: GPL-2.0-or-later
// The setup tour.
//
// One guided walk from the first click to a working link:
// permission, then pairing, then the switches on the iPhone, a try, done.
// The protocol moves at its own speed; what is *shown* moves through a
// queue, so every step stays on screen for a moment and none is skipped.

import {
    TOUR_ALREADY_PAIRED_MS,
    TOUR_MIN_STEP_MS,
    TOUR_TICK_MS,
    TOUR_VERIFY_MS,
} from './constants.js';
import {sender} from './format.js';

export const withTour = Base => class extends Base {
    setSystemSetup(status) {
        this._system = status;
        // The settings were changed by hand while the tour was showing the
        // commands: it goes on by itself.
        const tour = this._tour;
        if (tour?.step === 'permission' && this._permissionManual() && !this._needsPermission() &&
            !['done', 'waiting'].includes(tour.permission))
            this.tourPermission('done');
        this._changed();
    }

    // Nothing can ask for the password from here: the tour shows what to
    // run in a terminal instead.
    _permissionManual() {
        return this._system !== null && this._system.elevation === false;
    }

    _permissionCommands() {
        if (this._system?.command)
            return [this._system.command];
        return this._setupSteps().map(step => step.command).filter(c => c);
    }

    // The click on the indicator (or a menu entry): start the tour at the
    // first step that still has something to do, or bring it back.
    startTour() {
        if (this._engine !== 'connected' || !this._status)
            return;
        this._cardOpen = false;
        const tour = this._tour;
        if (tour) {
            tour.open = true;
            this._changed();
            return;
        }
        // The indicator had settled on "Link again" because the link would
        // not hold: the walk starts at the repair, whatever the links are
        // doing this very second. This click is the user taking it in hand.
        const unsteady = this.steady && this._unsteady !== null;
        // The wished settings ride along with the needed ones, or are
        // offered on their own once per session.
        const first = this._system && (this._needsPermission() || this._wishPending())
            ? 'permission' : this._tourFirstLinkStep(false, unsteady);
        if (first === 'done')
            return;
        this._newTour(first, first === 'permission'
            ? ['permission', ...this._tourPlanFrom(this._tourFirstLinkStep(true, unsteady))]
            : this._tourPlanFrom(first));
        this._tour.unsteady = unsteady;
        this._steadyReset();
        this._tourEnter(first, true);
        this._changed();
    }

    _newTour(step, plan) {
        this._tour = {
            open: true, step, since: this._now(), holdUntil: 0, queue: [], plan,
            permission: 'idle', repairs: 0, repair: null, error: '', failedAt: null,
            paired: false, checking: false, verified: false, verifyUntil: 0,
            reviewed: false, proof: null, forget: null,
        };
        this._tourResetPairing();
    }

    _tourResetPairing() {
        Object.assign(this._tour, {
            scans: 0, candidates: [], address: null, name: '', running: false, runningUntil: null,
            operationId: null, confirmId: undefined, code: '', until: 0, answered: false, already: false,
            searching: false,
        });
    }

    _linkedEither() {
        const conn = this._conn ?? {};
        return conn.classic_connected === true || conn.le_connected === true;
    }

    // The address of an iPhone this computer already holds a pairing for.
    _knownPairing() {
        const device = this._devices.find(d => d.iphone === true && (d.paired || d.bonded));
        if (device)
            return device.address;
        if (this._conn?.device_paired === true && this._status?.device_address)
            return this._status.device_address;
        const state = this._status?.bond_state;
        if (this._status?.bond_address && state && state !== 'none')
            return this._status.bond_address;
        return null;
    }

    // What the engine says about the health of the pairing: true or false when it says so, null when
    // it is an engine from before that contract.
    _needsRepair() {
        for (const source of [this._conn, this._status]) {
            if (typeof source?.needs_repair === 'boolean')
                return source.needs_repair;
        }
        return null;
    }

    // A pairing exists but cannot work. The engine's own word comes first;
    // without it: a bond with no LE half, or a link the iPhone keeps refusing.
    _brokenPairing() {
        const said = this._needsRepair();
        if (said !== null)
            return said;
        const conn = this._conn ?? {};
        const capability = this._status?.capability ?? {};
        const paired = conn.device_paired === true || capability.bonded_device_present === true;
        if (!paired)
            return false;
        if (capability.bonded_device_present === true && capability.bond_has_le === false &&
            capability.bond_carries_ancs !== true)
            return true;
        if (conn.device_paired === true && conn.le_available === false)
            return true;
        return /authenticat/i.test(String(conn.link_reason ?? ''));
    }

    // The engine found a pairing without its LE half and no way to get one
    // on this computer: messages, contacts and calls only.
    _noNotifications() {
        const conn = this._conn ?? {};
        return conn.needs_repair === false && conn.bond_state === 'partial' && conn.bond_detail === 'bredr_only';
    }

    _linkComplete() {
        const conn = this._conn ?? {};
        return conn.classic_connected === true && (conn.le_connected === true || this._noNotifications());
    }

    _allOn() {
        const s = this._switches();
        return s.messages && s.contacts && (s.notifications || this._noNotifications());
    }

    // Where the linking part starts. afterPermission: the settings of this
    // computer were just fixed, so a pairing that is not known to be broken
    // gets a chance to come back by itself.
    _tourFirstLinkStep(afterPermission, unsteady = false) {
        const broken = this._brokenPairing() || unsteady;
        if (this._linked() && !broken)
            return this._allOn() ? 'done' : 'switches';
        if (!this._knownPairing())
            return 'find';
        if (broken)
            return 'repair';
        if (afterPermission)
            return 'pairing';
        // An engine that vouches for the pairing can be asked to use it;
        // one that says nothing cannot be trusted with it.
        return this._needsRepair() === false ? 'find' : 'repair';
    }

    _tourPlanFrom(step) {
        const all = ['repair', 'find', 'code', 'pairing', 'switches', 'try', 'done'];
        if (step === 'pairing')
            return ['pairing', 'switches', 'try', 'done'];
        return all.slice(all.indexOf(step));
    }

    // Do what a step starts with, and queue it for display.
    _tourEnter(step, first = false) {
        const tour = this._tour;
        const now = this._now();
        switch (step) {
        case 'repair':
            tour.repairs++;
            tour.repair = {state: 'ask', address: this._knownPairing() ?? tour.address, operationId: null, until: 0};
            tour.paired = false;
            tour.verified = false;
            if (!tour.plan.includes('repair'))
                tour.plan = [...tour.plan.filter(s => s === 'permission'), ...this._tourPlanFrom('repair')];
            break;
        case 'find':
            this._tourResetPairing();
            tour.paired = false;
            tour.verified = false;
            tour.searching = true;
            this._scan();
            break;
        case 'pairing':
            if (!tour.paired) {
                // An existing pairing: wait and see whether it comes back.
                tour.paired = true;
                tour.checking = true;
                tour.verifyUntil = now + TOUR_VERIFY_MS;
            }
            break;
        case 'switches':
            tour.verified = true;
            break;
        case 'done':
            this._send({command: 'bt_list_threads'});
            this._send({command: 'bt_list_contacts', limit: 10000});
            break;
        default:
            break;
        }
        if (first) {
            tour.step = step;
            tour.since = now;
        } else {
            this._tourGo(step);
        }
        if (step === 'pairing')
            this._tourVerify();
        if (step === 'switches')
            this._tourSwitches();
    }

    // Queue a step: it shows once the one on screen has had its moment.
    _tourGo(step) {
        const tour = this._tour;
        const last = tour.queue.length > 0 ? tour.queue[tour.queue.length - 1] : tour.step;
        if (last === step)
            return;
        tour.queue.push(step);
        this._tourPump();
    }

    // Show a step right now (failure, or the user's own Back and Continue).
    _tourJump(step) {
        const tour = this._tour;
        tour.queue = [];
        tour.step = step;
        tour.since = this._now();
        tour.holdUntil = 0;
    }

    _tourPumpAt() {
        const tour = this._tour;
        return Math.max(tour.since + TOUR_MIN_STEP_MS, tour.holdUntil);
    }

    _tourPump() {
        const tour = this._tour;
        const now = this._now();
        if (tour.queue.length > 0 && now >= this._tourPumpAt()) {
            tour.step = tour.queue.shift();
            tour.since = now;
            tour.holdUntil = 0;
        }
    }

    _scan() {
        const tour = this._tour;
        tour.scans++;
        this._send({command: 'bt_scan'});
        this._send({command: 'bt_list_devices'});
    }

    _tourSearching() {
        const tour = this._tour;
        return !!tour && tour.searching === true && !tour.running;
    }

    _tourDevices() {
        const tour = this._tour;
        if (!this._tourSearching())
            return;
        const candidates = this._devices.filter(d => d.iphone === true);
        tour.candidates = candidates.map(d => ({address: d.address, name: d.name || d.address}));
        if (candidates.length === 1)
            this._tourCandidate(candidates[0]);
    }

    _tourCandidate(device) {
        const tour = this._tour;
        tour.name = device.name || '';
        // This computer already knows that iPhone: asking to pair would skip
        // the number and report a success that may not be one.
        if ((device.paired || device.bonded) && device.bond_state !== 'ok' && tour.repairs === 0) {
            tour.address = device.address;
            tour.candidates = [];
            tour.searching = false;
            this._tourEnter('repair');
            return;
        }
        this._pair(device.address);
    }

    choosePairingDevice(address) {
        const tour = this._tour;
        if (!this._tourSearching())
            return;
        const device = this._devices.find(d => d.address === address) ?? {address};
        this._tourCandidate(device);
        this._changed();
    }

    _pair(address) {
        const tour = this._tour;
        tour.address = address;
        tour.searching = false;
        tour.running = true;
        tour.candidates = [];
        tour.operationId = `pair-${++this._sequence}`;
        // "Found <name>" gets its own moment on screen.
        if (tour.step === 'find' && tour.queue.length === 0)
            tour.since = this._now();
        const command = {command: 'bt_pair', address, operation_id: tour.operationId};
        // After a repair, whatever is left of the old pairing must go.
        if (tour.repairs > 0)
            command.fresh = true;
        this._send(command);
    }

    _tourScanEnded() {
        const tour = this._tour;
        if (!this._tourSearching() || tour.candidates.length > 0)
            return;
        if (tour.scans < 6)
            this._scan();
        else
            this._tourFail(this._('Your iPhone was not found. Keep its Bluetooth screen open and stay close to this computer.'));
    }

    _tourFail(message) {
        const tour = this._tour;
        if (tour.step !== 'failed')
            tour.failedAt = tour.queue.length > 0 ? tour.queue[tour.queue.length - 1] : tour.step;
        this._tourJump('failed');
        tour.searching = false;
        tour.error = message;
    }

    _tourConfirmRequest(event) {
        const tour = this._tour;
        Object.assign(tour, {
            code: String(event.code ?? ''),
            confirmId: event.operation_id,
            answered: false,
            until: this._now() + (Number(event.timeout_seconds) || 60) * 1000,
        });
        this._tourGo('code');
    }

    _tourResult(event) {
        const _ = this._;
        const tour = this._tour;
        if (event.operation_id !== undefined && tour.operationId && event.operation_id !== tour.operationId)
            return;
        tour.running = false;
        tour.runningUntil = null;
        if (!event.success && (event.status === 'stale_bond' || event.reason === 'stale_bond')) {
            // The engine refuses to build on a pairing that cannot work.
            this._tourBroken();
            return;
        }
        if (!event.success) {
            if (tour.step === 'failed')
                return;
            this._tourFail({
                rejected: _('The link was declined. If the numbers were the same, try again and tap Pair on the iPhone.'),
                timeout: _('The iPhone did not answer in time.'),
                not_found: _('Your iPhone was not found. Keep its Bluetooth screen open and stay close to this computer.'),
                busy: _('Another attempt is still running. Wait a moment and try again.'),
            }[event.status] ?? _('Linking did not work this time.'));
            return;
        }
        // "Paired" is only a promise: the link counts once both halves are
        // really up. Until then, wait and say so.
        const now = this._now();
        tour.paired = true;
        tour.code = '';
        tour.checking = tour.already || event.status === 'already_paired';
        const repair = typeof event.needs_repair === 'boolean' ? event.needs_repair : event.dual_bond === false;
        if (repair) {
            this._tourBroken();
            return;
        }
        tour.verifyUntil = now + (tour.checking ? TOUR_ALREADY_PAIRED_MS : TOUR_VERIFY_MS);
        this._tourGo('pairing');
        this._tourVerify();
    }

    // After a pairing that claims to exist: both links up, or it is broken.
    _tourVerify() {
        const tour = this._tour;
        if (!tour || !tour.paired || tour.verified || tour.step === 'failed' || tour.step === 'repair' ||
            tour.queue.includes('repair'))
            return;
        if (this._linkComplete()) {
            if (tour.checking && !tour.plan.includes('code') && this._allOn()) {
                // A pairing that worked before came back whole.
                tour.plan = tour.plan.filter(step => step !== 'switches' && step !== 'try');
                tour.verified = true;
                this._tourEnter('done');
            } else {
                this._tourEnter('switches');
            }
            return;
        }
        if (this._brokenPairing() || this._now() >= tour.verifyUntil)
            this._tourBroken();
    }

    // The repair was offered because the links were slow, and they came up
    // after all before anything was forgotten: no repair needed.
    _tourRescue() {
        const tour = this._tour;
        if (!tour || tour.repair?.state !== 'ask' || !(tour.step === 'repair' || tour.queue.includes('repair')))
            return;
        // Links that come and go are what this repair is for.
        if (tour.unsteady)
            return;
        if (this._needsRepair() === true || !this._linkComplete() || this._conn?.le_connected !== true)
            return;
        tour.repair = null;
        tour.paired = true;
        tour.plan = tour.plan.filter(step => step !== 'repair');
        this._tourJump(tour.step === 'repair' ? 'pairing' : tour.step);
        this._tourEnter('switches');
    }

    _tourBroken() {
        const tour = this._tour;
        if (tour.repairs >= 1) {
            tour.paired = false;
            this._tourFail(this._('The link still does not come up. Turn Bluetooth off and on again on the iPhone and on this computer, then try once more.'));
            return;
        }
        this._tourEnter('repair');
    }

    // "Done on My iPhone": forget the iPhone here too, then pair afresh.
    tourRepairDone() {
        const tour = this._tour;
        if (tour?.step !== 'repair' || tour.repair?.state !== 'ask')
            return;
        if (!tour.repair.address) {
            this._tourAfterRepair();
        } else {
            tour.repair.state = 'removing';
            tour.repair.operationId = `unpair-${++this._sequence}`;
            tour.repair.until = this._now() + 15000;
            this._send({command: 'bt_unpair', address: tour.repair.address, operation_id: tour.repair.operationId});
        }
        this._changed();
    }

    _tourUnpairResult(event) {
        const tour = this._tour;
        if (tour?.repair?.state !== 'removing')
            return;
        if (event.operation_id !== undefined && event.operation_id !== tour.repair.operationId)
            return;
        if (event.success === false) {
            tour.repair.state = 'ask';
            this._tourFail(this._('This computer could not forget the iPhone by itself. Remove the iPhone in the Bluetooth settings of this computer, then try again.'));
            return;
        }
        this._tourAfterRepair();
    }

    _tourAfterRepair() {
        const tour = this._tour;
        this._steadyReset();
        tour.repair.state = 'removed';
        this._devices = this._devices.filter(d => d.address !== tour.repair.address);
        this._tourEnter('find');
    }

    _switches() {
        const conn = this._conn ?? {};
        // With the classic link on demand, a switch found on at the last
        // opening still counts while the link is closed.
        return {
            notifications: conn.ancs_ready === true,
            messages: this._messagesUsable(),
            contacts: this._contactsUsable(),
        };
    }

    _tourSwitches() {
        const tour = this._tour;
        if (!tour || !tour.verified || tour.reviewed || !tour.plan.includes('switches'))
            return;
        const shown = tour.queue.length > 0 ? tour.queue[tour.queue.length - 1] : tour.step;
        if (shown !== 'switches')
            return;
        if (this._allOn()) {
            // Leave the last tick on screen for a moment.
            tour.holdUntil = this._now() + TOUR_TICK_MS;
            this._send({command: 'bt_list_threads'});
            this._send({command: 'bt_list_contacts', limit: 10000});
            this._tourGo(tour.plan.includes('try') ? 'try' : 'done');
        }
    }

    // A message reached this computer while the tour was asking for one.
    _tourProof(name, body) {
        const tour = this._tour;
        if (tour?.step !== 'try' || tour.proof)
            return;
        const who = sender(name, '', '');
        tour.proof = {name: who.name || this._('Someone'), body: this._preview(body)};
    }

    // The password window: 'waiting' while it is up, then what came of it.
    tourPermission(state) {
        const tour = this._tour;
        if (!tour || tour.step !== 'permission')
            return;
        if (state === 'done' && this._needsPermission())
            state = 'failed';
        // No helper of root to run: the commands are shown instead.
        if (state === 'unavailable')
            state = 'idle';
        // Nothing needed was asked: a wish turned down, or that could not
        // be made, is let go and the tour goes on.
        if (['refused', 'failed', 'partial'].includes(state) && !this._needsPermission())
            state = 'skipped';
        if (['done', 'skipped'].includes(state) && this._systemWishes().length > 0)
            this._wishesDeclined = true;
        tour.permission = state;
        if (state === 'done' || state === 'skipped') {
            const next = this._tourFirstLinkStep(true, tour.unsteady === true);
            tour.plan = ['permission', ...this._tourPlanFrom(next)];
            tour.open = true;
            this._tourEnter(next);
        }
        this._changed();
    }

    // "Keep the link steady" in Phone Settings: asked for, so offered again
    // even if it was turned down earlier in the session.
    startSteadyTour() {
        this._wishesDeclined = false;
        this.startTour();
    }

    // "Skip" on a permission step that only offers wished settings.
    tourSkipPermission() {
        if (this._tour?.step !== 'permission' || this._needsPermission())
            return;
        this.tourPermission('skipped');
    }

    tourNext() {
        const tour = this._tour;
        if (!tour)
            return;
        if (tour.step === 'switches' && tour.plan.includes('try'))
            this._tourJump('try');
        else if (tour.step === 'switches' || tour.step === 'try')
            this._tourJump('done');
        else
            return;
        this._changed();
    }

    tourBack() {
        const tour = this._tour;
        if (!tour)
            return;
        if (tour.step === 'done' && tour.plan.includes('try')) {
            this._tourJump('try');
        } else if (tour.step === 'try' && tour.plan.includes('switches')) {
            tour.reviewed = true;
            this._tourJump('switches');
        } else {
            return;
        }
        this._changed();
    }

    // Try Again after a failure: back to the start of the linking part.
    retryTour() {
        const tour = this._tour;
        if (tour?.step !== 'failed' || tour.running)
            return;
        tour.error = '';
        tour.paired = false;
        tour.verified = false;
        if (tour.failedAt === 'permission') {
            tour.permission = 'idle';
            this._tourJump('permission');
        } else {
            this._tourResetPairing();
            const next = this._knownPairing() ? 'repair' : 'find';
            if (next === 'repair')
                tour.repairs = 0;
            this._tourEnter(next, true);
            tour.queue = [];
        }
        this._changed();
    }

    // Escape: the tour steps aside and the indicator brings it back where it
    // was. Where nothing is under way there is nothing to keep.
    closeTour() {
        const tour = this._tour;
        if (!tour)
            return;
        if (tour.step === 'forget') {
            if (tour.forget.state === 'removing') {
                tour.open = false;
                this._changed();
            } else {
                this.forgetBack();
            }
            return;
        }
        const keep = ['find', 'code', 'pairing', 'switches', 'repair'].includes(tour.step) ||
            (tour.step === 'permission' && tour.permission === 'waiting') || tour.queue.length > 0;
        if (!keep) {
            this.cancelTour();
            return;
        }
        tour.open = false;
        this._changed();
    }

    // Cancel, Not Now, Finish Later, Done: the tour is over.
    cancelTour() {
        const tour = this._tour;
        if (!tour)
            return;
        if (tour.code && !tour.answered && !tour.paired)
            this._sendPairAnswer(false);
        if (tour.running && tour.operationId && !tour.paired)
            this._abandoned = tour.operationId;
        this._tour = null;
        // What "Fix It" said only made sense on the step that showed it.
        if (this._fix?.state === 'fixed')
            this._fix = null;
        this._changed();
    }

    _sendPairAnswer(accept) {
        const tour = this._tour;
        const command = {command: 'bt_pair_confirm', accept};
        if (tour.confirmId !== undefined)
            command.operation_id = tour.confirmId;
        this._send(command);
        tour.answered = true;
    }

    confirmPairing(accept) {
        const tour = this._tour;
        if (tour?.step === 'code' && !tour.answered) {
            this._sendPairAnswer(accept === true);
            tour.runningUntil = this._now() + 60000;
            if (accept === true)
                this._tourGo('pairing');
            this._changed();
            return;
        }
        const pairing = this._pairing;
        if (!pairing)
            return;
        const command = {command: 'bt_pair_confirm', accept: accept === true};
        if (pairing.operationId !== undefined)
            command.operation_id = pairing.operationId;
        this._send(command);
        this._pairing = null;
        this._cardOpen = false;
        this._changed();
    }
};
