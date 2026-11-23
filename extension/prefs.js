// SPDX-License-Identifier: GPL-2.0-or-later
// Phone Settings: the ready-made replies, how much stays connected, call control, the color of the pill,
// the way back for the Bluetooth settings of this computer and, last, forgetting
// the iPhone.

import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences, gettext as _} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

import {buildEnginePage, engineNeedsAttention} from './lib/enginePage.js';
import {enginePaths, probeEngine} from './lib/engineInstaller.js';
import {stateSvg} from './lib/iconArt.js';
import {phoneLinksState, stopPhoneLinks, usePhoneLinks} from './lib/phoneLinks.js';
import {SetupHelper, rootHelper, runAsRoot} from './lib/setupHelper.js';

const REPLIES = 3;

Gio._promisify(Adw.AlertDialog.prototype, 'choose', 'choose_finish');

// The icon of the top bar, drawn here from the very paths the top bar uses
// (lib/iconArt.js), on a dark tile that stands for the bar: white glyph, and
// the three inks of the stylesheet.
const PREVIEW_INKS = {glyph: '#ffffff', amber: '#f2b45c', green: '#6fd58e', red: '#ff8a7a'};
const PREVIEW_BAR = '#1d1d20';

// One state of one style as a still image, `size` pixels wide. Rendered at
// twice that, so it stays sharp on a dense screen.
function iconPreview(style, state, size) {
    const svg = stateSvg(style, state, PREVIEW_INKS, {size: size * 2, pad: 4, background: PREVIEW_BAR});
    const image = new Gtk.Image({pixel_size: size});
    try {
        image.paintable = Gdk.Texture.new_from_bytes(new GLib.Bytes(new TextEncoder().encode(svg)));
    } catch (e) {
        // No SVG loader on this system: the name under it still says which.
        image.icon_name = 'image-missing-symbolic';
    }
    return image;
}

export default class PhonePreferences extends ExtensionPreferences {
    // "Use Landline for phone links" (#50): a desktop file in the user's
    // applications folder, made the default for tel: and callto: links
    // (lib/phoneLinks.js). A default that belongs to another app is only
    // replaced after asking.
    _phoneLinksRow(window, settings) {
        const about = _('Phone numbers you click in a browser or another app call through the iPhone, after you confirm in the top bar.');
        const row = new Adw.SwitchRow({title: _('Use Landline for phone links'), subtitle: about});
        let syncing = false;
        // A change under way: another click waits for it. The row is not
        // made insensitive, which would take the focus away and scroll the
        // page back to its top.
        let busy = false;
        let closed = false;
        window.connect('close-request', () => {
            closed = true;
            return false;
        });
        const names = others => [...new Set(others.map(o => o.name))].join(', ');
        const show = async () => {
            const state = await phoneLinksState();
            if (closed)
                return;
            syncing = true;
            row.active = state.ours;
            syncing = false;
            row.subtitle = state.ours || state.others.length === 0 ? about
                // Translators: %s is the name of one app or more, like GSConnect.
                : `${about}\n${_('Phone links open in %s now.').replace('%s', names(state.others))}`;
        };
        const ask = async others => {
            const dialog = new Adw.AlertDialog({
                heading: _('Use Landline for phone links?'),
                // Translators: %s is the name of one app or more, like GSConnect.
                body: _('Phone links open in %s now. Landline will open them instead; turning this off gives them back.')
                    .replace('%s', names(others)),
            });
            dialog.add_response('cancel', _('Cancel'));
            dialog.add_response('replace', _('Use Landline'));
            dialog.set_response_appearance('replace', Adw.ResponseAppearance.SUGGESTED);
            dialog.default_response = 'cancel';
            dialog.close_response = 'cancel';
            // The focus comes back to this row when the question closes,
            // and the page stays where it was.
            row.grab_focus();
            return await dialog.choose(window, null) === 'replace';
        };
        row.connect('notify::active', async () => {
            if (syncing)
                return;
            if (busy) {
                syncing = true;
                row.active = !row.active;
                syncing = false;
                return;
            }
            busy = true;
            try {
                if (row.active) {
                    const state = await phoneLinksState();
                    if (state.others.length === 0 || await ask(state.others)) {
                        await usePhoneLinks(settings, {
                            name: 'Landline',
                            comment: _('Call phone numbers through your iPhone'),
                        });
                    }
                } else {
                    await stopPhoneLinks(settings);
                }
                await show();
            } catch (e) {
                console.warn(`Landline: phone links: ${e.message}`);
                await show().catch(() => {});
                row.subtitle = _('The phone links could not be changed.');
            } finally {
                busy = false;
            }
        });
        show().catch(e => console.warn(`Landline: phone links: ${e.message}`));
        return row;
    }

    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage({
            title: _('iPhone'),
            icon_name: 'phone-symbolic',
            name: 'phone',
        });

        const replies = new Adw.PreferencesGroup({
            title: _('Ready-made replies'),
            description: _('Offered under an incoming message. One click sends them.'),
        });
        const rows = [];
        const current = settings.get_strv('quick-replies');
        for (let i = 0; i < REPLIES; i++) {
            const row = new Adw.EntryRow({
                // Translators: %d is 1, 2 or 3.
                title: _('Reply %d').replace('%d', i + 1),
                text: current[i] ?? '',
                show_apply_button: true,
            });
            row.connect('apply', () => {
                settings.set_strv('quick-replies', rows.map(r => r.text.trim()).filter(t => t));
            });
            rows.push(row);
            replies.add(row);
        }
        page.add(replies);

        // How much stays connected. The choice is kept by the phone link
        // service; a version of it with one mode only leaves the row greyed.
        const modes = ['lean', 'full'];
        const connection = new Adw.PreferencesGroup({
            title: _('Connection'),
            description: _('Light keeps this computer linked for notifications only, and connects for messages and calls when they are needed: easier on the battery of the iPhone, and the first message or call takes a few seconds longer. Always connected keeps everything connected all the time.'),
        });
        const mode = new Adw.ComboRow({
            title: _('Connection'),
            model: Gtk.StringList.new([_('Light (recommended)'), _('Always connected')]),
        });
        const showMode = () => {
            const [inForce, reason] = settings.get_string('link-mode-state').split(':');
            const chosen = Math.max(0, modes.indexOf(settings.get_string('link-mode')));
            if (mode.selected !== chosen)
                mode.selected = chosen;
            mode.sensitive = inForce !== '';
            mode.subtitle = inForce === ''
                ? _('The phone link service on this computer keeps everything connected. A newer version offers the choice.')
                : {
                    no_le: _('Everything stays connected for now: this pairing cannot carry notifications by themselves.'),
                    notifications_off: _('Everything stays connected for now: notifications are not shared by the iPhone yet.'),
                    phone_keeps_link: _('Everything stays connected for now: the iPhone keeps reopening the link. Light is tried again later.'),
                }[reason] ?? '';
        };
        showMode();
        mode.connect('notify::selected', () => {
            const wanted = modes[mode.selected] ?? 'lean';
            if (mode.sensitive && settings.get_string('link-mode') !== wanted)
                settings.set_string('link-mode', wanted);
        });
        const modeIds = [
            settings.connect('changed::link-mode', showMode),
            settings.connect('changed::link-mode-state', showMode),
        ];
        window.connect('close-request', () => {
            modeIds.forEach(id => settings.disconnect(id));
            return false;
        });
        connection.add(mode);
        page.add(connection);

        const sound = new Adw.PreferencesGroup({title: _('Sound')});
        const audio = new Adw.SwitchRow({
            title: _('Play iPhone audio on this computer'),
            subtitle: _('Music and sounds of the iPhone come out of this computer. Off: they stay on the iPhone. Calls are chosen one by one when you answer.'),
        });
        settings.bind('phone-audio-here', audio, 'active', Gio.SettingsBindFlags.DEFAULT);
        sound.add(audio);
        page.add(sound);

        const calls = new Adw.PreferencesGroup({title: _('Calls')});
        const control = new Adw.SwitchRow({
            title: _('Answer and decline calls from this computer'),
            subtitle: _('Shows who is calling in the top bar. The call itself stays on the iPhone unless you move the sound here.'),
        });
        settings.bind('call-control', control, 'active', Gio.SettingsBindFlags.DEFAULT);
        calls.add(control);
        calls.add(this._phoneLinksRow(window, settings));
        page.add(calls);

        const quiet = new Adw.PreferencesGroup({
            title: _('Quiet times'),
            description: _('While quiet, nothing from the iPhone pops up: no name, no text, no call card. Unread messages are only counted, and the menu still shows everything when you open it.'),
        });
        for (const [key, title, subtitle] of [
            ['follow-do-not-disturb', _('Stay quiet in Do Not Disturb'),
                _('Follows the Do Not Disturb switch of the system menu.')],
            ['quiet-while-sharing', _('Stay quiet while the screen is shared or recorded'),
                _('So nothing personal shows up in a presentation or a video call.')],
            ['calls-through-do-not-disturb', _('Let calls through in Do Not Disturb'),
                _('Incoming calls still show while Do Not Disturb is on. Never while the screen is shared.')],
            ['muted', _('Mute iPhone on this computer'),
                _('Complete silence, whatever the situation, until you turn this off.')],
        ]) {
            const row = new Adw.SwitchRow({title, subtitle});
            settings.bind(key, row, 'active', Gio.SettingsBindFlags.DEFAULT);
            quiet.add(row);
        }
        page.add(quiet);

        // Apps that never grow the pill: their notifications are only listed
        // in the menu.
        const apps = new Adw.PreferencesGroup({
            title: _('Notifications of other apps'),
            description: _('A notification of an iPhone app shows in the top bar for a few seconds. The apps listed here never do: their notifications only wait in the menu.'),
        });
        const add = new Adw.EntryRow({
            title: _('Add an app by its name, like WhatsApp'),
            show_apply_button: true,
        });
        apps.add(add);
        let appRows = [];
        const showApps = () => {
            appRows.forEach(row => apps.remove(row));
            appRows = settings.get_strv('quiet-apps').map(name => {
                const row = new Adw.ActionRow({title: name});
                const remove = new Gtk.Button({
                    icon_name: 'edit-delete-symbolic',
                    valign: Gtk.Align.CENTER,
                    // Translators: %s is the name of an app.
                    tooltip_text: _('Let %s show in the top bar again').replace('%s', name),
                });
                remove.add_css_class('flat');
                remove.connect('clicked', () => {
                    settings.set_strv('quiet-apps', settings.get_strv('quiet-apps').filter(n => n !== name));
                });
                row.add_suffix(remove);
                apps.add(row);
                return row;
            });
        };
        add.connect('apply', () => {
            const name = add.text.trim();
            const list = settings.get_strv('quiet-apps');
            if (name && !list.some(n => n.toLowerCase() === name.toLowerCase()))
                settings.set_strv('quiet-apps', [...list, name]);
            add.text = '';
        });
        // Off by default: nothing of a notification outlives it.
        const history = new Adw.SwitchRow({
            title: _('Keep earlier notifications'),
            subtitle: _('Notifications that left the iPhone, read or dismissed, stay listed under “Earlier” in the menu: the last 12 hours, 200 at most. They are kept in the memory of this session only, never written to disk, and are forgotten when the screen locks, when you log out or when you turn this off.'),
        });
        settings.bind('keep-history', history, 'active', Gio.SettingsBindFlags.DEFAULT);
        apps.add(history);
        // On by default; the one thing of the notifications that leaves this
        // computer, and only the name of the app.
        const appIcons = new Adw.SwitchRow({
            title: _('Show app icons'),
            subtitle: _('To show the real icon of an iPhone app, its identifier (like net.whatsapp.WhatsApp) is sent to Apple’s App Store, once per app; nothing of the notification itself is sent. The icon is then kept on this computer. Turned off, nothing is sent: apps also installed here show their icon, the others their initials.'),
        });
        settings.bind('show-app-icons', appIcons, 'active', Gio.SettingsBindFlags.DEFAULT);
        apps.add(appIcons);
        // The official apps of this computer (#49).
        const skip = new Adw.SwitchRow({
            title: _('Leave notifications to apps open on this computer'),
            subtitle: _('When Signal, Telegram, Discord, Slack or Teams is open here and shows its own notifications, theirs from the iPhone no longer show in the top bar. They still wait in the menu.'),
        });
        settings.bind('skip-running-apps', skip, 'active', Gio.SettingsBindFlags.DEFAULT);
        apps.add(skip);
        const suggest = new Adw.SwitchRow({
            title: _('Suggest official apps'),
            subtitle: _('When the official app of a notification is not on this computer, its card says so once, with where to get it.'),
        });
        settings.bind('suggest-apps', suggest, 'active', Gio.SettingsBindFlags.DEFAULT);
        // Turned on again: each app may be suggested once more.
        suggest.connect('notify::active', () => {
            if (suggest.active)
                settings.set_strv('suggested-apps', []);
        });
        apps.add(suggest);
        showApps();
        const appsChangedId = settings.connect('changed::quiet-apps', showApps);
        window.connect('close-request', () => {
            settings.disconnect(appsChangedId);
            return false;
        });
        page.add(apps);

        const away = new Adw.PreferencesGroup({
            title: _('When I walk away'),
            description: _('The iPhone is noticed gone 20 to 35 seconds after its signal is lost; the delay below starts then. Turning Bluetooth off on the iPhone, or putting this computer to sleep, does not lock. Nothing is ever unlocked automatically: you unlock as usual when you come back.'),
        });
        const lock = new Adw.SwitchRow({
            title: _('Lock this computer when I walk away with my iPhone'),
        });
        settings.bind('lock-on-away', lock, 'active', Gio.SettingsBindFlags.DEFAULT);
        away.add(lock);
        const delay = new Adw.SpinRow({
            title: _('Delay before locking'),
            subtitle: _('Seconds after the iPhone is noticed gone.'),
            adjustment: new Gtk.Adjustment({lower: 5, upper: 600, step_increment: 5, page_increment: 30}),
        });
        // The key is an integer, the row holds a double: kept in step by hand.
        delay.value = settings.get_int('lock-away-seconds');
        delay.connect('notify::value', () => {
            const seconds = Math.round(delay.value);
            if (settings.get_int('lock-away-seconds') !== seconds)
                settings.set_int('lock-away-seconds', seconds);
        });
        const delayChangedId = settings.connect('changed::lock-away-seconds', () => {
            const seconds = settings.get_int('lock-away-seconds');
            if (Math.round(delay.value) !== seconds)
                delay.value = seconds;
        });
        window.connect('close-request', () => {
            settings.disconnect(delayChangedId);
            return false;
        });
        settings.bind('lock-on-away', delay, 'sensitive', Gio.SettingsBindFlags.GET);
        away.add(delay);
        page.add(away);

        const battery = new Adw.PreferencesGroup({title: _('Battery')});
        const low = new Adw.SwitchRow({
            title: _('Tell me when the iPhone battery is low'),
            subtitle: _('The icon of the top bar and the menu say so at 20 % and at 10 %. Nothing pops up.'),
        });
        settings.bind('battery-alerts', low, 'active', Gio.SettingsBindFlags.DEFAULT);
        battery.add(low);
        page.add(battery);

        const look = new Adw.PreferencesGroup({title: _('Appearance')});
        const accent = new Adw.SwitchRow({
            title: _('Use the system accent color'),
            subtitle: _('Instead of amber, for the pill and the buttons.'),
        });
        settings.bind('use-accent-color', accent, 'active', Gio.SettingsBindFlags.DEFAULT);
        look.add(accent);
        // The drawing in the top bar: two cards side by side, each showing
        // its style large, then how the main states look. The one in use is
        // pressed in and ticked. Its colors do not follow the accent.
        // What happens, then what is not well: a mark on the same drawing.
        const states = [
            ['unread', _('Unread message')],
            ['ringing', _('Ringing')],
            ['call', _('On a call')],
            ['missed', _('Missed call')],
        ];
        const marks = [
            ['reconnecting', _('Reconnecting')],
            ['away', _('Out of range')],
            ['paused', _('Notifications paused')],
            ['attention', _('Something to do: open the menu')],
            ['battery', _('Battery low')],
            ['internet_failed', _('Internet from iPhone did not connect')],
        ];
        const cards = new Gtk.Box({
            spacing: 12,
            homogeneous: true,
            margin_top: 12, margin_bottom: 12, margin_start: 12, margin_end: 12,
        });
        const choices = new Map();
        for (const [style, name] of [['phone', _('Phone')], ['handset', _('Handset')]]) {
            const content = new Gtk.Box({
                orientation: Gtk.Orientation.VERTICAL,
                spacing: 10,
                margin_top: 10, margin_bottom: 10, margin_start: 6, margin_end: 6,
            });
            const large = iconPreview(style, 'linked', 96);
            large.halign = Gtk.Align.CENTER;
            content.append(large);
            const title = new Gtk.Box({spacing: 6, halign: Gtk.Align.CENTER});
            const tick = new Gtk.Image({icon_name: 'object-select-symbolic'});
            title.append(tick);
            const label = new Gtk.Label({label: name});
            label.add_css_class('heading');
            title.append(label);
            content.append(title);
            for (const list of [states, marks]) {
                const row = new Gtk.Box({spacing: 6, halign: Gtk.Align.CENTER});
                for (const [state, what] of list) {
                    const small = iconPreview(style, state, 32);
                    small.tooltip_text = what;
                    row.append(small);
                }
                content.append(row);
            }
            const card = new Gtk.ToggleButton({child: content, tooltip_text: name});
            card.add_css_class('card');
            card.connect('toggled', () => {
                tick.opacity = card.active ? 1 : 0;
                if (card.active && settings.get_string('icon-style') !== style)
                    settings.set_string('icon-style', style);
            });
            choices.set(style, {card, tick});
            cards.append(card);
        }
        // One of the two, never none.
        choices.get('handset').card.group = choices.get('phone').card;
        const showStyle = () => {
            const chosen = choices.get(settings.get_string('icon-style')) ?? choices.get('phone');
            chosen.card.active = true;
            for (const {card, tick} of choices.values())
                tick.opacity = card.active ? 1 : 0;
        };
        showStyle();
        const styleChangedId = settings.connect('changed::icon-style', showStyle);
        window.connect('close-request', () => {
            settings.disconnect(styleChangedId);
            return false;
        });
        const icon = new Adw.PreferencesRow({
            title: _('Icon'),
            activatable: false,
            child: cards,
        });
        look.add(icon);
        page.add(look);

        const reset = new Gtk.Button({label: _('Reset'), valign: Gtk.Align.CENTER});
        reset.add_css_class('flat');
        reset.connect('clicked', () => {
            settings.reset('quick-replies');
            const values = settings.get_strv('quick-replies');
            rows.forEach((row, i) => (row.text = values[i] ?? ''));
        });
        replies.set_header_suffix(reset);

        const phone = new Adw.PreferencesGroup({title: _('iPhone')});
        // The words of the iPhone are not translated here: they are what
        // the iPhone itself writes.
        const codes = ['', 'en', 'fr'];
        const language = new Adw.ComboRow({
            title: _('Language of the iPhone'),
            subtitle: _('The setup draws the iPhone screens with the words your iPhone uses.'),
            model: Gtk.StringList.new([_('Same as this computer'), 'English', 'Français']),
            selected: Math.max(0, codes.indexOf(settings.get_string('phone-language'))),
        });
        language.connect('notify::selected', () => {
            settings.set_string('phone-language', codes[language.selected] ?? '');
        });
        phone.add(language);
        page.add(phone);

        // The one-time Bluetooth settings can be put back as they were, by
        // the helper installed in a directory of root. Without it the group
        // stays hidden: there is nothing this window may run as administrator.
        const system = new Adw.PreferencesGroup({
            title: _('This computer'),
            description: _('Linking an iPhone may have changed one or two Bluetooth settings of this computer, and set up the steadier link. Undoing them asks for your password and can stop the link from working.'),
            visible: false,
        });
        const row = new Adw.ActionRow({title: _('Undo the Bluetooth changes')});
        const undo = new Gtk.Button({label: _('Undo'), valign: Gtk.Align.CENTER});
        const cancellable = new Gio.Cancellable();
        window.connect('close-request', () => {
            cancellable.cancel();
            return false;
        });
        let helper = null;
        rootHelper(cancellable).then(path => {
            helper = path;
            system.visible = path !== null && !cancellable.is_cancelled();
        });
        undo.connect('clicked', () => {
            undo.sensitive = false;
            runAsRoot(helper, 'revert', cancellable).then(result => {
                if (cancellable.is_cancelled())
                    return;
                undo.sensitive = true;
                row.subtitle = {
                    done: _('The Bluetooth settings are back as they were.'),
                    partial: _('Put back, but Bluetooth could not be restarted. Restart this computer.'),
                    refused: _('No password, so nothing was changed.'),
                }[result] ?? _('The Bluetooth settings could not be put back.');
            });
        });
        row.add_suffix(undo);
        system.add(row);
        // The steadier link: offered here, and in the setup, only while it
        // is missing; the menu no longer offers it (#51). The Shell opens
        // the setup step that explains it and asks for the password.
        const steady = new Adw.ActionRow({
            title: _('Keep the link steady'),
            subtitle: _('Lets this computer ask the iPhone for a more tolerant Bluetooth connection. Optional.'),
            visible: false,
        });
        const steadyButton = new Gtk.Button({label: _('Set Up…'), valign: Gtk.Align.CENTER});
        steadyButton.connect('clicked', () => {
            settings.set_boolean('steady-request', true);
            window.close();
        });
        steady.add_suffix(steadyButton);
        system.add(steady);
        const setup = new SetupHelper();
        window.connect('close-request', () => {
            setup.destroy();
            return false;
        });
        setup.status().then(state => {
            // null as well once the window is closed.
            if (state?.elevation === true && state.le_params === false)
                steady.visible = true;
        });
        page.add(system);

        // Last, alone, and red: nothing else on this page undoes the link.
        const danger = new Adw.PreferencesGroup({
            description: _('Removes the link on this computer, then shows how to do the same on the iPhone. You are asked to confirm before anything is removed.'),
        });
        const forget = new Adw.ButtonRow({title: _('Forget This iPhone…')});
        forget.add_css_class('destructive-action');
        forget.connect('activated', () => {
            // The question is asked by the Shell, over everything else.
            settings.set_boolean('forget-request', true);
            window.close();
        });
        danger.add(forget);
        page.add(danger);

        window.add(page);

        // Install, update, go back or remove the engine. Shown first when
        // the engine is missing or older than the one this version
        // installs, or does not answer.
        const paths = enginePaths();
        const engine = buildEnginePage({window, settings, dir: this.dir, gettext: _, paths});
        window.add(engine);
        if (engineNeedsAttention(paths)) {
            window.visible_page = engine;
        } else {
            let closed = false;
            window.connect('close-request', () => {
                closed = true;
                return false;
            });
            probeEngine(paths.socket, 1500).then(answered => {
                if (!answered && !closed && window.visible_page === page)
                    window.visible_page = engine;
            });
        }
    }
}
