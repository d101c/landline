// SPDX-License-Identifier: GPL-2.0-or-later
// Ties the engine client, the state model and the views together.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {AppIcons, countryFromLocale} from './lib/appIcons.js';
import {EngineClient, defaultSocketPath} from './lib/engineClient.js';
import {LAYOUT, shellEngineState} from './lib/engineInstall.js';
import {ENGINE_RELEASE} from './lib/engineRelease.js';
import {Illustrations} from './lib/illustrations.js';
import {Indicator} from './lib/indicator.js';
import {Model, STARTUP_COMMANDS} from './lib/model.js';
import {ENGINE_UNIT, LOG_PREFIX} from './lib/names.js';
import {bilingual, phoneLanguage} from './lib/phoneLabels.js';
import {Hotspot, NetworkManagerBackend} from './lib/hotspot.js';
import {QuietWatch} from './lib/quiet.js';
import {AppFinder} from './lib/officialApps.js';
import {INTERFACE_XML, MAX_LINK, OBJECT_PATH, PhoneLinkGate} from './lib/telLinks.js';
import {phoneLinksState, usePhoneLinks} from './lib/phoneLinks.js';
import {SetupDialog} from './lib/setup.js';
import {SetupHelper} from './lib/setupHelper.js';
import {run} from './lib/subprocess.js';

const SETUP_POLL_MS = 2000;
// "Link My iPhone…" in Phone Settings: how long the Shell waits for the
// engine it just installed before giving up on opening the tour.
const TOUR_REQUEST_MS = 30000;

Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'query_info_async');
Gio._promisify(Gio.AppInfo, 'get_default_for_uri_scheme_async', 'get_default_for_uri_scheme_finish');

// After the installed apps or the default apps changed, look again once
// things have settled.
const APPS_SETTLE_MS = 1500;

// Where a promise that nobody awaits ends when it fails; `about` says what
// was being done.
const warn = (e, about = '') => console.warn(`${LOG_PREFIX}: ${about ? `${about}: ` : ''}${e.message}`);

export default class PhoneExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._timerId = 0;
        this._startTimerId = 0;
        this._setupPollId = 0;
        this._tourWanted = 0;
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

        // The official apps of this computer, for the card of a
        // notification (#49): looked up once, again when the apps or their
        // defaults change. Never read synchronously on a notification.
        this._watchApps();
        this._model.setQuietApps(this._settings.get_strv('quiet-apps'));
        this._model.setKeepHistory(this._settings.get_boolean('keep-history'));
        this._model.batteryAlerts = this._settings.get_boolean('battery-alerts');
        // Settings the engine keeps, mirrored for Phone Settings.
        this._model.onSetting = (name, value) => this._mirrorSetting(name, value);

        // The icons of the iPhone apps: from the App Store when Phone
        // Settings allows it, else from the same app on this computer.
        this._appIcons = new AppIcons({
            dir: GLib.build_filenamev([GLib.get_user_cache_dir(), 'landline', 'app-icons']),
            country: countryFromLocale(GLib.get_language_names_with_category('LC_MONETARY'),
                GLib.get_language_names_with_category('LC_TIME'), GLib.get_language_names()),
            enabled: this._settings.get_boolean('show-app-icons'),
            desktopIcon: id => Shell.AppSystem.get_default().lookup_app(`${id}.desktop`)?.get_icon() ?? null,
            onChange: () => this._indicator?.appIconsChanged(),
        });

        this._indicator = new Indicator({
            model: this._model,
            actions: {run: (id, card) => this._runAction(id, card)},
            iconsDir: this.dir.get_child('icons').get_path(),
            gettext: this.gettext.bind(this),
            phoneLabel: key => bilingual(this._illustrations?.language, key),
            phoneArt: name => name === 'hotspot' ? this._illustrations?.hotspot() ?? null : null,
            appIcons: this._appIcons,
        });
        this._indicator.setUseAccent(this._settings.get_boolean('use-accent-color'));
        this._indicator.setIconStyle(this._settings.get_string('icon-style'));
        // Leftmost of the system icons: the pill grows towards the clock and
        // pushes nothing.
        Main.panel.addToStatusArea(this.uuid, this._indicator, 0, 'right');
        // GNOME Shell can destroy the panel's buttons itself while the session
        // closes, before disable(): from then on nothing reaches it (#58).
        const indicator = this._indicator;
        indicator.connect('destroy', () => {
            if (this._indicator === indicator)
                this._indicator = null;
        });

        // The Internet connection of the iPhone, through NetworkManager. The
        // switch of the menu asks the model, which asks for it here once the
        // change it is making is over.
        this._hotspot = new Hotspot(view => this._model?.setHotspot(view));
        this._hotspot.setBackend((onChange, onFailure) => new NetworkManagerBackend(onChange, onFailure));
        this._internetIdleId = 0;
        this._model.phoneLabel = key => bilingual(this._illustrations?.language, key);
        this._model.onInternet = on => {
            this._internetWanted = on;
            if (this._internetIdleId)
                return;
            this._internetIdleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                this._internetIdleId = 0;
                this._hotspot?.set(this._internetWanted);
                return GLib.SOURCE_REMOVE;
            });
        };

        this._model.onChange = () => {
            this._hotspot?.setAddress(this._model.phoneAddress);
            this._tryTourRequest();
            this._indicator?.sync();
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
                this._indicator?.sync();
            },
            'changed::use-accent-color', () => {
                const use = this._settings.get_boolean('use-accent-color');
                this._indicator?.setUseAccent(use);
                this._setDialogAccent(use);
            },
            'changed::icon-style', () => {
                this._indicator?.setIconStyle(this._settings.get_string('icon-style'));
            },
            'changed::call-control', () => {
                this._model.setCallControl(this._settings.get_boolean('call-control'));
            },
            'changed::battery-alerts', () => {
                this._model.batteryAlerts = this._settings.get_boolean('battery-alerts');
                this._indicator?.sync();
            },
            'changed::keep-history', () => {
                this._model.setKeepHistory(this._settings.get_boolean('keep-history'));
            },
            'changed::quiet-apps', () => {
                this._model.setQuietApps(this._settings.get_strv('quiet-apps'));
            },
            'changed::show-app-icons', () => {
                this._appIcons.setEnabled(this._settings.get_boolean('show-app-icons'));
            },
            'changed::skip-running-apps', () => {
                this._model.skipRunningApps = this._settings.get_boolean('skip-running-apps');
            },
            'changed::suggest-apps', () => {
                this._model.suggestApps = this._settings.get_boolean('suggest-apps');
            },
            'changed::suggested-apps', () => {
                this._model.setSuggestedApps(this._settings.get_strv('suggested-apps'));
            },
            'changed::phone-links', () => {
                this._syncPhoneLinks();
                this._syncPhoneLinksOffer();
            },
            'changed::phone-links-offered', () => this._syncPhoneLinksOffer(),
            'changed::phone-audio-here', () => this._model.wantSetting('phone-audio-here',
                this._settings.get_boolean('phone-audio-here')),
            'changed::lock-on-away', () => this._model.wantSetting('lock-on-away',
                this._settings.get_boolean('lock-on-away')),
            'changed::lock-away-seconds', () => this._model.wantSetting('lock-away-seconds',
                this._settings.get_int('lock-away-seconds')),
            'changed::link-mode', () => this._model.wantSetting('link-mode',
                this._settings.get_string('link-mode')),
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
            'changed::forget-request', () => this._onForgetRequest(),
            // Its "Link My iPhone…", once the engine is installed.
            'changed::tour-request', () => this._onTourRequest(),
            // Its "Keep the link steady", while it is not set up: no longer
            // offered in the menu (#51).
            'changed::steady-request', () => this._onSteadyRequest(), this);
        // Nothing is known of the engine yet: Phone Settings shows the choice
        // of the connection once it is.
        if (this._settings.get_string('link-mode-state') !== '')
            this._settings.set_string('link-mode-state', '');
        // A request left over from a session that ended: not one to act on.
        if (this._settings.get_boolean('forget-request'))
            this._settings.set_boolean('forget-request', false);
        if (this._settings.get_boolean('tour-request'))
            this._settings.set_boolean('tour-request', false);
        if (this._settings.get_boolean('steady-request'))
            this._settings.set_boolean('steady-request', false);
        this._readEngineInstall().catch(warn);
        this._phoneLinkGate = new PhoneLinkGate();
        this._phoneLinksHeld = false;
        this._phoneLinks = null;
        this._syncPhoneLinks();
        this._syncPhoneLinksOffer();

        this._client = new EngineClient({
            path: defaultSocketPath(),
            hello: STARTUP_COMMANDS,
            onEvent: event => this._model.handleEvent(event),
            onState: state => this._onEngineState(state),
        });
        this._client.start();
    }

    disable() {
        // The model calls back into the indicator and into this object; those
        // calls stop first. On 18 November a screen lock disabled the
        // extension, clearHistory() below redrew the menu through onChange
        // after the illustrations were gone, the error left disable()
        // half-way and the old indicator stuck in the top bar.
        if (this._model) {
            this._model.onChange = null;
            this._model.onNotification = null;
            this._model.onSetting = null;
            this._model.onInternet = null;
        }
        this._indicator?.destroy();
        this._indicator = null;
        // What is on its way to the App Store is cancelled, the session goes.
        this._appIcons?.destroy();
        this._appIcons = null;
        // A phone link clicked from now on fails at once ("no such object"):
        // nothing is left listening.
        this._phoneLinks?.unexport();
        this._phoneLinks = null;
        this._unwatchApps();
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = 0;
        }
        if (this._startTimerId) {
            GLib.source_remove(this._startTimerId);
            this._startTimerId = 0;
        }
        this._pollSystemSetup(false);
        this._tourWanted = 0;
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
        if (this._internetIdleId) {
            GLib.source_remove(this._internetIdleId);
            this._internetIdleId = 0;
        }
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
            // What "Earlier" kept goes with the extension.
            this._model.clearHistory();
            this._model.phoneLabel = null;
            this._model.phoneArt = null;
            this._model.appRoute = null;
            this._model.appRunning = null;
        }
        // Stopping the client and clearing the history above change the
        // model, which set its timer again: it goes with the rest.
        if (this._timerId) {
            GLib.source_remove(this._timerId);
            this._timerId = 0;
        }
        this._indicator?.destroy();
        this._indicator = null;
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
        } else if (name === 'suggested-apps') {
            s.set_strv(name, value);
        } else if (name === 'link-mode' || name === 'link-mode-state') {
            if (s.get_string(name) !== value)
                s.set_string(name, value);
        } else if (s.get_boolean(name) !== value) {
            s.set_boolean(name, value);
        }
    }

    // --- the official apps of this computer (#49) ---------------------------------

    _watchApps() {
        const apps = Shell.AppSystem.get_default();
        const cancellable = this._cancellable;
        this._finder = new AppFinder({
            // Kept in memory by the Shell.
            lookupApp: id => apps.lookup_app(id)?.get_name() ?? null,
            defaultFor: async scheme => {
                try {
                    const info = await Gio.AppInfo.get_default_for_uri_scheme_async(scheme, cancellable);
                    return info?.get_id() ?? null;
                } catch (e) {
                    return null;
                }
            },
            isRunning: id => apps.lookup_app(id)?.get_state() === Shell.AppState.RUNNING,
        }, () => this._model?.officialAppsChanged());
        this._model.appRoute = bundleId => this._finder?.route(bundleId) ?? null;
        this._model.appRunning = bundleId => this._finder?.running(bundleId) ?? false;
        this._model.skipRunningApps = this._settings.get_boolean('skip-running-apps');
        this._model.suggestApps = this._settings.get_boolean('suggest-apps');
        this._model.setSuggestedApps(this._settings.get_strv('suggested-apps'));
        this._appsSettleId = 0;
        // Installed, removed, or another default chosen.
        Gio.AppInfoMonitor.get().connectObject('changed', () => {
            if (this._appsSettleId)
                GLib.source_remove(this._appsSettleId);
            this._appsSettleId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, APPS_SETTLE_MS, () => {
                this._appsSettleId = 0;
                this._finder?.refresh().catch(warn);
                // Another app may open phone links now, or none any more (#50).
                this._syncPhoneLinksOffer();
                // An app installed or removed lends its icon, or no longer (#55).
                this._appIcons?.appsChanged();
                return GLib.SOURCE_REMOVE;
            });
        }, this);
        this._finder.refresh().catch(warn);
    }

    _unwatchApps() {
        Gio.AppInfoMonitor.get().disconnectObject(this);
        if (this._appsSettleId) {
            GLib.source_remove(this._appsSettleId);
            this._appsSettleId = 0;
        }
        this._finder?.stop();
        this._finder = null;
    }

    // --- phone links (#50) --------------------------------------------------------

    // Phone Settings turned "Use Landline for phone links" on: the desktop
    // file it wrote runs `gdbus call` on this object of the Shell's own
    // connection (lib/telLinks.js). Off, or the extension disabled: no
    // object, and the call fails at once.
    // If the object cannot be exported (another one at that path), the
    // links fail as with the extension disabled; enable() goes on (#56).
    _syncPhoneLinks() {
        const wanted = this._settings.get_boolean('phone-links');
        if (wanted && !this._phoneLinks) {
            const exported = Gio.DBusExportedObject.wrapJSObject(INTERFACE_XML, {
                Call: uri => this._onPhoneLink(uri),
            });
            try {
                exported.export(Gio.DBus.session, OBJECT_PATH);
            } catch (e) {
                warn(e, 'phone links');
                return;
            }
            this._phoneLinks = exported;
        } else if (!wanted && this._phoneLinks) {
            this._phoneLinks.unexport();
            this._phoneLinks = null;
        }
    }

    // A link clicked anywhere: the confirmation drops from the top bar.
    // Nothing is dialled before its "Call".
    // A link too long to be a number still opens the card, which says it
    // has none. Too many links in a short time: said in the journal, once,
    // and not shown (lib/telLinks.js, PhoneLinkGate).
    _onPhoneLink(uri) {
        if (typeof uri !== 'string' || !this._model || !this._indicator)
            return;
        uri = uri.slice(0, MAX_LINK + 1);
        const shown = this._indicator.menu.isOpen && this._model.view().card.kind === 'phonelink';
        const gate = this._phoneLinkGate.take(uri, shown);
        if (gate === 'same')
            return;
        if (gate === 'hold') {
            if (!this._phoneLinksHeld)
                warn(new Error('too many in a short time, the next ones are not shown for a minute'), 'phone links');
            this._phoneLinksHeld = true;
            return;
        }
        this._phoneLinksHeld = false;
        this._model.phoneLink(uri);
        if (!this._indicator.menu.isOpen)
            this._indicator.menu.open();
    }

    // "Use Landline for phone links?" in the menu, once ever (#50): only
    // while the option is off, has not been offered, and no app opens tel:
    // or callto: links. An app that has them is never replaced from here:
    // Phone Settings asks first.
    _syncPhoneLinksOffer() {
        const s = this._settings;
        // A look still on its way is outdated by this one.
        const asked = this._phoneLinksAsked = (this._phoneLinksAsked ?? 0) + 1;
        if (!s || s.get_boolean('phone-links') || s.get_boolean('phone-links-offered')) {
            this._model?.setPhoneLinksOffer(false);
            return;
        }
        phoneLinksState().then(state => {
            if (asked === this._phoneLinksAsked && this._settings)
                this._model?.setPhoneLinksOffer(!state.ours && state.others.length === 0);
        }).catch(warn);
    }

    async _usePhoneLinks() {
        // Looked at again: an app may have taken them since the offer.
        const state = await phoneLinksState();
        if (!this._settings || state.ours)
            return;
        if (state.others.length > 0) {
            this.openPreferences();
            return;
        }
        await usePhoneLinks(this._settings, {
            name: 'Landline',
            comment: this.gettext('Call phone numbers through your iPhone'),
        });
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

    _onTourRequest() {
        if (!this._settings.get_boolean('tour-request'))
            return;
        this._settings.set_boolean('tour-request', false);
        this._tourWanted = Date.now();
        this._client?.retryNow();
        this._readEngineInstall().catch(warn);
        this._tryTourRequest();
    }

    // The tour opens once the engine answers and has said how things are.
    // Cleared before startTour(), which may call back here.
    _tryTourRequest() {
        const wanted = this._tourWanted;
        if (!wanted || !this._model)
            return;
        this._tourWanted = 0;
        if (Date.now() - wanted > TOUR_REQUEST_MS)
            return;
        this._model.startTour();
        if (!this._model.view().tour?.open)
            this._tourWanted = wanted;
    }

    // Whether the engine is installed in the home folder, and its version
    // (engine/VERSION, absent from an install made by hand). Read without
    // blocking, at enable() and each time the engine comes or goes.
    async _readEngineInstall() {
        const cancellable = this._cancellable;
        const dir = Gio.File.new_for_path(GLib.build_filenamev([GLib.get_home_dir(), ...LAYOUT.base, LAYOUT.engine]));
        let present = false;
        try {
            await dir.get_child(LAYOUT.daemon).query_info_async('standard::type',
                Gio.FileQueryInfoFlags.NONE, GLib.PRIORITY_DEFAULT, cancellable);
            present = true;
        } catch (e) {
            present = false;
        }
        let version = null;
        if (present) {
            try {
                const [bytes] = await dir.get_child(LAYOUT.versionFile).load_contents_async(cancellable);
                version = new TextDecoder().decode(bytes).trim();
            } catch (e) {
                version = null;
            }
        }
        if (!cancellable?.is_cancelled())
            this._model?.setEngineInstall(shellEngineState(version, present, ENGINE_RELEASE));
    }

    _onForgetRequest() {
        if (!this._settings.get_boolean('forget-request'))
            return;
        this._settings.set_boolean('forget-request', false);
        this._forget();
    }

    _onSteadyRequest() {
        if (!this._settings.get_boolean('steady-request'))
            return;
        this._settings.set_boolean('steady-request', false);
        this._runAction('steady', null);
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
        this._readEngineInstall().catch(warn);
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
        case 'steady':
            this._indicator?.menu.close();
            this._model.startSteadyTour();
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
        case 'mute':
            this._settings.set_boolean('muted', card.muted === true);
            break;
        case 'settings':
        case 'install-engine':
            // Phone Settings opens on its Engine page by itself when the
            // engine is missing or older than the one it installs.
            this._indicator?.menu.close();
            this.openPreferences();
            break;
        case 'open-app': {
            // The official app of that notification, or its web client.
            const open = card.handoff?.open;
            try {
                if (open?.uri)
                    Gio.AppInfo.launch_default_for_uri(open.uri, global.create_app_launch_context(0, -1));
                else if (open?.app)
                    Shell.AppSystem.get_default().lookup_app(open.app)?.activate();
            } catch (e) {
                console.warn(`${LOG_PREFIX}: could not open the app: ${e.message}`);
            }
            break;
        }
        case 'get-app':
            // Where the official app is found, in the browser.
            try {
                if (/^https:\/\//.test(card.suggestion?.url ?? ''))
                    Gio.AppInfo.launch_default_for_uri(card.suggestion.url, global.create_app_launch_context(0, -1));
            } catch (e) {
                console.warn(`${LOG_PREFIX}: could not open the page of the app: ${e.message}`);
            }
            break;
        case 'use-phone-links':
            this._usePhoneLinks().catch(e => {
                warn(e, 'phone links');
                // Phone Settings says what is wrong and can try again.
                this.openPreferences();
            });
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
