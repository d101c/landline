// SPDX-License-Identifier: GPL-2.0-or-later
// The setup tour: one dialog that walks from the first click to a working
// link. It only shows view().tour of the model; every step says where to be
// on the iPhone and what to do there, with a drawing of that screen.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import St from 'gi://St';

import * as Animation from 'resource:///org/gnome/shell/ui/animation.js';
import * as ModalDialog from 'resource:///org/gnome/shell/ui/modalDialog.js';

import {bilingual} from './phoneLabels.js';

const STEP_FADE_MS = 220;
// One drawing has the room to be large; two share it.
const PHONE_SIZE = 236;
const PHONE_PAIR_SIZE = 208;

function wrapped(text, styleClass, centered = false) {
    const label = new St.Label({text, style_class: styleClass, x_expand: true});
    label.clutter_text.line_wrap = true;
    label.clutter_text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
    label.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
    if (centered)
        label.clutter_text.line_alignment = Pango.Alignment.CENTER;
    return label;
}

function badge(text) {
    const label = new St.Label({
        text,
        style_class: 'landline-avatar landline-step-number',
        y_align: Clutter.ActorAlign.START,
    });
    label.clutter_text.x_align = Clutter.ActorAlign.CENTER;
    label.clutter_text.y_align = Clutter.ActorAlign.CENTER;
    return label;
}

export const SetupDialog = GObject.registerClass(
class PhoneSetupDialog extends ModalDialog.ModalDialog {
    // actions.applyPermission(): ask for the password and change the settings.
    // actions.restartEngine(): "Fix It", restart the engine.
    _init({model, actions, illustrations, gettext}) {
        super._init({styleClass: 'landline-tour'});
        this._model = model;
        this._actions = actions;
        this._art = illustrations;
        this._ = gettext;
        this._key = null;
        this._step = null;
        this._helpOpen = false;
        this._focusId = 0;

        // Always there: where we are, and where to be on the iPhone.
        const band = new St.BoxLayout({
            style_class: 'landline-tour-band',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        this._stepLabel = new St.Label({style_class: 'landline-tour-step'});
        band.add_child(this._stepLabel);
        this._track = new St.Widget({style_class: 'landline-tour-track', x_expand: true});
        this._fill = new St.Widget({style_class: 'landline-tour-fill'});
        this._track.add_child(this._fill);
        this._track.connect('notify::width', () => this._syncProgress(false));
        band.add_child(this._track);
        const where = new St.BoxLayout({style_class: 'landline-tour-where'});
        where.add_child(new St.Label({
            text: gettext('On your iPhone:'),
            style_class: 'landline-tour-where-key',
            y_align: Clutter.ActorAlign.START,
            x_align: Clutter.ActorAlign.START,
        }));
        this._whereLabel = wrapped('', 'landline-tour-where-text');
        where.add_child(this._whereLabel);
        band.add_child(where);
        this.contentLayout.add_child(band);

        this._body = new St.BoxLayout({
            style_class: 'landline-tour-body',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        this.contentLayout.add_child(this._body);

        this.connect('destroy', () => {
            if (this._focusId) {
                GLib.source_remove(this._focusId);
                this._focusId = 0;
            }
        });
    }

    // Escape puts the tour aside; the model decides whether it is kept.
    vfunc_key_press_event(event) {
        if (event.get_key_symbol() === Clutter.KEY_Escape) {
            this._model.closeTour();
            return Clutter.EVENT_STOP;
        }
        return super.vfunc_key_press_event(event);
    }

    // tour: view().tour of the model.
    update(tour) {
        this._tour = tour;
        if (this._step !== tour.step)
            this._helpOpen = false;
        const key = [tour.step, tour.at, tour.permission, tour.repair, tour.found, tour.candidates.length,
            tour.answered, tour.canRetry, tour.checking, tour.proof !== null, tour.allOn && tour.reviewed,
            tour.canBack, tour.noNotifications, tour.obex, tour.forget?.state, this._helpOpen,
            tour.permissionNeeds.join('+'), tour.permissionManual, tour.permissionCommands.join('\n'),
            this._art.language].join('|');
        if (key !== this._key) {
            const stepChanged = this._step !== null && this._step !== tour.step;
            this._key = key;
            this._step = tour.step;
            this._layout(tour);
            if (stepChanged)
                this._enter();
            this._wantFocus();
        }
        this._progress = tour.total > 0 ? tour.index / tour.total : 0;
        this._syncProgress(true);
        this._refresh?.(tour);
    }

    // The language of the iPhone changed in Phone Settings: draw again.
    relayout() {
        this._key = null;
        if (this._tour)
            this.update(this._tour);
    }

    // A word of the iPhone as an English sentence names it: "Pair", or
    // "Pair (Jumeler)" when the iPhone speaks another language.
    _p(key) {
        return bilingual(this._art.language, key);
    }

    // The same under a drawing, where there is little room: the word the
    // drawing shows.
    _cap(key) {
        return this._art.language === 'en' ? this._art.labels[key] : `“${this._art.labels[key]}”`;
    }

    // Settings › Bluetooth, as the iPhone writes it.
    _path() {
        return `${this._p('settings')} › ${this._p('bluetooth')}`;
    }

    _syncProgress(animate) {
        const width = Math.round(this._track.width * (this._progress ?? 0));
        if (this._track.width <= 0)
            return;
        this._fill.remove_transition('width');
        if (animate && this.mapped && this._fill.width > 0) {
            this._fill.ease({width, duration: 300, mode: Clutter.AnimationMode.EASE_OUT_QUAD});
        } else {
            this._fill.set_width(width);
        }
    }

    // A new step slides in, so a change of step is always seen.
    _enter() {
        const body = this._body;
        body.remove_all_transitions();
        body.opacity = 0;
        body.translation_y = 10;
        body.ease({
            opacity: 255,
            translation_y: 0,
            duration: STEP_FADE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }

    // The keyboard stays on the tour: on its main button after each change,
    // and again when a window that was above it (the password one) is gone.
    _wantFocus() {
        this._focusTries = 40;
        this._takeFocus();
        if (this._focusId)
            return;
        this._focusId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 250, () => {
            if (this._takeFocus() || --this._focusTries <= 0) {
                this._focusId = 0;
                return GLib.SOURCE_REMOVE;
            }
            return GLib.SOURCE_CONTINUE;
        });
    }

    _takeFocus() {
        if (this.state !== ModalDialog.State.OPENED && this.state !== ModalDialog.State.OPENING)
            return false;
        // Not the top window right now: wait for our turn.
        if (global.stage.get_grab_actor() !== this)
            return false;
        // Where no button may be the default, the keyboard rests on the
        // dialog itself: Enter must not press anything.
        const target = this._restFocus ? this.dialogLayout : this.dialogLayout.initialKeyFocus;
        if (!target?.mapped)
            return false;
        if (global.stage.key_focus !== target)
            target.grab_key_focus();
        return true;
    }

    // --- pieces ---------------------------------------------------------------

    _heading(title, text) {
        this._body.add_child(wrapped(title, 'landline-tour-title', true));
        if (text)
            this._body.add_child(wrapped(text, 'landline-tour-text', true));
    }

    // One or two drawings of the iPhone, each with a line under it; two are
    // numbered when they are two things to do one after the other.
    _phones(items, numbered = true) {
        const row = new St.BoxLayout({
            style_class: 'landline-tour-phones',
            x_align: Clutter.ActorAlign.CENTER,
        });
        const icons = [];
        for (const [index, item] of items.entries()) {
            const column = new St.BoxLayout({
                orientation: Clutter.Orientation.VERTICAL,
                style_class: 'landline-tour-phone',
            });
            const icon = new St.Icon({
                gicon: item.icon,
                icon_size: items.length > 1 ? PHONE_PAIR_SIZE : PHONE_SIZE,
            });
            column.add_child(icon);
            icons.push(icon);
            if (item.caption) {
                const caption = new St.BoxLayout({
                    style_class: 'landline-tour-caption',
                    x_align: Clutter.ActorAlign.CENTER,
                });
                if (items.length > 1 && numbered)
                    caption.add_child(badge(String(index + 1)));
                caption.add_child(new St.Label({text: item.caption, y_align: Clutter.ActorAlign.CENTER}));
                column.add_child(caption);
            }
            row.add_child(column);
        }
        this._body.add_child(row);
        return {row, icons};
    }

    // What is going on right now, with a spinner while it is not over.
    _status(text, {busy = true, detail = ''} = {}) {
        const box = new St.BoxLayout({
            style_class: 'landline-tour-status',
            orientation: Clutter.Orientation.VERTICAL,
            x_align: Clutter.ActorAlign.CENTER,
        });
        const row = new St.BoxLayout({style_class: 'landline-tour-status-row', x_align: Clutter.ActorAlign.CENTER});
        if (busy) {
            const spinner = new Animation.Spinner(16);
            spinner.y_align = Clutter.ActorAlign.CENTER;
            spinner.play();
            row.add_child(spinner);
        } else {
            row.add_child(new St.Icon({
                icon_name: 'object-select-symbolic',
                style_class: 'landline-tour-ok',
                y_align: Clutter.ActorAlign.CENTER,
            }));
        }
        const label = new St.Label({text, style_class: 'landline-tour-status-text', y_align: Clutter.ActorAlign.CENTER});
        row.add_child(label);
        box.add_child(row);
        if (detail) {
            const line = wrapped(detail, 'landline-sub', true);
            line.x_align = Clutter.ActorAlign.CENTER;
            box.add_child(line);
        }
        this._body.add_child(box);
        return label;
    }

    // Lines that tick by themselves when the engine sees them done.
    _checklist(entries) {
        const box = new St.BoxLayout({
            style_class: 'landline-tour-checks',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        const rows = entries.map(([id, label]) => {
            const row = new St.BoxLayout({style_class: 'landline-switch-row'});
            const state = new St.Icon({
                icon_name: 'object-select-symbolic',
                style_class: 'landline-switch-state',
                y_align: Clutter.ActorAlign.CENTER,
            });
            row.add_child(state);
            // A long label (an iPhone in another language) takes two lines.
            const text = wrapped(label, '');
            text.y_align = Clutter.ActorAlign.CENTER;
            row.add_child(text);
            box.add_child(row);
            return {id, row, state, label, on: null};
        });
        this._body.add_child(box);
        return values => {
            for (const entry of rows) {
                const on = values[entry.id] === true;
                if (on === entry.on)
                    continue;
                const first = entry.on === null;
                entry.on = on;
                entry.state.remove_all_transitions();
                if (on && !first) {
                    // The tick pops in: something just happened on the iPhone.
                    entry.state.set_pivot_point(0.5, 0.5);
                    entry.state.set_scale(0.3, 0.3);
                    entry.state.opacity = 0;
                    entry.state.ease({
                        opacity: 255, scale_x: 1, scale_y: 1,
                        duration: 250, mode: Clutter.AnimationMode.EASE_OUT_BACK,
                    });
                } else {
                    entry.state.opacity = on ? 255 : 0;
                }
                if (on)
                    entry.row.add_style_class_name('landline-switch-on');
                else
                    entry.row.remove_style_class_name('landline-switch-on');
                entry.row.accessible_name = on
                    ? `${entry.label}, ${this._('on')}` : `${entry.label}, ${this._('waiting')}`;
            }
        };
    }

    // What to run in a terminal, and what happens next.
    _commands(commands) {
        const _ = this._;
        const box = new St.BoxLayout({
            style_class: 'landline-tour-commands',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        for (const command of commands)
            box.add_child(wrapped(command, 'landline-tour-command'));
        if (commands.length > 0)
            this._body.add_child(box);
        this._body.add_child(wrapped(commands.length > 0
            ? _('Copy, paste in a terminal, and give your password there. The setup goes on by itself once it is done.')
            : _('The installation notes of the phone link say what to change. The setup goes on by itself once it is done.'),
        'landline-sub', true));
    }

    _buttons(list, restFocus = false) {
        if (list.length === 0) {
            // Nothing to press while something is under way: one button
            // that says so keeps the dialog from changing size.
            this._restFocus = true;
            this.setButtons([{label: this._('One moment…'), action: () => {}}]);
            const [wait] = this.buttonLayout.get_children();
            wait.reactive = false;
            wait.can_focus = false;
            wait.add_style_class_name('landline-tour-wait');
            return;
        }
        this._restFocus = restFocus;
        this.setButtons(list.map(({label, action, primary}) => ({label, action, default: primary === 'default'})));
        const actors = this.buttonLayout.get_children();
        list.forEach(({primary}, index) => {
            if (primary)
                actors[index].add_style_class_name('landline-dialog-primary');
        });
    }

    _host(tour) {
        return tour.hostName || this._('this computer');
    }

    // "Forget This iPhone…", inside a step that is stuck.
    _forgetLink() {
        const button = new St.Button({
            style_class: 'button landline-tour-link',
            label: this._('Forget This iPhone…'),
            can_focus: true,
            x_align: Clutter.ActorAlign.CENTER,
        });
        button.connect('clicked', () => this._model.startForget());
        this._body.add_child(button);
    }

    // --- steps ----------------------------------------------------------------

    _layout(tour) {
        const _ = this._;
        this._body.destroy_all_children();
        this._refresh = null;
        const host = this._host(tour);

        const names = {
            permission: _('Permission'),
            repair: _('Start fresh'),
            find: _('Find your iPhone'),
            code: _('Check the number'),
            pairing: _('Pairing'),
            switches: _('Allow sharing'),
            try: _('Try it'),
            done: _('Done'),
            forget: _('Forget this iPhone'),
        };
        const path = this._path();
        // Translators: the first %s is "Settings › Bluetooth", the second the
        // name of this computer, the third "Forget This Device".
        const forgetPath = _('%s › ⓘ next to “%s” › %s')
            .replace('%s', path).replace('%s', host).replace('%s', this._p('forget'));
        const where = {
            permission: _('nothing yet. This part happens on this computer.'),
            repair: forgetPath,
            // Translators: %s is "Settings › Bluetooth".
            find: _('%s. Stay on that screen.').replace('%s', path),
            code: _('the pairing request that just popped up.'),
            pairing: _('stay on %s.').replace('%s', path),
            switches: _('%s › ⓘ next to “%s”').replace('%s', path).replace('%s', host),
            try: _('nothing to do. It can stay locked.'),
            done: _('nothing more to do.'),
            forget: tour.forget?.state === 'phone' ? forgetPath
                : _('nothing yet. This part happens on this computer.'),
        };
        // Translators: "Step 2 of 6 — Find your iPhone".
        this._stepLabel.text = tour.total > 0
            ? _('Step %d of %d — %s')
                .replace('%d', tour.index).replace('%d', tour.total).replace('%s', names[tour.at] ?? '')
            : names[tour.at] ?? '';
        this._track.visible = tour.total > 0;
        this._whereLabel.text = where[tour.at] ?? '';

        const cancel = {label: _('Cancel'), action: () => this._model.cancelTour()};
        switch (tour.step) {
        case 'permission':
            this._permission(tour);
            break;
        case 'repair':
            this._repair(tour, host);
            break;
        case 'find':
            this._find(tour, cancel);
            break;
        case 'code':
            this._code(tour, host);
            break;
        case 'pairing':
            this._pairing(tour, host, cancel);
            break;
        case 'switches':
            this._switches(tour, host);
            break;
        case 'try':
            this._try(tour);
            break;
        case 'done':
            this._done(tour);
            break;
        case 'forget':
            this._forget(tour, host);
            break;
        default:
            this._heading(_('That did not work'), tour.error);
            if (tour.at !== 'permission') {
                this._body.add_child(wrapped(
                    _('Keeps failing? Forget the link on both sides and start from scratch.'),
                    'landline-sub', true));
                this._forgetLink();
            }
            if (tour.canRetry) {
                this._buttons([cancel, {
                    label: _('Try Again'), primary: 'default',
                    action: () => this._model.retryTour(),
                }]);
            } else {
                this._status(_('One moment…'));
                this._buttons([cancel]);
            }
            break;
        }
    }

    _permission(tour) {
        const _ = this._;
        // Only what this computer really lacks: one point or two.
        const known = {
            experimental: [_('Turn on newer Bluetooth features'), _('Needed for notifications and calls.')],
            ertm: [_('Allow reliable data transfer'),
                _('Needed to read messages. Some game controllers may need it off.')],
        };
        const points = tour.permissionNeeds.map(id => known[id]).filter(point => point);
        const manual = tour.permissionManual && tour.permission !== 'done';
        const [one, two] = manual
            ? [_('One Bluetooth setting needs to change. It is done once, as administrator, in a terminal: this extension cannot ask for your password on this computer.'),
                _('Two Bluetooth settings need to change. It is done once, as administrator, in a terminal: this extension cannot ask for your password on this computer.')]
            : [_('One Bluetooth setting needs to change. It is done once, and it asks for your password because it belongs to the whole computer.'),
                _('Two Bluetooth settings need to change. It is done once, and it asks for your password because they belong to the whole computer.')];
        this._heading(_('Let this computer talk to your iPhone'), points.length === 1 ? one : two);
        const steps = new St.BoxLayout({
            style_class: 'landline-dialog-steps',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        for (const [index, [title, detail]] of points.entries()) {
            const row = new St.BoxLayout({style_class: 'landline-step'});
            if (points.length > 1)
                row.add_child(badge(String(index + 1)));
            const text = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
            text.add_child(wrapped(title, 'landline-row-name'));
            text.add_child(wrapped(detail, 'landline-sub'));
            row.add_child(text);
            steps.add_child(row);
        }
        this._body.add_child(steps);

        const notNow = {label: _('Not Now'), action: () => this._model.cancelTour()};
        if (tour.permissionManual && tour.permission !== 'done') {
            this._commands(tour.permissionCommands);
            this._buttons([notNow, {
                label: _('Copy Commands'), primary: 'default',
                action: () => this._actions.copyCommands(tour.permissionCommands),
            }]);
            return;
        }
        const again = {
            label: _('Try Again'), primary: 'default',
            action: () => this._actions.applyPermission(),
        };
        switch (tour.permission) {
        case 'waiting':
            this._status(_('Waiting for your password…'),
                {detail: _('The password window belongs to the system. The setup goes on by itself right after.')});
            this._buttons([]);
            break;
        case 'refused':
            this._body.add_child(wrapped(_('No password, so nothing was changed. Whenever you are ready.'),
                'landline-tour-note', true));
            this._buttons([notNow, again]);
            break;
        case 'failed':
            this._body.add_child(wrapped(_('The Bluetooth settings could not be changed. Nothing was lost.'),
                'landline-tour-note landline-error', true));
            this._buttons([notNow, again]);
            break;
        case 'partial':
            this._body.add_child(wrapped(_('The settings are saved, but one could not be turned on now. Restart this computer, then click the phone in the top bar again.'),
                'landline-tour-note', true));
            this._buttons([{label: _('Close'), primary: 'default', action: () => this._model.cancelTour()}]);
            break;
        case 'done':
            this._status(_('This computer is ready'), {busy: false});
            this._buttons([]);
            break;
        default:
            this._body.add_child(wrapped(tour.restartsBluetooth
                ? _('Bluetooth restarts once: keyboards, mice and headphones drop for a few seconds. Nothing else is changed. Undo any time in Phone Settings.')
                : _('Nothing else is changed. Undo any time in Phone Settings.'),
            'landline-sub', true));
            this._buttons([notNow, {...again, label: _('Continue')}]);
            break;
        }
    }

    _repair(tour, host) {
        const _ = this._;
        this._heading(_('Let’s start fresh'),
            _('This computer and your iPhone remember each other, but the link is broken. Forget this computer on the iPhone; this computer then forgets the iPhone too.'));
        this._phones([
            // Translators: %s is the name of this computer.
            {icon: this._art.bluetooth({host, state: 'notConnected', ring: true}), caption: _('Tap ⓘ next to “%s”').replace('%s', host)},
            // Translators: %s is "Forget This Device", as the iPhone writes it.
            {icon: this._art.info({host, forget: true}), caption: _('Tap %s').replace('%s', this._cap('forget'))},
        ]);
        if (tour.repair === 'ask') {
            this._body.add_child(wrapped(_('Not in the list? Then the iPhone has already forgotten it: just continue.'),
                'landline-sub', true));
            this._forgetLink();
            this._buttons([
                {label: _('Not Now'), action: () => this._model.cancelTour()},
                {
                    label: _('Done on My iPhone'), primary: 'default',
                    action: () => this._model.tourRepairDone(),
                },
            ]);
        } else {
            this._status(tour.repair === 'removed'
                ? _('Forgotten on this computer') : _('Forgetting the iPhone on this computer…'),
            {busy: tour.repair !== 'removed', detail: _('A few seconds.')});
            this._buttons([]);
        }
    }

    _find(tour, cancel) {
        const _ = this._;
        this._heading(_('Open Bluetooth on your iPhone'),
            // Translators: %s is "Settings › Bluetooth".
            _('On your iPhone, open %s. Stay on that screen and don’t tap anything.').replace('%s', this._path()));
        this._phones([{icon: this._art.bluetooth()}]);
        if (tour.candidates.length > 1) {
            this._body.add_child(wrapped(_('Several iPhones are nearby. Which one is yours?'), 'landline-sub', true));
            for (const candidate of tour.candidates.slice(0, 4)) {
                const button = new St.Button({
                    style_class: 'button landline-action',
                    label: candidate.name,
                    can_focus: true,
                    x_expand: true,
                });
                button.connect('clicked', () => this._model.choosePairingDevice(candidate.address));
                this._body.add_child(button);
            }
        } else if (tour.found) {
            // Translators: %s is the name of the iPhone.
            this._status(_('Found %s').replace('%s', tour.deviceName || _('your iPhone')),
                {busy: false, detail: _('Asking it to pair. The request shows up on the iPhone in a few seconds.')});
        } else {
            this._status(_('Looking for your iPhone…'), {detail: _('Usually a few seconds, a minute at most.')});
        }
        this._buttons([cancel]);
    }

    _code(tour, host) {
        const _ = this._;
        this._heading(_('A pairing request pops up on your iPhone'),
            // Translators: %s is "Pair", the button of the iPhone.
            _('Check the number is the same, tap %s on the iPhone, then choose They Match here.')
                .replace('%s', this._p('pair')));
        const row = new St.BoxLayout({style_class: 'landline-tour-compare', x_align: Clutter.ActorAlign.CENTER});
        row.add_child(new St.Icon({gicon: this._art.request({host, code: tour.code}), icon_size: PHONE_SIZE}));
        const side = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            style_class: 'landline-tour-compare-side',
            y_align: Clutter.ActorAlign.CENTER,
        });
        side.add_child(new St.Label({
            text: _('On this computer'),
            style_class: 'landline-sub',
            x_align: Clutter.ActorAlign.CENTER,
        }));
        const code = String(tour.code);
        side.add_child(new St.Label({
            text: code.length === 6 ? `${code.slice(0, 3)} ${code.slice(3)}` : code,
            style_class: 'landline-code landline-code-large',
            x_align: Clutter.ActorAlign.CENTER,
        }));
        const left = new St.Label({style_class: 'landline-sub', x_align: Clutter.ActorAlign.CENTER});
        side.add_child(left);
        row.add_child(side);
        this._body.add_child(row);
        if (tour.answered) {
            left.text = _('Answer sent…');
            this._buttons([]);
            return;
        }
        this._refresh = now => {
            // Translators: %d is a number of seconds.
            left.text = _('%d s left to answer').replace('%d', now.secondsLeft);
        };
        // Neither is the default: Enter must not confirm a number unread.
        this._buttons([
            {label: _('They Differ'), action: () => this._model.confirmPairing(false)},
            {label: _('They Match'), primary: true, action: () => this._model.confirmPairing(true)},
        ], true);
    }

    _pairing(tour, host, cancel) {
        const _ = this._;
        if (tour.checking) {
            this._heading(_('Looking for your iPhone…'),
                _('This computer and your iPhone already know each other. Keep the iPhone close and unlocked, with Bluetooth on. This can take up to a minute.'));
        } else {
            this._heading(_('Pairing…'),
                _('This takes a few seconds, sometimes half a minute. Keep the iPhone unlocked and close.'));
        }
        const phones = this._phones([{icon: this._art.bluetooth({host})}]);
        const tick = this._checklist([
            ['classic', _('Link for calls, messages and contacts')],
            ['le', _('Link for notifications')],
        ]);
        const waiting = this._status(_('Waiting for both links to come up…'));
        let shown = null;
        this._refresh = now => {
            tick(now.links);
            const up = now.links.classic && now.links.le;
            if (up !== shown) {
                shown = up;
                phones.icons[0].gicon = this._art.bluetooth({host, state: up ? 'connected' : ''});
                waiting.text = up ? _('Both links are up') : _('Waiting for both links to come up…');
            }
        };
        this._buttons([cancel]);
    }

    _switches(tour, host) {
        const _ = this._;
        const obex = tour.obex;
        const help = this._helpOpen && (!obex || obex === 'fixed');
        this._heading(_('Now allow the iPhone to share with this computer'),
            // Translators: %s is "Allow", the button of the iPhone.
            _('Turn these switches on. Tap %s if the iPhone asks.')
                .replace('%s', this._p('allow')));
        // With the help open, or once the fault is known: what the page
        // should look like, next to what it looks like without the switches.
        const compare = help || (!!obex && obex !== 'fixed');
        const phones = this._phones(compare ? [
            {icon: this._art.info({host, switches: tour.switches}), caption: _('What you should see')},
            {icon: this._art.info({host, switches: tour.switches, bare: true}), caption: _('If you only see this')},
        ] : [
            {icon: this._art.bluetooth({host, state: 'connected', ring: true}), caption: _('Tap ⓘ next to “%s”').replace('%s', host)},
            {icon: this._art.info({host, switches: tour.switches}), caption: _('Turn these on')},
        ], !compare);
        const drawing = compare ? 0 : 1;
        let tick = null;
        if (!compare) {
            // In the order of the page on the iPhone.
            tick = this._checklist([
                ['messages', this._p('showNotifications')],
                ['contacts', this._p('syncContacts')],
                ['notifications', tour.noNotifications
                    // Translators: %s is "Share System Notifications".
                    ? _('%s (not available with this Bluetooth adapter)').replace('%s', this._p('shareNotifications'))
                    : this._p('shareNotifications')],
            ]);
        }
        let shown = '';
        this._refresh = now => {
            tick?.(now.switches);
            const state = JSON.stringify(now.switches);
            if (state !== shown) {
                shown = state;
                phones.icons[drawing].gicon = this._art.info({host, switches: now.switches});
            }
        };

        const later = {label: _('Finish Later'), action: () => this._model.cancelTour()};
        const fix = {
            label: _('Fix It'), primary: 'default',
            action: () => this._actions.restartEngine(),
        };
        const two = text => text
            .replace('%s', this._p('deviceType')).replace('%s', this._p('shareNotifications'));
        const note = (text, error = true) => this._body.add_child(wrapped(text,
            error ? 'landline-tour-note landline-error' : 'landline-tour-note', true));
        const sub = text => this._body.add_child(wrapped(text, 'landline-sub', true));
        switch (obex) {
        case 'down':
        case 'failed':
            note(obex === 'failed'
                ? _('The phone link could not be restarted.')
                : _('This computer can’t offer messages and contacts right now.'));
            // Translators: the two %s are "Device Type" and "Share System Notifications".
            sub(two(_('That is why the iPhone shows only %s and %s. Fix It restarts the phone link on this computer; the iPhone drops for a few seconds.')));
            if (obex === 'failed')
                this._forgetLink();
            this._buttons([later, fix]);
            return;
        case 'restarting':
            this._status(_('Restarting the phone link…'),
                {detail: _('The iPhone links again by itself. Up to a minute.')});
            this._buttons([]);
            return;
        case 'fixed':
            // Then the usual help: the switches may still need a moment.
            this._status(_('Messages and contacts are on offer again'),
                {busy: false, detail: _('On the iPhone, tap ⓘ again: the switches show up within a minute.')});
            break;
        case 'still':
            note(_('Restarting did not bring messages and contacts back.'));
            sub(_('Start from scratch: forget the link on both sides, then link again.'));
            this._buttons([later, {
                label: _('Forget This iPhone…'), primary: 'default',
                action: () => this._model.startForget(),
            }]);
            return;
        default:
            break;
        }

        const toggle = new St.Button({
            style_class: 'button landline-tour-link',
            can_focus: true,
            x_align: Clutter.ActorAlign.CENTER,
        });
        const row = new St.BoxLayout({style_class: 'landline-tour-link-row'});
        row.add_child(new St.Label({text: _('Don’t see these switches?'), y_align: Clutter.ActorAlign.CENTER}));
        row.add_child(new St.Icon({
            icon_name: help ? 'pan-up-symbolic' : 'pan-down-symbolic',
            style_class: 'popup-menu-arrow',
            y_align: Clutter.ActorAlign.CENTER,
        }));
        toggle.set_child(row);
        toggle.accessible_name = _('Don’t see these switches?');
        toggle.connect('clicked', () => {
            this._helpOpen = !this._helpOpen;
            this.relayout();
        });
        if (help) {
            this._body.add_child(toggle);
            // Translators: the two %s are "Device Type" and "Share System Notifications".
            sub(two(_('If the iPhone shows only %s and %s, this computer was not offering messages and contacts when the iPhone looked. Close the page, wait a few seconds and open it again. Still the same after a minute? Start from scratch:')));
            this._forgetLink();
        } else {
            if (obex !== 'fixed')
                sub(_('These switches can take up to a minute to appear.'));
            this._body.add_child(toggle);
        }
        if (tour.allOn && tour.reviewed) {
            this._buttons([later, {
                label: _('Continue'), primary: 'default',
                action: () => this._model.tourNext(),
            }]);
        } else {
            // Nothing here is a default: Enter must not end the setup.
            this._buttons([later], true);
        }
    }

    _forget(tour, host) {
        const _ = this._;
        const state = tour.forget?.state;
        const name = tour.deviceName || _('your iPhone');
        const leave = {
            label: tour.forget?.back ? _('Back') : _('Cancel'),
            action: () => this._model.forgetBack(),
        };
        if (state === 'phone') {
            this._heading(_('Now forget this computer on your iPhone'),
                _('This computer has forgotten the iPhone. The iPhone keeps its own record of this computer, and only you can remove it.'));
            this._phones([
                {icon: this._art.bluetooth({host, state: 'notConnected', ring: true}), caption: _('Tap ⓘ next to “%s”').replace('%s', host)},
                {icon: this._art.info({host, forget: true}), caption: _('Tap %s').replace('%s', this._cap('forget'))},
            ]);
            this._body.add_child(wrapped(
                _('Not in the list? Then the iPhone has already forgotten it. Link Again starts the setup from the search.'),
                'landline-sub', true));
            this._buttons([
                {label: _('Close'), action: () => this._model.cancelTour()},
                {label: _('Link Again'), primary: 'default', action: () => this._model.forgetLinkAgain()},
            ]);
            return;
        }
        if (state === 'error') {
            this._heading(_('That did not work'),
                _('This computer could not forget the iPhone by itself. Remove the iPhone in the Bluetooth settings of this computer, then try again.'));
            this._buttons([leave, {
                label: _('Try Again'), primary: 'default',
                action: () => this._model.forgetConfirm(),
            }]);
            return;
        }
        // Translators: %s is the name of the iPhone.
        this._heading(_('Forget %s?').replace('%s', name),
            _('Messages, calls and notifications stop showing up here until you link the iPhone again. Nothing is deleted on the iPhone.'));
        const steps = new St.BoxLayout({
            style_class: 'landline-dialog-steps',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        const points = [
            [_('This computer forgets the iPhone'), _('Done for you, in a few seconds.')],
            [_('You forget this computer on the iPhone'),
                _('Nobody can do that in your place: the next screen shows where to tap.')],
            [_('Link again whenever you like'), _('The setup starts over from the search, with a new number to check.')],
        ];
        for (const [index, [title, detail]] of points.entries()) {
            const row = new St.BoxLayout({style_class: 'landline-step'});
            row.add_child(badge(String(index + 1)));
            const text = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
            text.add_child(wrapped(title, 'landline-row-name'));
            text.add_child(wrapped(detail, 'landline-sub'));
            row.add_child(text);
            steps.add_child(row);
        }
        this._body.add_child(steps);
        if (state === 'removing') {
            this._status(_('Forgetting the iPhone on this computer…'), {detail: _('A few seconds.')});
            this._buttons([]);
            return;
        }
        // Not a default: Enter must not forget anything.
        this._buttons([leave, {
            label: _('Forget'), primary: true,
            action: () => this._model.forgetConfirm(),
        }], true);
    }

    _try(tour) {
        const _ = this._;
        this._heading(_('Let’s try it'),
            _('Send a message to your iPhone from another phone, or ask someone to.'));
        const proof = tour.proof;
        this._phones([{
            icon: this._art.lock(proof
                ? {title: proof.name, text: proof.body}
                : {title: this._art.labels.messages, text: _('Hello!')}),
        }]);
        if (proof) {
            this._status(_('It works'), {busy: false});
            const bubble = new St.BoxLayout({
                style_class: 'landline-bubble landline-tour-proof',
                orientation: Clutter.Orientation.VERTICAL,
                x_align: Clutter.ActorAlign.CENTER,
            });
            bubble.add_child(new St.Label({text: proof.name, style_class: 'landline-row-name'}));
            const text = new St.Label({text: proof.body, style_class: 'landline-body'});
            bubble.add_child(text);
            this._body.add_child(bubble);
        } else {
            this._status(_('Waiting for a message…'),
                {detail: _('It shows up here a few seconds after the iPhone gets it, 15 at most.')});
        }
        const buttons = [];
        if (tour.canBack)
            buttons.push({label: _('Back'), action: () => this._model.tourBack()});
        buttons.push(proof
            ? {label: _('Continue'), primary: 'default', action: () => this._model.tourNext()}
            : {label: _('Skip'), action: () => this._model.tourNext()});
        // While waiting, Enter neither goes back nor skips.
        this._buttons(buttons, !proof);
    }

    _done(tour) {
        const _ = this._;
        const check = new St.Icon({
            icon_name: 'object-select-symbolic',
            style_class: 'landline-tour-done',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._body.add_child(check);
        this._heading(_('Your iPhone is linked'), '');
        const stats = new St.Label({style_class: 'landline-tour-stats', x_align: Clutter.ActorAlign.CENTER});
        this._body.add_child(stats);
        this._refresh = now => {
            const parts = [];
            if (now.battery !== null)
                parts.push(_('Battery %d %%').replace('%d', now.battery).replace('%%', '%'));
            if (now.contacts !== null)
                parts.push(_('%d contacts').replace('%d', now.contacts));
            parts.push(_('%d conversations').replace('%d', now.conversations));
            if (now.calls)
                parts.push(_('Calls'));
            stats.text = parts.join('   ·   ');
        };

        const daily = new St.BoxLayout({
            style_class: 'landline-dialog-steps',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        const lines = [
            ['phone-symbolic', _('In the top bar'),
                _('A small phone with an amber dot: your iPhone is linked. Click it for your conversations.')],
            ['mail-unread-symbolic', _('When a message or a call arrives'),
                _('The dot grows into a pill with the name. Click it to reply, answer or decline.')],
            ['view-refresh-symbolic', _('When you walk away'),
                _('The link drops and the dot goes hollow. It comes back by itself when you return.')],
        ];
        for (const [iconName, title, detail] of lines) {
            const row = new St.BoxLayout({style_class: 'landline-step'});
            row.add_child(new St.Icon({
                icon_name: iconName,
                style_class: 'landline-tour-daily-icon',
                y_align: Clutter.ActorAlign.START,
            }));
            const text = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
            text.add_child(wrapped(title, 'landline-row-name'));
            text.add_child(wrapped(detail, 'landline-sub'));
            row.add_child(text);
            daily.add_child(row);
        }
        this._body.add_child(daily);

        const buttons = [];
        if (tour.canBack)
            buttons.push({label: _('Back'), action: () => this._model.tourBack()});
        buttons.push({label: _('Done'), primary: 'default', action: () => this._model.cancelTour()});
        this._buttons(buttons);
    }
});
