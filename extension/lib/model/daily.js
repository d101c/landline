// SPDX-License-Identifier: GPL-2.0-or-later
// Everyday settings: the ones the engine keeps, the battery. The iPhone's
// Internet connection is in internet.js.
//
// The sound of the iPhone and the lock on walking away are settings of the
// engine. Phone Settings shows them through three keys of the extension: a
// change there comes in by wantSetting(), a value the engine holds goes out
// by onSetting.

import {PHONE_AUDIO_WAIT_MS, SETTING_ECHO_MS} from './constants.js';
import {COMMAND, FIELD, LINK_MODE} from './protocol.js';

// The settings the engine keeps, by the key Phone Settings knows them by.
const ENGINE_SETTINGS = ['phone-audio-here', 'lock-on-away', 'lock-away-seconds', 'link-mode'];

export const withDaily = Base => class extends Base {
    _engineSetting(name) {
        switch (name) {
        case 'phone-audio-here':
            return this._audioState ? this._audioState.phone_audio === 'here' : null;
        case 'lock-on-away':
            return typeof this._status?.lock_on_away === 'boolean' ? this._status.lock_on_away : null;
        case 'lock-away-seconds':
            return Number.isInteger(this._status?.lock_away_seconds) ? this._status.lock_away_seconds : null;
        case 'link-mode': {
            // 'lean' or 'full'; null with an engine that has one mode only.
            // The status carries the setting; the connection repeats it.
            const mode = this._status?.[FIELD.linkMode] ?? this._conn?.[FIELD.linkMode];
            return Object.values(LINK_MODE).includes(mode) ? mode : null;
        }
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
            // Turned on: the engine opens the link for it, the menu waits.
            this._phoneAudioWait = value ? {since: this._now()} : null;
            break;
        case 'lock-on-away':
            this._send({command: 'bt_set_lock_on_away', enabled: value === true,
                grace_seconds: this._wanted.get('lock-away-seconds')?.value ?? this._status.lock_away_seconds ?? 30});
            break;
        case 'lock-away-seconds':
            this._send({command: 'bt_set_lock_on_away', grace_seconds: value,
                enabled: this._wanted.get('lock-on-away')?.value ?? this._status.lock_on_away === true});
            break;
        case 'link-mode':
            if (!Object.values(LINK_MODE).includes(value)) {
                this._wanted.delete(name);
                return;
            }
            this._send({command: COMMAND.setLinkMode, mode: value});
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

    // "Connect for Sound": the choice is on, the classic link is not up for
    // it. In the lean mode the engine opens the link when the sound is asked
    // for and lets it go after a silence (#43): asking again opens it again.
    connectPhoneAudio() {
        if (this._phoneAudioHere() !== true || this._engine !== 'connected')
            return;
        this._send({command: 'bt_set_phone_audio', route: 'here'});
        this._phoneAudioWait = {since: this._now()};
        this._changed();
    }

    // What the switch shows: the value just asked for while the engine has
    // not answered yet, then the one it holds.
    _phoneAudioHere() {
        const held = this._engineSetting('phone-audio-here');
        const wanted = this._wanted.get('phone-audio-here');
        if (held !== null && wanted && this._now() < wanted.until)
            return wanted.value;
        return held;
    }

    // 'off' | 'away' | 'standby' | 'connecting' | 'playing' | 'failed', or
    // null with an engine that cannot say. In the lean link mode the classic
    // link is closed until the sound is asked for: the engine says the route
    // is 'unavailable' then. Asked for (turned on, "Connect for Sound"), it is
    // on its way; else the choice stands by until it is asked for, or until
    // the iPhone opens the link itself.
    _phoneAudioState() {
        const here = this._phoneAudioHere();
        if (here === null)
            return null;
        if (!here)
            return 'off';
        if (this._audioState?.phone_audio_route === 'here')
            return 'playing';
        if (this._link() !== 'linked')
            return 'away';
        const wait = this._phoneAudioWait;
        if (!wait)
            return 'standby';
        if (this._now() >= wait.since + PHONE_AUDIO_WAIT_MS)
            return 'failed';
        return 'connecting';
    }

    // Called on every change: ends the wait for the sound (it is started
    // by asking for it).
    _phoneAudioWatch() {
        const here = this._phoneAudioHere() === true;
        const route = this._audioState?.phone_audio_route;
        // Played once: the line on how to choose it on the iPhone has been
        // seen when the menu closes.
        if (here && route === 'here' && this._cardOpen)
            this._phoneAudioPlayedOpen = true;
        // Done, turned off, or the iPhone gone: nothing waited for any more.
        if (!here || route === 'here' || this._link() !== 'linked')
            this._phoneAudioWait = null;
    }

    _phoneAudioDeadlines() {
        const wait = this._phoneAudioWait;
        if (!wait || this._now() >= wait.since + PHONE_AUDIO_WAIT_MS)
            return [];
        return [wait.since + PHONE_AUDIO_WAIT_MS];
    }

    // For the menu: {state, computer, hint}, or null.
    _phoneAudioView() {
        const state = this._phoneAudioState();
        if (state === null)
            return null;
        // The name the iPhone lists: the one of the Bluetooth adapter.
        const adapter = (this._status?.adapters ?? []).find(a => a && a.name);
        return {
            state,
            computer: adapter?.name || this._hostName || '',
            // How to choose this computer on the iPhone: said while it plays,
            // until the menu has been closed once on it.
            hint: state === 'playing' && !this._phoneAudioHintSeen,
        };
    }

    // The menu closes.
    _phoneAudioCardClosed() {
        if (this._phoneAudioPlayedOpen)
            this._phoneAudioHintSeen = true;
        this._phoneAudioPlayedOpen = false;
    }

    _reportSettings() {
        const now = this._now();
        this._reportLinkMode();
        for (const name of ENGINE_SETTINGS) {
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

    // Read-only: what Phone Settings writes under the choice of the mode.
    _reportLinkMode() {
        const state = this._linkModeState();
        if (state === this._linkModeTold)
            return;
        this._linkModeTold = state;
        this.onSetting?.('link-mode-state', state);
    }

    // --- battery -----------------------------------------------------------------

    // The engine says the battery went under 20 % or 10 %: the icon says so
    // until the menu is opened, and the menu as long as it lasts. Nothing
    // pops up. `initial` (already low when the engine started) counts too.
    _onBatteryLow(event) {
        const threshold = Number(event.threshold);
        const level = Number(event.level);
        if (!Number.isFinite(threshold) || !Number.isFinite(level) || this._batteryTold.has(threshold))
            return;
        this._batteryTold.add(threshold);
        // A fall through both at once is one event; do not tell the higher later.
        if (threshold <= 10)
            this._batteryTold.add(20);
        if (!this.batteryAlerts)
            return;
        this._batteryLow = {level, seen: false};
    }
};
