// SPDX-License-Identifier: GPL-2.0-or-later
// Everyday settings: the ones the engine keeps, the iPhone's Internet
// connection, the battery.
//
// The sound of the iPhone and the lock on walking away are settings of the
// engine. Phone Settings shows them through three keys of the extension: a
// change there comes in by wantSetting(), a value the engine holds goes out
// by onSetting.

import {SETTING_ECHO_MS} from './constants.js';

export const withDaily = Base => class extends Base {
    _engineSetting(name) {
        switch (name) {
        case 'phone-audio-here':
            return this._audioState ? this._audioState.phone_audio === 'here' : null;
        case 'lock-on-away':
            return typeof this._status?.lock_on_away === 'boolean' ? this._status.lock_on_away : null;
        case 'lock-away-seconds':
            return Number.isInteger(this._status?.lock_away_seconds) ? this._status.lock_away_seconds : null;
        default:
            return null;
        }
    }

    wantSetting(name, value) {
        const held = this._engineSetting(name);
        // Nothing known of the engine yet, or nothing to change.
        if (held === null || held === value || this._engine !== 'connected')
            return;
        this._wanted.set(name, {value, until: this._now() + SETTING_ECHO_MS});
        switch (name) {
        case 'phone-audio-here':
            this._send({command: 'bt_set_phone_audio', route: value ? 'here' : 'phone'});
            // Shown at once; bt_audio confirms.
            this._audioState = {...this._audioState, phone_audio: value ? 'here' : 'phone'};
            break;
        case 'lock-on-away':
            this._send({command: 'bt_set_lock_on_away', enabled: value === true,
                grace_seconds: this._wanted.get('lock-away-seconds')?.value ?? this._status.lock_away_seconds ?? 30});
            break;
        case 'lock-away-seconds':
            this._send({command: 'bt_set_lock_on_away', grace_seconds: value,
                enabled: this._wanted.get('lock-on-away')?.value ?? this._status.lock_on_away === true});
            break;
        default:
            break;
        }
        this._changed();
    }

    // "Play iPhone audio on this computer", from the menu.
    setPhoneAudio(here) {
        this.wantSetting('phone-audio-here', here === true);
        this.onSetting?.('phone-audio-here', here === true);
    }

    _reportSettings() {
        const now = this._now();
        for (const name of ['phone-audio-here', 'lock-on-away', 'lock-away-seconds']) {
            const held = this._engineSetting(name);
            if (held === null)
                continue;
            const wanted = this._wanted.get(name);
            if (wanted) {
                if (held !== wanted.value && now < wanted.until)
                    continue;
                this._wanted.delete(name);
            }
            this.onSetting?.(name, held);
        }
    }

    // --- the iPhone's Internet connection ------------------------------------------

    // The address NetworkManager knows the iPhone by.
    get phoneAddress() {
        return this._status?.device_address || '';
    }

    // {state: 'unavailable' | 'off' | 'connecting' | 'on', failed}
    setHotspot(hotspot) {
        this._hotspot = hotspot;
        this._changed();
    }

    // --- battery -----------------------------------------------------------------

    // The engine says the battery went under 20 % or 10 %: one quiet desktop
    // notification per crossing. `initial` (already low when the engine
    // started) is told too, once.
    _onBatteryLow(event) {
        const threshold = Number(event.threshold);
        const level = Number(event.level);
        if (!Number.isFinite(threshold) || !Number.isFinite(level) || this._batteryTold.has(threshold))
            return;
        this._batteryTold.add(threshold);
        // A fall through both at once is one event; do not tell the higher later.
        if (threshold <= 10)
            this._batteryTold.add(20);
        if (!this.batteryAlerts || this._hush())
            return;
        this.onAlert?.({kind: 'battery', level, threshold});
    }
};
