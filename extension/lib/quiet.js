// SPDX-License-Identifier: GPL-2.0-or-later
// When nothing may pop up: the Do Not Disturb switch of GNOME, and a screen
// that is being shared or recorded. This only watches and reports; what stays
// silent is decided by the model.

import Gio from 'gi://Gio';

const NOTIFICATIONS_SCHEMA = 'org.gnome.desktop.notifications';

export class QuietWatch {
    // onChange({dnd, sharing}) is called at once, then at every change.
    constructor(onChange) {
        this._onChange = onChange;
        this._handles = new Map();        // remote access handle -> signal id

        // "Do Not Disturb" in the quick settings is this key, inverted.
        this._settings = new Gio.Settings({schema_id: NOTIFICATIONS_SCHEMA});
        this._settings.connectObject('changed::show-banners', () => this._report(), this);

        // The same source the Shell uses for its own "screen is being shared"
        // indicator: every screen cast or remote desktop session is a handle.
        // A session that began before this was created is not seen.
        this._controller = global.backend.get_remote_access_controller?.() ?? null;
        this._controller?.connectObject('new-handle', (_c, handle) => this._onHandle(handle), this);

        this._report();
    }

    get dnd() {
        return !this._settings.get_boolean('show-banners');
    }

    get sharing() {
        return this._handles.size > 0;
    }

    _onHandle(handle) {
        if (!handle.is_recording || this._handles.has(handle))
            return;
        this._handles.set(handle, handle.connect('stopped', () => {
            this._drop(handle);
            this._report();
        }));
        this._report();
    }

    _drop(handle) {
        const id = this._handles.get(handle);
        if (id)
            handle.disconnect(id);
        this._handles.delete(handle);
    }

    _report() {
        this._onChange?.({dnd: this.dnd, sharing: this.sharing});
    }

    destroy() {
        this._onChange = null;
        this._settings.disconnectObject(this);
        this._controller?.disconnectObject(this);
        for (const handle of [...this._handles.keys()])
            this._drop(handle);
    }
}
