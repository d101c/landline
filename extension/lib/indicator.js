// SPDX-License-Identifier: GPL-2.0-or-later
// The indicator in the top bar: an icon that says the state by itself at
// rest, and the head of a pill when something is going on. Clicking it opens
// the card built by cards.js.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {appIconActor, buildCard} from './cards.js';
import {StatusIcon} from './statusIcon.js';

const MORPH_MS = 200;
const TONES = ['quiet', 'accent', 'call', 'neutral'];

export const Indicator = GObject.registerClass(
class PhoneIndicator extends PanelMenu.Button {
    // actions: what the cards may ask for (see extension.js).
    // phoneLabel(key): a word of the iPhone, as an English sentence names it.
    // phoneArt(name): a drawing of an iPhone screen (lib/illustrations.js).
    // appIcons: the icons of the iPhone apps (lib/appIcons.js), or null.
    _init({model, actions, iconsDir, gettext, phoneLabel, phoneArt, appIcons = null}) {
        super._init(0.5, gettext('iPhone'));
        this._model = model;
        this._actions = actions;
        this._iconsDir = iconsDir;
        this._ = gettext;
        this._phoneLabel = phoneLabel ?? (key => key);
        this._phoneArt = phoneArt ?? (() => null);
        this._shape = null;
        this._cardKey = null;
        this._card = null;
        this._laterId = 0;
        this._useAccent = false;
        this._laidOut = false;
        this._opening = false;
        this._view = null;
        this._cardJson = null;
        this._iconStyle = 'phone';
        this._passive = null;
        this._passiveKey = null;
        this._passiveCard = null;
        this._appIcons = appIcons;
        // What shows an app icon in the open menu, to follow the ones that come.
        this._iconWatchers = new Set();
        this._iconsVersion = 0;
        this._iconsIdleId = 0;
        this._appsKey = null;

        this.add_style_class_name('landline-button');
        this.menu.actor.add_style_class_name('landline-menu');
        // One width for everything that drops under the indicator, whatever
        // the length of a name or of a message.
        this.menu.box.add_style_class_name('landline-menu-box');

        this._pill = new St.BoxLayout({
            style_class: 'landline-pill',
            y_align: Clutter.ActorAlign.CENTER,
        });
        // Always there, always as wide: what it draws says the state.
        this._icon = new StatusIcon();
        // "Sent": a check mark in its place.
        this._check = new St.Icon({
            style_class: 'landline-glyph',
            icon_name: 'object-select-symbolic',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });
        // The icons of the apps of a notification, of a few, of a summary.
        this._apps = new St.BoxLayout({
            style_class: 'landline-pill-apps',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
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
        for (const child of [this._icon, this._check, this._apps, this._avatar, this._strong, this._text])
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
        if (this._destroyed)
            return;
        this._useAccent = use;
        for (const actor of [this, this.menu.actor, this._passive.actor]) {
            if (use)
                actor.add_style_class_name('landline-system-accent');
            else
                actor.remove_style_class_name('landline-system-accent');
        }
    }

    // 'phone' or 'handset': which drawings the icon uses.
    setIconStyle(style) {
        if (this._destroyed)
            return;
        this._iconStyle = style;
        if (this._view)
            this._syncPill(this._view);
    }

    // An icon of an app came, or "Show app icons" changed: the pill and the
    // open menu take it, once for all that came at the same time.
    appIconsChanged() {
        if (this._iconsIdleId)
            return;
        this._iconsIdleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._iconsIdleId = 0;
            this._iconsVersion++;
            for (const watcher of [...this._iconWatchers])
                watcher();
            if (this._view)
                this._syncPill(this._view);
            return GLib.SOURCE_REMOVE;
        });
    }

    _appIcon(appId) {
        return this._appIcons?.icon(appId) ?? null;
    }

    icon(name) {
        if (name.startsWith('landline-'))
            return Gio.icon_new_for_string(`${this._iconsDir}/${name}.svg`);
        return new Gio.ThemedIcon({name});
    }

    // Bring the bar and the open card in line with the model.
    sync() {
        // Torn down by GNOME Shell itself (session closing) before disable():
        // nothing is drawn any more (#58).
        if (this._destroyed)
            return;
        const view = this._model.view();
        this._view = view;
        this.accessible_name = view.a11y;
        this._syncPill(view);
        this._syncCard(view);
        this._syncPassive(view);
    }

    _syncPill(view) {
        const pill = view.pill;
        const shape = [view.mode, view.link, pill.tone, pill.check ? 'check' : '',
            pill.avatar ?? '', pill.text ?? '', (pill.apps ?? []).join(' '), this._iconsVersion,
            view.mode === 'call' ? '' : pill.strong ?? ''].join('|');
        const apply = () => this._applyPill(pill, view.icon);
        if (shape === this._shape) {
            apply();
        } else if (!this.mapped) {
            this._pill.remove_transition('width');
            this._pill.clip_to_allocation = false;
            this._pill.set_width(-1);
            apply();
        } else {
            this._morph(apply);
        }
        this._shape = shape;
    }

    _applyPill(pill, icon) {
        for (const tone of TONES) {
            if (tone === pill.tone)
                this.add_style_class_name(`landline-${tone}`);
            else
                this.remove_style_class_name(`landline-${tone}`);
        }
        // A summary is a sentence: it gets more room than a name.
        if (pill.wide)
            this.add_style_class_name('landline-wide');
        else
            this.remove_style_class_name('landline-wide');
        if (pill.dim)
            this.add_style_class_name('landline-dim');
        else
            this.remove_style_class_name('landline-dim');

        this._check.visible = pill.check === true;
        this._icon.visible = !this._check.visible;
        this._icon.setState({style: this._iconStyle, state: icon.state, moving: icon.moving, dim: icon.dim === true});
        const shown = this._syncApps(pill.apps ?? []);
        // One app: its icon takes the place of its initials.
        this._avatar.visible = !!pill.avatar && !(shown > 0 && pill.apps.length === 1);
        this._avatar.text = pill.avatar ?? '';
        this._strong.visible = !!pill.strong;
        this._strong.text = pill.strong ?? '';
        this._text.visible = !!pill.text;
        this._text.text = (pill.text ?? '').replace(/\s+/g, ' ');
    }

    // The icons of the pill, made again only when they change. Returns how
    // many there are: an app without one is left out.
    _syncApps(apps) {
        const key = `${apps.join(' ')}|${this._iconsVersion}`;
        if (key !== this._appsKey) {
            this._appsKey = key;
            this._apps.destroy_all_children();
            for (const appId of apps) {
                const actor = appIconActor(this._appIcon(appId), 'pill');
                if (actor)
                    this._apps.add_child(actor);
            }
        }
        const count = this._apps.get_n_children();
        this._apps.visible = count > 0;
        // The icons take room of their own: the text keeps its own.
        if (count > 0)
            this.add_style_class_name('landline-with-apps');
        else
            this.remove_style_class_name('landline-with-apps');
        return count;
    }

    // Change the content, then slide the width from the old size to the new
    // one, so the bar never jumps.
    _morph(apply) {
        const pill = this._pill;
        const from = pill.width;
        pill.remove_transition('width');
        pill.clip_to_allocation = false;
        pill.set_width(-1);
        apply();
        const [, to] = pill.get_preferred_width(-1);
        if (Math.abs(to - from) < 1)
            return;
        pill.set_width(from);
        // Clipped only while it slides: at rest the icon may move a little
        // past its own box (a wave, a bubble that hops).
        pill.clip_to_allocation = true;
        pill.ease({
            width: to,
            duration: MORPH_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onStopped: finished => {
                pill.clip_to_allocation = false;
                if (finished)
                    pill.set_width(-1);
            },
        });
    }

    _cardKeyOf(card) {
        // The call card has two layouts: one "Answer", or the two answers.
        return `${card.kind}:${card.thread ?? card.problem ?? card.answerChoice ?? card.uid ?? ''}`;
    }

    _syncCard(view) {
        if (!this.menu.isOpen)
            return;
        if (!view.cardOpen) {
            this.menu.close();
            return;
        }
        // A click on the button would start a dialog ("Link again" while
        // the link is broken): nothing to show in the menu, which closes at
        // once. The click on the button runs it, as when the menu is shut
        // (#52).
        if (view.card.kind === 'direct') {
            this.menu.close({animate: false});
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
            this._card?.focus?.();
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
        // The view may have changed since this was planned (#52).
        if (view.card.kind === 'direct') {
            this._card = null;
            this._cardKey = null;
            this.menu.close({animate: false});
            return;
        }
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
            phoneArt: name => this._phoneArt(name),
            appIcon: appId => this._appIcon(appId),
            watchAppIcons: watcher => {
                this._iconWatchers.add(watcher);
                return () => this._iconWatchers.delete(watcher);
            },
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
        const key = this._cardKeyOf(view.card);
        if (this._passiveKey === null) {
            this._passiveKey = key;
            this._passiveCard = buildCard(this._passive, view.card, this._host(this._passive));
            this._passive.open();
        } else if (this._passive.isOpen && key !== this._passiveKey) {
            // The same call, now with the two ways to answer.
            this._passiveKey = key;
            this._passiveCard = buildCard(this._passive, view.card, this._host(this._passive));
        } else if (this._passive.isOpen) {
            this._passiveCard.update?.(view.card);
        }
    }

    _onDestroy() {
        this._destroyed = true;
        if (this._iconsIdleId) {
            GLib.source_remove(this._iconsIdleId);
            this._iconsIdleId = 0;
        }
        this._iconWatchers.clear();
        this._appIcons = null;
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
