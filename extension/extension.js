// SPDX-License-Identifier: GPL-2.0-or-later
// Ties the engine client, the state model and the views together.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {EngineClient, defaultSocketPath} from './lib/engineClient.js';
import {Illustrations} from './lib/illustrations.js';
import {Indicator} from './lib/indicator.js';
import {Model, STARTUP_COMMANDS} from './lib/model.js';
import {ENGINE_UNIT, LOG_PREFIX} from './lib/names.js';
import {bilingual, phoneLanguage} from './lib/phoneLabels.js';
import {Hotspot, NetworkManagerBackend} from './lib/hotspot.js';
import {NotificationBridge} from './lib/notifications.js';
import {QuietWatch} from './lib/quiet.js';
import {replyLink} from './lib/replyLinks.js';
import {SetupDialog} from './lib/setup.js';
import {SetupHelper} from './lib/setupHelper.js';
import {run} from './lib/subprocess.js';

const SETUP_POLL_MS = 2000;

// Where a promise that nobody awaits ends when it fails.
const warn = e => console.warn(`${LOG_PREFIX}: ${e.message}`);

export default class PhoneExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._timerId = 0;
        this._startTimerId = 0;
        this._setupPollId = 0;
        this._startTried = false;
        this._cancellable = new Gio.Cancellable();
        this._tourDialog = null;
        // Every dialog not destroyed yet: the one shown, and one still
        // fading out after it was closed.
        this._dialogs = new Set();
        this._applying = false;
        this._restarting = false;

        this._model = new Model({
            send: command => this._client?.send(command),
            translate: this.gettext.bind(this),
            quickReplies: this._quickReplies(),
            hostName: GLib.get_host_name(),
            callControl: this._settings.get_boolean('call-control'),
        });
        this._illustrations = new Illustrations(this.dir.get_child('illustrations'),
            this._phoneLanguage(), () => this._tourDialog?.relayout());

        this._bridge = new NotificationBridge({
            onDismissed: uid => this._model?.clearNotification(uid),
            replyLabel: this.gettext('Reply in app'),
        });
        this._model.onNotification = (kind, payload) => {
            if (kind === 'reply') {
                const link = replyLink(payload.appId, payload.number);
                if (link)
                    this._bridge.handle('reply', {uid: payload.uid, ...link});
                return;
            }
            this._bridge.handle(kind, payload);
        };
        this._model.batteryAlerts = this._settings.get_boolean('battery-alerts');
        this._model.onAlert = alert => this._onAlert(alert);
        // Settings the engine keeps, mirrored for Phone Settings.
        this._model.onSetting = (name, value) => this._mirrorSetting(name, value);

        this._indicator = new Indicator({
            model: this._model,
            actions: {run: (id, card) => this._runAction(id, card)},
            iconsDir: this.dir.get_child('icons').get_path(),
            gettext: this.gettext.bind(this),
            phoneLabel: key => bilingual(this._illustrations.language, key),
        });
        this._indicator.setUseAccent(this._settings.get_boolean('use-accent-color'));
        // Leftmost of the system icons: the pill grows towards the clock and
        // pushes nothing.
        Main.panel.addToStatusArea(this.uuid, this._indicator, 0, 'right');

        // The Internet connection of the iPhone, through NetworkManager.
        this._hotspot = new Hotspot(view => this._model?.setHotspot(view));
        this._hotspot.setBackend(onChange => new NetworkManagerBackend(onChange));

        this._model.onChange = () => {
            this._hotspot?.setAddress(this._model.phoneAddress);
            this._indicator.sync();
            this._syncTourDialog();
            this._schedule();
        };

        // Do Not Disturb, a shared screen, and our own mute switch.
        this._quiet = new QuietWatch(facts => this._model?.setQuiet(facts));
        this._syncQuietSettings();

        this._setupAsked = 0;
        this._setupPolling = false;
        this._helper = new SetupHelper();
        this._refreshSystemSetup().catch(warn);

        this._settings.connectObject(
            'changed::quick-replies', () => {
                this._model.quickReplies = this._quickReplies();
                this._indicator.sync();
            },
            'changed::use-accent-color', () => {
                const use = this._settings.get_boolean('use-accent-color');
                this._indicator.setUseAccent(use);
                this._setDialogAccent(use);
            },
            'changed::call-control', () => {
                this._model.setCallControl(this._settings.get_boolean('call-control'));
            },
            'changed::battery-alerts', () => {
                this._model.batteryAlerts = this._settings.get_boolean('battery-alerts');
            },
            'changed::phone-audio-here', () => this._model.wantSetting('phone-audio-here',
                this._settings.get_boolean('phone-audio-here')),
            'changed::lock-on-away', () => this._model.wantSetting('lock-on-away',
                this._settings.get_boolean('lock-on-away')),
            'changed::lock-away-seconds', () => this._model.wantSetting('lock-away-seconds',
                this._settings.get_int('lock-away-seconds')),
            'changed::muted', () => this._syncQuietSettings(),
            'changed::follow-do-not-disturb', () => this._syncQuietSettings(),
            'changed::quiet-while-sharing', () => this._syncQuietSettings(),
            'changed::calls-through-do-not-disturb', () => this._syncQuietSettings(),
            'changed::phone-language', () => {
                this._illustrations.setLanguage(this._phoneLanguage());
                this._tourDialog?.relayout();
            },
            // Phone Settings runs in its own process: its "Forget This
            // iPhone…" button raises this flag, and the Shell does the rest.
            'changed::forget-request', () => this._onForgetRequest(), this);
        // A request left over from a session that ended: not one to act on.
        if (this._settings.get_boolean('forget-request'))
            this._settings.set_boolean('forget-request', false);

        this._client = new EngineClient({
            path: defaultSocketPath(),
            hello: STARTUP_COMMANDS,
            onEvent: event => this._model.handleEvent(event),
            onState: state => this._onEngineState(state),
        });
        this._client.start();
    }

    disable() {
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = 0;
        }
        if (this._startTimerId) {
            GLib.source_remove(this._startTimerId);
            this._startTimerId = 0;
        }
        this._pollSystemSetup(false);
        this._cancellable?.cancel();
        this._cancellable = null;
        this._client?.stop();
        this._client = null;
        this._helper?.destroy();
        this._helper = null;
        this._setupPolling = false;
        this._setupAsked = 0;
        this._startTried = false;
        this._quiet?.destroy();
        this._quiet = null;
        this._hotspot?.destroy();
        this._hotspot = null;
        this._tourDialog = null;
        for (const dialog of this._dialogs ?? []) {
            dialog.disconnectObject(this);
            dialog.destroy();
        }
        this._dialogs = null;
        this._illustrations?.destroy();
        this._illustrations = null;
        this._applying = false;
        this._restarting = false;
        this._settings?.disconnectObject(this);
        this._settings = null;
        if (this._model) {
            this._model.onChange = null;
            this._model.onNotification = null;
            this._model.onAlert = null;
            this._model.onSetting = null;
        }
        this._indicator?.destroy();
        this._indicator = null;
        this._bridge?.destroy();
        this._bridge = null;
        this._model = null;
    }

    // The engine holds another value than the key of Phone Settings.
    _mirrorSetting(name, value) {
        const s = this._settings;
        if (!s)
            return;
        if (name === 'lock-away-seconds') {
            const seconds = Math.min(600, Math.max(5, value));
            if (s.get_int(name) !== seconds)
                s.set_int(name, seconds);
        } else if (s.get_boolean(name) !== value) {
            s.set_boolean(name, value);
        }
    }

    _onAlert(alert) {
        if (alert.kind !== 'battery')
            return;
        // Translators: %d is the battery level of the iPhone.
        this._bridge?.notice(this.gettext('iPhone battery at %d %%').format(alert.level), '');
    }

    _syncQuietSettings() {
        const s = this._settings;
        this._model.setQuiet({
            muted: s.get_boolean('muted'),
            followDnd: s.get_boolean('follow-do-not-disturb'),
            whileSharing: s.get_boolean('quiet-while-sharing'),
            callsThroughDnd: s.get_boolean('calls-through-do-not-disturb'),
        });
    }

    _quickReplies() {
        const _ = this.gettext.bind(this);
        const replies = this._settings.get_strv('quick-replies').map(s => s.trim()).filter(s => s);
        // The defaults of the schema are the English texts: show them translated.
        return replies.map(text => _(text));
    }

    // The language the drawings of the iPhone are written in: the choice of
    // Phone Settings, or a guess from the languages of this desktop.
    _phoneLanguage() {
        return phoneLanguage(this._settings.get_string('phone-language'),
            GLib.get_language_names(), GLib.get_language_names_with_category('LC_TIME'));
    }

    _onForgetRequest() {
        if (!this._settings.get_boolean('forget-request'))
            return;
        this._settings.set_boolean('forget-request', false);
        this._forget();
    }

    _forget() {
        this._indicator?.menu.close();
        if (!this._model.startForget()) {
            Main.notify(this.gettext('Landline'),
                this.gettext('The phone link is not running, so there is nothing to forget right now.'));
        }
    }

    // One timer, set for the next thing the model is waiting for: the end of
    // a pill, the next second of a call.
    _schedule() {
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = 0;
        }
        const deadline = this._model?.nextDeadline();
        if (deadline === null || deadline === undefined)
            return;
        const delay = Math.max(20, deadline - Date.now());
        this._timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delay, () => {
            this._timerId = 0;
            this._model?.expire();
            return GLib.SOURCE_REMOVE;
        });
    }

    // --- engine ----------------------------------------------------------------

    // The socket does not answer: ask systemd for the engine once, and only
    // say "not running" if that does not bring it up.
    _onEngineState(state) {
        if (state === 'connected') {
            this._startTried = false;
            this._model.setEngineState('connected');
            return;
        }
        // "Fix It" is restarting the engine: it is expected to go away, and
        // it is not ours to start a second time.
        if (this._restarting || this._model.fixing) {
            this._model.setEngineState('absent');
            return;
        }
        if (this._startTried) {
            this._model.setEngineState('absent');
            return;
        }
        this._startTried = true;
        this._startEngine().catch(warn);
    }

    async _startEngine() {
        const cancellable = this._cancellable;
        let started = false;
        try {
            const {exited, status} = await run(['systemctl', '--user', 'start', ENGINE_UNIT], cancellable);
            started = exited && status === 0;
        } catch (e) {
            started = false;
        }
        if (cancellable.is_cancelled() || !this._client)
            return;
        if (!started) {
            if (this._client.state !== 'connected')
                this._model.setEngineState('absent');
            return;
        }
        // Started: give it a moment to open its socket, then look.
        this._client.retryNow();
        if (this._startTimerId)
            GLib.source_remove(this._startTimerId);
        this._startTimerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 3000, () => {
            this._startTimerId = 0;
            this._client?.retryNow();
            if (this._client && this._client.state !== 'connected')
                this._model?.setEngineState('absent');
            return GLib.SOURCE_REMOVE;
        });
    }

    // "Fix It": this computer is not offering messages and contacts (the
    // engine lost the system service that carries them). A restart of the
    // engine puts that right; the model watches what comes back.
    async _restartEngine() {
        if (this._restarting || !this._client)
            return;
        this._restarting = true;
        this._indicator?.menu.close();
        this._model.fixStarted();
        const cancellable = this._cancellable;
        let restarted = false;
        try {
            const {exited, status} = await run(['systemctl', '--user', 'restart', ENGINE_UNIT], cancellable);
            restarted = exited && status === 0;
        } catch (e) {
            restarted = false;
        }
        if (cancellable.is_cancelled() || !this._client)
            return;
        this._restarting = false;
        if (restarted)
            this._client.retryNow();
        else
            this._model.fixFailed();
    }

    // --- permission ------------------------------------------------------------

    // Several of these may be on their way at once (enable(), the end of an
    // "apply", the two-second poll): only the latest one asked is kept.
    async _refreshSystemSetup() {
        const helper = this._helper;
        const asked = ++this._setupAsked;
        const status = await helper.status();
        if (this._helper === helper && asked === this._setupAsked)
            this._model.setSystemSetup(status);
        return status;
    }

    // While the tour shows commands to run by hand, look every two seconds
    // whether they were: the tour then goes on by itself. A look that has
    // not come back yet is not asked a second time.
    _pollSystemSetup(wanted) {
        if (!wanted) {
            if (this._setupPollId) {
                GLib.source_remove(this._setupPollId);
                this._setupPollId = 0;
            }
            return;
        }
        if (this._setupPollId)
            return;
        this._setupPollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, SETUP_POLL_MS, () => {
            if (this._setupPolling)
                return GLib.SOURCE_CONTINUE;
            this._setupPolling = true;
            const helper = this._helper;
            this._refreshSystemSetup().catch(warn).finally(() => {
                if (this._helper === helper)
                    this._setupPolling = false;
            });
            return GLib.SOURCE_CONTINUE;
        });
    }

    // Writes to the clipboard, on a click on "Copy Commands". Never reads it.
    _copyCommands(commands, hint = this.gettext('Paste them in a terminal, then restart Bluetooth.')) {
        St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, (commands ?? []).join('\n'));
        Main.notify(this.gettext('Commands copied'), hint);
    }

    // The password window is the one of the system. The tour stays up
    // behind it, says it is waiting, and goes on by itself when it is over.
    async _applyPermission() {
        const helper = this._helper;
        if (!helper || this._applying)
            return;
        this._applying = true;
        this._model.tourPermission('waiting');
        const result = await helper.apply();
        if (this._helper !== helper)
            return;
        await this._refreshSystemSetup();
        if (this._helper !== helper)
            return;
        this._applying = false;
        // 'done', 'partial', 'refused' or 'failed': the tour says it in words.
        this._model.tourPermission(result);
    }

    // --- setup tour --------------------------------------------------------------

    _syncTourDialog() {
        const tour = this._model.view().tour;
        this._pollSystemSetup(tour?.open === true && tour.step === 'permission' &&
            tour.permissionManual && tour.permission !== 'done');
        if (tour?.open && !this._tourDialog) {
            const dialog = new SetupDialog({
                model: this._model,
                actions: {
                    applyPermission: () => this._applyPermission().catch(warn),
                    restartEngine: () => this._restartEngine().catch(warn),
                    copyCommands: commands => this._copyCommands(commands,
                        this.gettext('Paste them in a terminal.')),
                },
                illustrations: this._illustrations,
                gettext: this.gettext.bind(this),
            });
            this._tourDialog = dialog;
            this._dialogs.add(dialog);
            this._setDialogAccent(this._settings.get_boolean('use-accent-color'));
            dialog.connectObject('destroy', () => {
                this._dialogs.delete(dialog);
                if (this._tourDialog !== dialog)
                    return;
                // Closed by something else than the model: put the tour aside.
                this._tourDialog = null;
                this._model?.closeTour();
            }, this);
            dialog.update(tour);
            if (!dialog.open()) {
                this._tourDialog = null;
                dialog.destroy();
                this._model.closeTour();
            }
        } else if (tour?.open) {
            this._tourDialog.update(tour);
        } else if (this._tourDialog) {
            const dialog = this._tourDialog;
            this._tourDialog = null;
            dialog.close();
        }
    }

    _setDialogAccent(use) {
        const dialog = this._tourDialog;
        if (!dialog)
            return;
        if (use)
            dialog.add_style_class_name('landline-system-accent');
        else
            dialog.remove_style_class_name('landline-system-accent');
    }

    _runAction(id, card) {
        switch (id) {
        case 'permission':
        case 'link':
            this._indicator?.menu.close();
            this._model.startTour();
            break;
        case 'retry':
            this._client?.retryNow();
            break;
        case 'fix':
            this._restartEngine().catch(warn);
            break;
        case 'forget':
            this._forget();
            break;
        case 'hotspot':
            this._hotspot?.set(card.on === true);
            break;
        case 'mute':
            this._settings.set_boolean('muted', card.muted === true);
            break;
        case 'settings':
            this._indicator?.menu.close();
            this.openPreferences();
            break;
        case 'copy-commands':
            this._indicator?.menu.close();
            this._copyCommands(card.commands);
            break;
        default:
            break;
        }
    }
}
