// SPDX-License-Identifier: GPL-2.0-or-later
// Phone Settings: the ready-made replies, call control, the color of the pill,
// the way back for the Bluetooth settings of this computer and, last, forgetting
// the iPhone.

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences, gettext as _} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

import {rootHelper, runAsRoot} from './lib/setupHelper.js';

const REPLIES = 3;

export default class PhonePreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage();

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
            subtitle: _('One quiet notification at 20 % and one at 10 %.'),
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
            description: _('Linking an iPhone may have changed one or two Bluetooth settings of this computer. Undoing them asks for your password and can stop the link from working.'),
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
    }
}
