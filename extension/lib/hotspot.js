// SPDX-License-Identifier: GPL-2.0-or-later
// The iPhone's Internet on this computer. Once an iPhone is paired,
// NetworkManager holds a Bluetooth profile for it ("<name> Network", PAN to
// the iPhone's NAP), which GNOME shows as a "Tether" tile. This drives that
// same profile from our menu: find it by the address of the iPhone, turn it
// on and off, say what state it is in and, when it fails, how. Nothing is
// created, hidden or deleted; the tile of GNOME stays as it is.
//
// What NetworkManager says of a failure (src/core/devices/bluetooth/
// nm-device-bt.c), on the iPhone's Bluetooth device:
//   - the connection to the iPhone's network service is refused or does not
//     come: FAILED, reason BT_FAILED, before the device reached CONFIG;
//   - it was accepted (BNEP is up, NetworkManager asks for an address), then
//     the link went: FAILED, BT_FAILED, after CONFIG;
//   - no address in 45 s: FAILED, IP_CONFIG_UNAVAILABLE (or a DHCP reason);
//   - once ACTIVATED, the link goes (the iPhone walked away, or stopped
//     sharing): FAILED, CARRIER or BT_FAILED.
// BlueZ answers every refusal with the same "Input/output error", so whether
// the iPhone is here at all is not read here: the model knows it from the
// engine.

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';

import {LOG_PREFIX} from './names.js';

// How long the iPhone may take to hand out its connection: past the 45 s
// NetworkManager gives DHCP, so that "no address" is told as such.
const CONNECT_TIMEOUT_MS = 50000;

// NetworkManager's device states and reasons, by number (libnm's
// NM.DeviceState and NM.DeviceStateReason), so that the rule below can be
// tested without libnm.
export const DEVICE = {prepare: 40, config: 50, ipConfig: 70, activated: 100, failed: 120};
const REASON = {ipConfigUnavailable: 5, dhcpStartFailed: 15, dhcpError: 16, dhcpFailed: 17};

// What a FAILED of the iPhone's device means, from the furthest state the
// attempt reached and the reason given: 'refused', 'dropped', 'no-internet'
// or 'lost'.
export function failureKind(reached, reason) {
    if (reached >= DEVICE.activated)
        return 'lost';
    if ([REASON.ipConfigUnavailable, REASON.dhcpStartFailed, REASON.dhcpError, REASON.dhcpFailed].includes(reason))
        return 'no-internet';
    return reached >= DEVICE.config ? 'dropped' : 'refused';
}

// NetworkManager, through libnm. Speaks of one iPhone by its address:
//   state(address)      'unavailable' (no profile for it), 'off', 'connecting', 'on'
//   connect(address)    resolves once asked; rejects when it cannot be asked
//   disconnect(address)
// calls onChange() when anything it reads may have changed, and
// onFailure(address, kind) when the iPhone's device fails (failureKind()).
export class NetworkManagerBackend {
    constructor(onChange, onFailure) {
        this._onChange = onChange;
        this._onFailure = onFailure;
        this._cancellable = new Gio.Cancellable();
        this._client = null;
        this._actives = new Set();
        this._devices = new Map();        // NM.DeviceBt -> furthest state of this attempt
        this._start().catch(e => {
            if (!this._cancellable.is_cancelled())
                console.warn(`${LOG_PREFIX}: NetworkManager is not reachable: ${e.message}`);
        });
    }

    async _start() {
        // Loaded here, so a system without libnm only loses this one entry.
        const {default: NM} = await import('gi://NM');
        this._NM = NM;
        Gio._promisify(NM.Client, 'new_async');
        Gio._promisify(NM.Client.prototype, 'activate_connection_async');
        Gio._promisify(NM.Client.prototype, 'deactivate_connection_async');
        const client = await NM.Client.new_async(this._cancellable);
        if (this._cancellable.is_cancelled())
            return;
        this._client = client;
        const changed = () => this._changed();
        client.connectObject(
            'connection-added', changed, 'connection-removed', changed,
            'active-connection-added', changed, 'active-connection-removed', changed,
            'device-added', changed, 'device-removed', changed, this);
        this._changed();
    }

    _changed() {
        // The state of an activation moves without the list changing.
        const actives = this._client?.get_active_connections() ?? [];
        for (const active of actives) {
            if (!this._actives.has(active)) {
                this._actives.add(active);
                active.connectObject('notify::state', () => this._onChange?.(), this);
            }
        }
        for (const active of [...this._actives]) {
            if (!actives.includes(active)) {
                active.disconnectObject(this);
                this._actives.delete(active);
            }
        }
        // The Bluetooth devices, to tell how an attempt ended.
        const devices = (this._client?.get_devices() ?? []).filter(d => d instanceof this._NM.DeviceBt);
        for (const device of devices) {
            if (!this._devices.has(device)) {
                this._devices.set(device, 0);
                device.connectObject('state-changed', (d, state, old, reason) =>
                    this._deviceState(d, state, reason), this);
            }
        }
        for (const device of [...this._devices.keys()]) {
            if (!devices.includes(device)) {
                device.disconnectObject(this);
                this._devices.delete(device);
            }
        }
        this._onChange?.();
    }

    _deviceState(device, state, reason) {
        if (state === DEVICE.prepare)
            this._devices.set(device, state);
        else if (state < DEVICE.failed)
            this._devices.set(device, Math.max(this._devices.get(device) ?? 0, state));
        if (state === DEVICE.failed) {
            const kind = failureKind(this._devices.get(device) ?? 0, reason);
            this._devices.set(device, 0);
            console.log(`${LOG_PREFIX}: the iPhone's connection failed (${kind}, NetworkManager reason ${reason})`);
            this._onFailure?.(device.get_hw_address(), kind);
        }
        this._onChange?.();
    }

    _same(a, b) {
        return !!a && !!b && String(a).toUpperCase() === String(b).toUpperCase();
    }

    // The Bluetooth profile NetworkManager made for that iPhone.
    _profile(address) {
        return (this._client?.get_connections() ?? []).find(connection =>
            this._same(connection.get_setting_bluetooth()?.get_bdaddr(), address)) ?? null;
    }

    _device(address) {
        return [...this._devices.keys()].find(device => this._same(device.get_hw_address(), address)) ?? null;
    }

    _active(profile) {
        return (this._client?.get_active_connections() ?? []).find(active =>
            active.get_uuid() === profile.get_uuid()) ?? null;
    }

    same(a, b) {
        return this._same(a, b);
    }

    state(address) {
        const profile = address ? this._profile(address) : null;
        if (!profile)
            return 'unavailable';
        const active = this._active(profile);
        if (!active)
            return 'off';
        const State = this._NM.ActiveConnectionState;
        switch (active.get_state()) {
        case State.ACTIVATED:
            return 'on';
        case State.ACTIVATING:
            return 'connecting';
        default:
            return 'off';
        }
    }

    async connect(address) {
        const profile = this._profile(address);
        if (!profile)
            throw new Error('no profile');
        // Without a device NetworkManager picks one itself, or refuses.
        await this._client.activate_connection_async(profile, this._device(address), null, this._cancellable);
        this._changed();
    }

    async disconnect(address) {
        const profile = this._profile(address);
        const active = profile ? this._active(profile) : null;
        if (active)
            await this._client.deactivate_connection_async(active, this._cancellable);
        this._changed();
    }

    destroy() {
        this._onChange = null;
        this._onFailure = null;
        this._cancellable.cancel();
        for (const active of this._actives)
            active.disconnectObject(this);
        this._actives.clear();
        for (const device of this._devices.keys())
            device.disconnectObject(this);
        this._devices.clear();
        this._client?.disconnectObject(this);
        this._client = null;
    }
}

// What the menu shows: {state, failed, reason} for the iPhone at `address`.
//   state   'unavailable', 'off', 'connecting' or 'on'
//   failed  the last try did not get there, or the connection fell
//   reason  with failed: 'refused', 'dropped', 'no-internet', 'lost',
//           'timeout' (no answer in time), 'error' (could not be asked)
export class Hotspot {
    // onChange({state, failed, reason}) at every change. timeoutMs: for the
    // tests.
    constructor(onChange, {timeoutMs = CONNECT_TIMEOUT_MS} = {}) {
        this._onChange = onChange;
        this._timeoutMs = timeoutMs;
        this._backend = null;
        this._address = '';
        this._failure = '';               // the reason of the last failure, '' when none
        this._asked = false;              // a connection was asked for and is not up yet
        this._timeoutId = 0;
        this._last = '';
    }

    // null: no backend, the entry is not offered. make(onChange, onFailure).
    setBackend(make) {
        this._backend?.destroy();
        this._backend = make
            ? make(() => this._sync(), (address, kind) => this._failed(address, kind))
            : null;
        this._sync();
    }

    setAddress(address) {
        address = address || '';
        if (address === this._address)
            return;
        this._address = address;
        this._settle('');
        this._sync();
    }

    get view() {
        let state = this._backend && this._address ? this._backend.state(this._address) : 'unavailable';
        // Asked, and NetworkManager has not started yet: already "connecting".
        if (state === 'off' && this._asked)
            state = 'connecting';
        const failed = this._failure !== '' && state === 'off';
        return {state, failed, reason: failed ? this._failure : ''};
    }

    _failed(address, kind) {
        const same = this._backend?.same ? this._backend.same(address, this._address)
            : String(address).toUpperCase() === this._address.toUpperCase();
        if (!same)
            return;
        // A failure nobody asked about is the connection that fell; one
        // after "off" was asked is not a failure.
        if (this._asked || kind === 'lost')
            this._settle(kind);
        this._sync();
    }

    _sync() {
        const state = this._backend && this._address ? this._backend.state(this._address) : 'unavailable';
        if (this._asked && state === 'on')
            this._settle('');
        const view = this.view;
        const key = JSON.stringify(view);
        if (key !== this._last) {
            this._last = key;
            this._onChange?.(view);
        }
    }

    _settle(failure) {
        this._asked = false;
        this._failure = failure;
        if (this._timeoutId) {
            GLib.source_remove(this._timeoutId);
            this._timeoutId = 0;
        }
    }

    set(on) {
        if (!this._backend || !this._address)
            return;
        if (!on) {
            this._settle('');
            this._backend.disconnect(this._address).catch(e =>
                console.warn(`${LOG_PREFIX}: could not leave the iPhone's connection: ${e.message}`));
            this._sync();
            return;
        }
        if (this._asked || this._backend.state(this._address) === 'on')
            return;
        this._settle('');
        this._asked = true;
        this._timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._timeoutMs, () => {
            this._timeoutId = 0;
            if (this._asked) {
                this._settle('timeout');
                // NetworkManager may still be at it: it is not left half-way.
                this._backend?.disconnect(this._address).catch(() => {});
                this._sync();
            }
            return GLib.SOURCE_REMOVE;
        });
        this._backend.connect(this._address).catch(e => {
            console.warn(`${LOG_PREFIX}: could not ask for the iPhone's connection: ${e.message}`);
            if (this._asked) {
                this._settle('error');
                this._sync();
            }
        });
        this._sync();
    }

    destroy() {
        this._onChange = null;
        this._settle('');
        this._backend?.destroy();
        this._backend = null;
    }
}
