// SPDX-License-Identifier: GPL-2.0-or-later
// "Use iPhone's Internet Connection": once an iPhone is paired, NetworkManager
// holds a Bluetooth profile for it ("<name> Network"), which GNOME shows as a
// "Tether" tile. This drives that same profile from our menu: find it by the
// address of the iPhone, turn it on and off, say what state it is in. Nothing
// is created, hidden or deleted; the tile of GNOME stays as it is.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {LOG_PREFIX} from './names.js';

// How long the iPhone may take to hand out its connection.
const CONNECT_TIMEOUT_MS = 30000;

// NetworkManager, through libnm. Speaks of one iPhone by its address:
//   state(address)      'unavailable' (no profile for it), 'off', 'connecting', 'on'
//   connect(address)    resolves once asked; rejects when it cannot be asked
//   disconnect(address)
// and calls onChange() when anything it reads may have changed.
export class NetworkManagerBackend {
    constructor(onChange) {
        this._onChange = onChange;
        this._cancellable = new Gio.Cancellable();
        this._client = null;
        this._actives = new Set();
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
        for (const active of this._client?.get_active_connections() ?? []) {
            if (!this._actives.has(active)) {
                this._actives.add(active);
                active.connectObject('notify::state', () => this._onChange?.(), this);
            }
        }
        for (const active of [...this._actives]) {
            if (!(this._client?.get_active_connections() ?? []).includes(active)) {
                active.disconnectObject(this);
                this._actives.delete(active);
            }
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
        return (this._client?.get_devices() ?? []).find(device =>
            device instanceof this._NM.DeviceBt && this._same(device.get_hw_address(), address)) ?? null;
    }

    _active(profile) {
        return (this._client?.get_active_connections() ?? []).find(active =>
            active.get_uuid() === profile.get_uuid()) ?? null;
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
        this._cancellable.cancel();
        for (const active of this._actives)
            active.disconnectObject(this);
        this._actives.clear();
        this._client?.disconnectObject(this);
        this._client = null;
    }
}

// What the menu shows: {state, failed} for the iPhone at `address`.
//   state   'unavailable', 'off', 'connecting' or 'on'
//   failed  the last try to turn it on did not get there
export class Hotspot {
    // onChange({state, failed}) at every change.
    constructor(onChange) {
        this._onChange = onChange;
        this._backend = null;
        this._address = '';
        this._failed = false;
        this._asked = false;              // a connection was asked for and is not up yet
        this._sawConnecting = false;
        this._timeoutId = 0;
        this._last = '';
    }

    // null: no backend, the entry is not offered.
    setBackend(make) {
        this._backend?.destroy();
        this._backend = make ? make(() => this._sync()) : null;
        this._sync();
    }

    setAddress(address) {
        address = address || '';
        if (address === this._address)
            return;
        this._address = address;
        this._settle(false);
        this._sync();
    }

    get view() {
        let state = this._backend && this._address ? this._backend.state(this._address) : 'unavailable';
        // Asked, and NetworkManager has not started yet: already "connecting".
        if (state === 'off' && this._asked)
            state = 'connecting';
        return {state, failed: this._failed && state === 'off'};
    }

    _sync() {
        const state = this._backend && this._address ? this._backend.state(this._address) : 'unavailable';
        if (this._asked) {
            if (state === 'on')
                this._settle(false);
            else if (state === 'connecting')
                this._sawConnecting = true;
            else if (this._sawConnecting)
                this._settle(true);       // it began, then fell back: the iPhone said no
        }
        const view = this.view;
        const key = JSON.stringify(view);
        if (key !== this._last) {
            this._last = key;
            this._onChange?.(view);
        }
    }

    _settle(failed) {
        this._asked = false;
        this._sawConnecting = false;
        this._failed = failed;
        if (this._timeoutId) {
            GLib.source_remove(this._timeoutId);
            this._timeoutId = 0;
        }
    }

    set(on) {
        if (!this._backend || !this._address)
            return;
        if (!on) {
            this._settle(false);
            this._backend.disconnect(this._address).catch(e =>
                console.warn(`${LOG_PREFIX}: could not leave the iPhone's connection: ${e.message}`));
            this._sync();
            return;
        }
        this._settle(false);
        this._asked = true;
        this._timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CONNECT_TIMEOUT_MS, () => {
            this._timeoutId = 0;
            if (this._asked) {
                this._settle(true);
                this._sync();
            }
            return GLib.SOURCE_REMOVE;
        });
        this._backend.connect(this._address).catch(() => {
            if (this._asked) {
                this._settle(true);
                this._sync();
            }
        });
        this._sync();
    }

    destroy() {
        this._onChange = null;
        this._settle(false);
        this._backend?.destroy();
        this._backend = null;
    }
}
