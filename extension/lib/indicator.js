// SPDX-License-Identifier: GPL-2.0-or-later
// The indicator in the top bar: a phone glyph and a dot at rest, a pill when
// something is going on. Clicking it opens the card built by cards.js.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {buildCard} from './cards.js';

const MORPH_MS = 200;
const TONES = ['quiet', 'accent', 'call', 'neutral'];

export const Indicator = GObject.registerClass(
class PhoneIndicator extends PanelMenu.Button {
    // actions: what the cards may ask for (see extension.js).
    // phoneLabel(key): a word of the iPhone, as an English sentence names it.
    _init({model, actions, iconsDir, gettext, phoneLabel}) {
        super._init(0.5, gettext('iPhone'));
        this._model = model;
        this._actions = actions;
        this._iconsDir = iconsDir;
        this._ = gettext;
        this._phoneLabel = phoneLabel ?? (key => key);
        this._shape = null;
        this._cardKey = null;
        this._card = null;
        this._laterId = 0;
        this._useAccent = false;
        this._laidOut = false;
        this._opening = false;
        this._view = null;
        this._cardJson = null;
        this._glyphName = null;
        this._passive = null;
        this._passiveKey = null;
        this._passiveCard = null;

        this.add_style_class_name('landline-button');
        this.menu.actor.add_style_class_name('landline-menu');
        // One width for everything that drops under the indicator, whatever
        // the length of a name or of a message.
        this.menu.box.add_style_class_name('landline-menu-box');

        this._pill = new St.BoxLayout({
            style_class: 'landline-pill',
            y_align: Clutter.ActorAlign.CENTER,
            clip_to_allocation: true,
        });
        this._glyph = new St.Icon({
            style_class: 'landline-glyph',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._avatar = new St.Label({
            style_class: 'landline-pill-avatar',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._avatar.clutter_text.x_align = Clutter.ActorAlign.CENTER;
        this._strong = new St.Label({
            style_class: 'landline-pill-strong',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._text = new St.Label({
            style_class: 'landline-pill-text',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._dot = new St.Widget({
            style_class: 'landline-dot',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._count = new St.Label({
            style_class: 'landline-count',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._count.clutter_text.x_align = Clutter.ActorAlign.CENTER;
        // At rest the dot and the count share one slot of a fixed width: the
        // indicator is as wide linked, away, starting or with unread messages.
        this._slot = new St.Widget({
            style_class: 'landline-slot',
            layout_manager: new Clutter.BinLayout(),
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._dot.x_align = Clutter.ActorAlign.CENTER;
        this._dot.x_expand = true;
        this._count.x_align = Clutter.ActorAlign.CENTER;
        this._count.x_expand = true;
        this._slot.add_child(this._dot);
        this._slot.add_child(this._count);
        for (const child of [this._glyph, this._avatar, this._strong, this._text, this._slot])
            this._pill.add_child(child);
        this.add_child(this._pill);

        // In the states where a click starts a dialog (permission, linking),
        // there is no menu to open.
        const toggle = this.menu.toggle.bind(this.menu);
        this.menu.toggle = () => {
            const card = this._model.view().card;
            if (card.kind === 'direct' && !this.menu.isOpen)
                this._actions.run(card.action, card);
            else
                toggle();
        };

        this.menu.connect('open-state-changed', (menu, open) => {
            if (open && this._model.view().card.kind === 'direct') {
                const card = this._model.view().card;
                this.menu.close({animate: false});
                this._actions.run(card.action, card);
                return;
            }
            if (open) {
                this._passive?.close({animate: false});
                // Filled now, before the first frame of the opening.
                this._opening = true;
                this._cardKey = null;
                this._model.cardOpened();
                this._opening = false;
                this._model.refreshLists();
            } else {
                this._model.cardClosed();
            }
        });

        // An incoming call shows its card by itself. That one must not take
        // the keyboard from whatever the user is typing in, so it is a menu
        // of its own that is never made modal.
        this._passive = new PopupMenu.PopupMenu(this, 0.5, St.Side.TOP);
        this._passive.actor.add_style_class_name('panel-menu');
        this._passive.actor.add_style_class_name('landline-menu');
        this._passive.box.add_style_class_name('landline-menu-box');
        Main.layoutManager.addTopChrome(this._passive.actor);
        this._passive.actor.hide();
        this._passiveKey = null;

        // A menu with nothing in it does not open: the real content replaces
        // this line each time the menu opens.
        this.menu.addMenuItem(new PopupMenu.PopupMenuItem(gettext('iPhone')));

        this.sync();

        // A card that shows by itself is placed against this button: it
        // waits until the button has a place in the bar.
        const firstAllocation = this.connect('notify::allocation', () => {
            this.disconnect(firstAllocation);
            this._laidOut = true;
            this._syncPassive(this._model.view());
        });
    }

    setUseAccent(use) {
        this._useAccent = use;
        for (const actor of [this, this.menu.actor, this._passive.actor]) {
            if (use)
                actor.add_style_class_name('landline-system-accent');
            else
                actor.remove_style_class_name('landline-system-accent');
        }
    }

    icon(name) {
        if (name.startsWith('landline-'))
            return Gio.icon_new_for_string(`${this._iconsDir}/${name}.svg`);
        return new Gio.ThemedIcon({name});
    }

    // Bring the bar and the open card in line with the model.
    sync() {
        const view = this._model.view();
        this._view = view;
        this.accessible_name = view.a11y;
        this._syncPill(view);
        this._syncCard(view);
        this._syncPassive(view);
    }

    _syncPill(view) {
        const pill = view.pill;
        const shape = [view.mode, view.link, pill.tone, pill.icon ?? '', pill.dot ?? '',
            pill.count ? 'n' : '', pill.avatar ?? '', pill.text ?? '',
            view.mode === 'call' ? '' : pill.strong ?? ''].join('|');
        const apply = () => this._applyPill(pill);
        if (shape === this._shape) {
            apply();
        } else if (!this.mapped) {
            this._pill.remove_transition('width');
            this._pill.set_width(-1);
            apply();
        } else {
            this._morph(apply);
        }
        this._shape = shape;
    }

    _applyPill(pill) {
        for (const tone of TONES) {
            if (tone === pill.tone)
                this.add_style_class_name(`landline-${tone}`);
            else
                this.remove_style_class_name(`landline-${tone}`);
        }
        if (pill.dim)
            this.add_style_class_name('landline-dim');
        else
            this.remove_style_class_name('landline-dim');

        this._glyph.visible = !!pill.icon;
        if (pill.icon) {
            const name = {
                phone: 'landline-symbolic',
                call: 'call-start-symbolic',
                check: 'object-select-symbolic',
                person: 'avatar-default-symbolic',
            }[pill.icon];
            if (this._glyphName !== name) {
                this._glyphName = name;
                this._glyph.gicon = this.icon(name);
            }
        }
        this._avatar.visible = !!pill.avatar;
        this._avatar.text = pill.avatar ?? '';
        this._strong.visible = !!pill.strong;
        this._strong.text = pill.strong ?? '';
        this._text.visible = !!pill.text;
        this._text.text = (pill.text ?? '').replace(/\s+/g, ' ');
        this._dot.visible = !!pill.dot;
        for (const kind of ['on', 'off', 'warn']) {
            if (kind === pill.dot)
                this._dot.add_style_class_name(`landline-dot-${kind}`);
            else
                this._dot.remove_style_class_name(`landline-dot-${kind}`);
        }
        this._count.visible = !!pill.count;
        // One character wide at most, so the slot never has to grow.
        this._count.text = pill.count ? pill.count > 9 ? '9+' : String(pill.count) : '';
        // The slot is there whenever the indicator is at rest, empty or not.
        this._slot.visible = !!pill.dot || !!pill.count || (pill.tone === 'quiet' && !pill.strong);
    }

    // Change the content, then slide the width from the old size to the new
    // one, so the bar never jumps.
    _morph(apply) {
        const pill = this._pill;
        const from = pill.width;
        pill.remove_transition('width');
        pill.set_width(-1);
        apply();
        const [, to] = pill.get_preferred_width(-1);
        if (Math.abs(to - from) < 1)
            return;
        pill.set_width(from);
        pill.ease({
            width: to,
            duration: MORPH_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onStopped: finished => {
                if (finished)
                    pill.set_width(-1);
            },
        });
    }

    _cardKeyOf(card) {
        return `${card.kind}:${card.thread ?? card.problem ?? ''}`;
    }

    _syncCard(view) {
        if (!this.menu.isOpen)
            return;
        if (!view.cardOpen) {
            this.menu.close();
            return;
        }
        const key = this._cardKeyOf(view.card);
        const json = JSON.stringify(view.card);
        if (key === this._cardKey) {
            if (json === this._cardJson)
                return;
            if (this._card?.update) {
                this._cardJson = json;
                this._card.update(view.card);
                return;
            }
        }
        if (this._opening) {
            // Filled before the first frame. The keyboard focus has to wait:
            // the menu takes it for itself right after this.
            this._rebuild(view);
            this._later(() => this._card?.focus?.());
            return;
        }
        // The click that led here may still be running on an item of the
        // card about to be replaced.
        this._later(() => {
            this._rebuild(this._model.view());
            this._card.focus?.();
        });
    }

    // Run once the current event is over, if the menu is still open.
    _later(callback) {
        if (this._laterId)
            GLib.source_remove(this._laterId);
        this._laterId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._laterId = 0;
            if (this.menu.isOpen)
                callback();
            return GLib.SOURCE_REMOVE;
        });
    }

    _rebuild(view) {
        this._cardKey = this._cardKeyOf(view.card);
        this._cardJson = JSON.stringify(view.card);
        this._card = buildCard(this.menu, view.card, this._host(this.menu));
    }

    _host(menu) {
        return {
            _: this._,
            model: this._model,
            actions: this._actions,
            icon: name => this.icon(name),
            phoneLabel: key => this._phoneLabel(key),
            close: () => menu.close(),
        };
    }

    _syncPassive(view) {
        const wanted = view.mode === 'ring' && !this.menu.isOpen && this._laidOut;
        if (!wanted) {
            if (this._passive.isOpen)
                this._passive.close();
            if (view.mode !== 'ring')
                this._passiveKey = null;
            return;
        }
        // Shown once per call: closed by hand, it stays closed.
        const key = `${view.card.name}`;
        if (this._passiveKey === null) {
            this._passiveKey = key;
            this._passiveCard = buildCard(this._passive, view.card, this._host(this._passive));
            this._passive.open();
        } else if (this._passive.isOpen) {
            this._passiveCard.update?.(view.card);
        }
    }

    _onDestroy() {
        if (this._laterId) {
            GLib.source_remove(this._laterId);
            this._laterId = 0;
        }
        // Closed first, without animation: destroyed with a submenu open,
        // the menu is laid out once more after it is gone.
        this.menu.close({animate: false});
        this._pill.remove_all_transitions();
        if (this._passive) {
            this._passive.close({animate: false});
            // Destroying the actor also takes it out of the layout manager.
            this._passive.destroy();
            this._passive = null;
        }
        super._onDestroy();
    }
});
