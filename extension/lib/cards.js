// SPDX-License-Identifier: GPL-2.0-or-later
// What drops under the indicator: the short menu at rest, the reply card, the
// two call cards, the pairing code and the cards that explain a problem.
// Each builder empties the menu, fills it and returns
// {update(card), focus()}; either may be missing.

import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Pango from 'gi://Pango';
import St from 'gi://St';

import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

// A menu row that runs a callback and leaves the menu open.
const StayOpenItem = GObject.registerClass(
class PhoneStayOpenItem extends PopupMenu.PopupBaseMenuItem {
    _init(callback) {
        super._init({style_class: 'landline-row'});
        this._callback = callback;
    }

    activate() {
        this._callback();
    }
});

// The first line of the menu at rest. The name sits in the middle of the
// whole line, whatever stands left and right of it: it gets the room both
// sides leave when each is given the width of the wider one, and is cut with
// an ellipsis beyond that. Only when that room would be next to nothing does
// it take the space between the two sides instead. Texts share one baseline.
const TOP_GAP = 10;
const TOP_MIN_NAME = 64;

const TopRow = GObject.registerClass(
class PhoneTopRow extends St.Widget {
    _init(glyph, name, state, battery) {
        super._init({x_expand: true, y_align: Clutter.ActorAlign.CENTER});
        this.glyph = glyph;
        this.nameLabel = name;
        this.stateLabel = state;
        this.batteryLabel = battery;
        for (const child of [glyph, name, state, battery]) {
            if (child)
                this.add_child(child);
        }
    }

    _widths() {
        const natural = actor => actor ? actor.get_preferred_width(-1)[1] : 0;
        const battery = natural(this.batteryLabel);
        const state = natural(this.stateLabel);
        return {
            glyph: natural(this.glyph),
            name: natural(this.nameLabel),
            state,
            battery,
            right: state + (battery && state ? TOP_GAP : 0) + battery,
        };
    }

    vfunc_get_preferred_width(_forHeight) {
        const w = this._widths();
        const sides = w.glyph + w.right + 2 * TOP_GAP;
        return this.get_theme_node().adjust_preferred_width(sides + Math.min(w.name, TOP_MIN_NAME), sides + w.name);
    }

    vfunc_get_preferred_height(_forWidth) {
        let height = 0;
        for (const child of this.get_children())
            height = Math.max(height, child.get_preferred_height(-1)[1]);
        return this.get_theme_node().adjust_preferred_height(height, height);
    }

    vfunc_allocate(box) {
        this.set_allocation(box);
        const content = this.get_theme_node().get_content_box(box);
        const width = content.x2 - content.x1;
        const height = content.y2 - content.y1;
        const w = this._widths();

        // The name: centred on the line.
        const side = Math.max(w.glyph, w.right) + TOP_GAP;
        let nameWidth = Math.min(w.name, Math.max(0, width - 2 * side));
        let nameX = (width - nameWidth) / 2;
        if (nameWidth < Math.min(w.name, TOP_MIN_NAME)) {
            const free = Math.max(0, width - w.glyph - w.right - 2 * TOP_GAP);
            nameWidth = Math.min(w.name, free);
            nameX = w.glyph + TOP_GAP + (free - nameWidth) / 2;
        }

        // One baseline for the three texts, set by the name in the middle
        // of the height.
        const baseline = label => label.clutter_text.get_layout().get_baseline() / Pango.SCALE;
        const nameHeight = this.nameLabel.get_preferred_height(-1)[1];
        const line = (height - nameHeight) / 2 + baseline(this.nameLabel);
        const place = (actor, x, w_, y) => {
            const h = actor.get_preferred_height(-1)[1];
            const child = new Clutter.ActorBox();
            child.x1 = Math.round(content.x1 + x);
            child.x2 = Math.round(content.x1 + x + w_);
            child.y1 = Math.round(content.y1 + (y ?? (height - h) / 2));
            child.y2 = child.y1 + Math.ceil(h);
            actor.allocate(child);
        };
        const onLine = (label, x, w_) => place(label, x, w_, line - baseline(label));

        place(this.glyph, 0, w.glyph);
        onLine(this.nameLabel, nameX, nameWidth);
        let x = width;
        if (this.batteryLabel) {
            x -= w.battery;
            onLine(this.batteryLabel, x, w.battery);
            x -= TOP_GAP;
        }
        onLine(this.stateLabel, x - w.state, w.state);
    }

});

export function buildCard(menu, card, host) {
    menu.removeAll();
    switch (card.kind) {
    case 'reply':
        return replyCard(menu, card, host);
    case 'ring':
        return ringCard(menu, card, host);
    case 'call':
        return callCard(menu, card, host);
    case 'pair':
        return pairCard(menu, card, host);
    case 'problem':
        return problemCard(menu, card, host);
    case 'compose':
        return composeCard(menu, card, host);
    default:
        return restMenu(menu, card, host);
    }
}

// --- pieces ------------------------------------------------------------------

// A menu item that only holds content: not dimmed, not highlighted, not
// activatable.
function plainItem(styleClass) {
    const item = new PopupMenu.PopupBaseMenuItem({
        can_focus: false,
        activate: false,
        hover: false,
        style_class: styleClass,
    });
    item.track_hover = false;
    return item;
}

function cardBox(menu) {
    const item = plainItem('landline-card-item');
    const box = new St.BoxLayout({
        style_class: 'landline-card',
        orientation: Clutter.Orientation.VERTICAL,
        x_expand: true,
    });
    item.add_child(box);
    menu.addMenuItem(item);
    appear(box);
    return box;
}

// New content settles in instead of snapping.
function appear(actor) {
    actor.opacity = 0;
    actor.translation_y = -6;
    actor.ease({
        opacity: 255,
        translation_y: 0,
        duration: 180,
        mode: Clutter.AnimationMode.EASE_OUT_QUAD,
    });
}

// Initials in a disc; a plain silhouette when there are none (a bare number,
// a short code).
function avatar(text, styleClass = '') {
    if (!text) {
        return new St.Icon({
            icon_name: 'avatar-default-symbolic',
            style_class: `landline-avatar landline-avatar-icon ${styleClass}`,
            y_align: Clutter.ActorAlign.CENTER,
        });
    }
    const label = new St.Label({
        text,
        style_class: `landline-avatar ${styleClass}`,
        y_align: Clutter.ActorAlign.CENTER,
    });
    label.clutter_text.x_align = Clutter.ActorAlign.CENTER;
    label.clutter_text.y_align = Clutter.ActorAlign.CENTER;
    return label;
}

function wrapped(text, styleClass) {
    const label = new St.Label({text, style_class: styleClass, x_expand: true});
    label.clutter_text.line_wrap = true;
    label.clutter_text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
    label.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
    return label;
}

// Wrapped text held to a number of lines, the last one ending in "…".
// A text that wraps and ellipsizes only ever asks for one line, so the height
// is worked out here, once the width is known. onChange(truncated) says
// whether there is more to show; the returned function opens and closes it.
function clamped(label, lines, onChange) {
    const text = label.clutter_text;
    text.line_wrap = true;
    text.line_wrap_mode = Pango.WrapMode.WORD_CHAR;
    text.ellipsize = Pango.EllipsizeMode.NONE;
    let expanded = false;
    let applied = '';
    let laterId = 0;
    const fit = () => {
        laterId = 0;
        const width = Math.floor(text.width);
        if (width <= 0)
            return GLib.SOURCE_REMOVE;
        const probe = text.get_layout().copy();
        probe.set_ellipsize(Pango.EllipsizeMode.NONE);
        probe.set_height(-1);
        probe.set_width(width * Pango.SCALE);
        const [, full] = probe.get_pixel_size();
        const count = Math.max(1, probe.get_line_count());
        const truncated = count > lines;
        const key = `${width}|${expanded}|${truncated}`;
        if (key === applied)
            return GLib.SOURCE_REMOVE;
        applied = key;
        if (truncated && !expanded) {
            const {scaleFactor} = St.ThemeContext.get_for_stage(global.stage);
            text.ellipsize = Pango.EllipsizeMode.END;
            label.set_style(`height: ${Math.ceil(full / count * lines / scaleFactor)}px;`);
        } else {
            text.ellipsize = Pango.EllipsizeMode.NONE;
            label.set_style(null);
        }
        onChange(truncated);
        return GLib.SOURCE_REMOVE;
    };
    // Not from inside the allocation itself.
    const later = () => {
        if (!laterId)
            laterId = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, fit);
    };
    text.connect('notify::allocation', later);
    label.connect('destroy', () => {
        if (laterId)
            global.compositor.get_laters().remove(laterId);
        laterId = 0;
    });
    return value => {
        expanded = value;
        later();
    };
}

// Avatar, a name and a line under it.
function header(card, avatarClass) {
    const row = new St.BoxLayout({style_class: 'landline-header'});
    row.add_child(avatar(card.initials, avatarClass));
    const text = new St.BoxLayout({
        orientation: Clutter.Orientation.VERTICAL,
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    });
    const title = new St.Label({text: card.name, style_class: 'landline-title'});
    const subtitle = new St.Label({text: card.subtitle, style_class: 'landline-sub'});
    text.add_child(title);
    text.add_child(subtitle);
    row.add_child(text);
    const setSubtitle = c => {
        subtitle.text = c.subtitle;
        if (c.isError)
            subtitle.add_style_class_name('landline-error');
        else
            subtitle.remove_style_class_name('landline-error');
    };
    setSubtitle(card);
    return {row, setSubtitle};
}

// A wide button with an icon and a word.
function actionButton(host, label, iconName, styleClass, callback) {
    const content = new St.BoxLayout({
        style_class: 'landline-action-content',
        x_align: Clutter.ActorAlign.CENTER,
    });
    if (iconName) {
        content.add_child(new St.Icon({
            gicon: host.icon(iconName),
            style_class: 'landline-action-icon',
            y_align: Clutter.ActorAlign.CENTER,
        }));
    }
    content.add_child(new St.Label({text: label, y_align: Clutter.ActorAlign.CENTER}));
    const button = new St.Button({
        style_class: `button landline-action ${styleClass}`,
        child: content,
        can_focus: true,
        x_expand: true,
        accessible_name: label,
    });
    button.connect('clicked', callback);
    return button;
}

function pair(left, right) {
    const row = new St.BoxLayout({style_class: 'landline-pair', x_expand: true});
    row.layout_manager.homogeneous = true;
    row.add_child(left);
    row.add_child(right);
    return row;
}

function clock(timestamp) {
    if (!timestamp)
        return '';
    const when = GLib.DateTime.new_from_unix_local(timestamp);
    const now = GLib.DateTime.new_now_local();
    if (!when)
        return '';
    if (when.format('%F') === now.format('%F'))
        return when.format('%H:%M');
    if (now.difference(when) < 6 * GLib.TIME_SPAN_DAY)
        return when.format('%a');
    return when.format('%e %b').trim();
}

// --- reply -------------------------------------------------------------------

// One message: a soft bubble, held to a few lines. A long one opens on a
// click and closes on the next.
function bubble(message, lines, host) {
    const _ = host._;
    const column = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
    const button = new St.Button({
        style_class: 'landline-bubble',
        child: column,
        reactive: false,
        can_focus: false,
        x_expand: true,
    });
    if (!message.body) {
        // Bluetooth carries the text only.
        column.add_child(new St.Label({
            text: _('Photo or attachment · see it on your iPhone'),
            style_class: 'landline-body landline-attachment',
        }));
        return button;
    }
    const label = new St.Label({text: message.body, style_class: 'landline-body', x_expand: true});
    const more = new St.Label({text: _('Show more'), style_class: 'landline-more', visible: false});
    const setExpanded = clamped(label, lines, truncated => {
        more.visible = truncated;
        button.reactive = truncated;
        button.can_focus = truncated;
        button.accessible_name = truncated ? message.body : null;
    });
    column.add_child(label);
    column.add_child(more);
    let open = false;
    button.connect('clicked', () => {
        open = !open;
        setExpanded(open);
        more.text = open ? _('Show less') : _('Show more');
    });
    return button;
}

function replyCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);

    const subtitleOf = c => ({
        subtitle: c.subtitle || [_('Messages'), clock(c.timestamp)].filter(s => s).join(' · '),
        isError: c.isError,
    });
    const head = header({...card, ...subtitleOf(card)}, '');
    const close = new St.Button({
        style_class: 'button landline-round',
        child: new St.Icon({icon_name: 'window-close-symbolic', style_class: 'landline-round-icon'}),
        can_focus: true,
        accessible_name: _('Close'),
        y_align: Clutter.ActorAlign.CENTER,
    });
    close.connect('clicked', () => host.close());
    head.row.add_child(close);
    box.add_child(head.row);

    const bubbles = new St.BoxLayout({
        style_class: 'landline-bubbles',
        orientation: Clutter.Orientation.VERTICAL,
        x_expand: true,
    });
    box.add_child(bubbles);
    let shown = '';
    const fill = (c, animate) => {
        const key = JSON.stringify(c.messages);
        if (key === shown)
            return;
        shown = key;
        bubbles.destroy_all_children();
        // A single message may take four lines; several share the room.
        const lines = c.messages.length > 1 ? 3 : 4;
        c.messages.forEach((message, index) => {
            const actor = bubble(message, lines, host);
            bubbles.add_child(actor);
            if (animate && index === c.messages.length - 1)
                appear(actor);
        });
    };
    fill(card, false);

    const chips = new St.BoxLayout({style_class: 'landline-chips', x_expand: true});
    const chipButtons = [];
    for (const text of card.quick.slice(0, 3)) {
        const chip = new St.Button({
            style_class: 'button landline-chip',
            child: new St.Label({text}),
            can_focus: true,
            accessible_name: text,
        });
        chip.connect('clicked', () => host.model.reply(text));
        chips.add_child(chip);
        chipButtons.push(chip);
    }
    if (chipButtons.length > 0)
        box.add_child(chips);

    const entry = new St.Entry({
        style_class: 'landline-entry',
        hint_text: _('Reply…'),
        text: card.draft,
        can_focus: true,
        x_expand: true,
        accessible_name: _('Reply'),
    });
    const send = new St.Button({
        style_class: 'landline-send',
        child: new St.Icon({gicon: host.icon('landline-send-symbolic'), style_class: 'landline-send-icon'}),
        can_focus: true,
        accessible_name: _('Send'),
        y_align: Clutter.ActorAlign.CENTER,
    });
    entry.set_secondary_icon(send);
    const submit = () => host.model.reply(entry.text);
    entry.clutter_text.connect('activate', submit);
    entry.clutter_text.connect('text-changed', () => host.model.setDraft(entry.text));
    send.connect('clicked', submit);
    box.add_child(entry);

    if (!card.repliable) {
        entry.hide();
        chips.hide();
        box.add_child(wrapped(_('This conversation cannot be answered from here.'), 'landline-sub'));
    }

    return {
        update: c => {
            head.setSubtitle(subtitleOf(c));
            fill(c, true);
        },
        focus: () => {
            if (card.repliable)
                entry.grab_key_focus();
        },
    };
}

// --- new message -----------------------------------------------------------------

// A field to look a contact up, and the matches under it. Picking one opens
// the reply card on that conversation.
function composeCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);

    const head = new St.BoxLayout({style_class: 'landline-header'});
    head.add_child(new St.Label({
        text: _('New message'),
        style_class: 'landline-title',
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    }));
    const close = new St.Button({
        style_class: 'button landline-round',
        child: new St.Icon({icon_name: 'window-close-symbolic', style_class: 'landline-round-icon'}),
        can_focus: true,
        accessible_name: _('Close'),
        y_align: Clutter.ActorAlign.CENTER,
    });
    close.connect('clicked', () => host.close());
    head.add_child(close);
    box.add_child(head);

    const entry = new St.Entry({
        style_class: 'landline-entry landline-search',
        hint_text: _('Name or number'),
        text: card.query,
        can_focus: true,
        x_expand: true,
        accessible_name: _('Name or number'),
    });
    entry.clutter_text.connect('text-changed', () => host.model.composeQuery(entry.text));
    entry.clutter_text.connect('activate', () => host.model.composePick(0));
    box.add_child(entry);

    const list = new St.BoxLayout({
        style_class: 'landline-results',
        orientation: Clutter.Orientation.VERTICAL,
        x_expand: true,
    });
    box.add_child(list);
    let shown = null;
    const fill = c => {
        const key = JSON.stringify([c.query, c.results]);
        if (key === shown)
            return;
        shown = key;
        list.destroy_all_children();
        if (c.results.length === 0) {
            list.add_child(wrapped(c.query
                ? _('No contact with that name. A full phone number works too.')
                : _('Type the first letters of a contact, or a phone number.'), 'landline-sub'));
            return;
        }
        c.results.forEach((result, index) => {
            const row = new St.BoxLayout({
                style_class: 'landline-row',
                x_expand: true,
                x_align: Clutter.ActorAlign.START,
            });
            row.add_child(avatar(result.initials, ''));
            const text = new St.BoxLayout({
                orientation: Clutter.Orientation.VERTICAL,
                x_expand: true,
                y_align: Clutter.ActorAlign.CENTER,
            });
            text.add_child(new St.Label({text: result.name, style_class: 'landline-row-name'}));
            if (result.detail)
                text.add_child(new St.Label({text: result.detail, style_class: 'landline-sub'}));
            row.add_child(text);
            const button = new St.Button({
                style_class: 'button landline-result',
                child: row,
                can_focus: true,
                x_expand: true,
                accessible_name: [result.name, result.detail].filter(t => t).join(', '),
            });
            button.connect('clicked', () => host.model.composePick(index));
            list.add_child(button);
        });
    };
    fill(card);

    return {
        update: fill,
        focus: () => entry.grab_key_focus(),
    };
}

// --- calls -------------------------------------------------------------------

function ringCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    const head = header(card, 'landline-avatar-large landline-avatar-accent');
    box.add_child(head.row);
    const decline = actionButton(host, _('Decline'), 'landline-hangup-symbolic',
        'landline-decline', () => host.model.decline());
    if (!card.answerChoice) {
        const answer = actionButton(host, _('Answer'), 'call-start-symbolic',
            'landline-answer', () => host.model.answer());
        box.add_child(pair(decline, answer));
        return {
            update: c => head.setSubtitle(c),
        };
    }
    // This computer can carry the sound: answering says where it goes,
    // instead of one "Answer" that may leave the call silent.
    const here = actionButton(host, _('Answer here'), 'call-start-symbolic',
        'landline-answer', () => host.model.answer('computer'));
    const onPhone = actionButton(host, _('Answer on iPhone'), 'landline-symbolic',
        'landline-answer landline-answer-phone', () => host.model.answer('phone'));
    // One under the other: each label is read in full.
    box.add_child(here);
    box.add_child(onPhone);
    box.add_child(decline);
    box.add_child(wrapped(
        _('Here: the call plays through this computer. On iPhone: pick up the phone to talk.'),
        'landline-sub'));
    return {
        update: c => head.setSubtitle(c),
    };
}

function callCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    const head = header(card, 'landline-avatar-large landline-avatar-accent');
    box.add_child(head.row);

    const choice = (label, where) => {
        const button = new St.Button({
            style_class: 'button landline-choice',
            label,
            can_focus: true,
            toggle_mode: false,
            x_expand: true,
        });
        button.connect('clicked', () => host.model.setCallAudio(where));
        return button;
    };
    const toPhone = choice(_('iPhone'), 'phone');
    const toComputer = choice(_('This computer'), 'computer');
    box.add_child(pair(toPhone, toComputer));
    const hangUp = actionButton(host, _('Hang Up'), 'landline-hangup-symbolic',
        'landline-decline', () => host.model.hangUp());
    box.add_child(hangUp);
    const hint = wrapped(card.hint ?? '', 'landline-sub');
    hint.visible = !!card.hint;
    box.add_child(hint);

    const setAudio = c => {
        hint.text = c.hint ?? '';
        hint.visible = !!c.hint;
        for (const [button, where] of [[toPhone, 'phone'], [toComputer, 'computer']]) {
            if (c.audio === where)
                button.add_style_class_name('landline-chosen');
            else
                button.remove_style_class_name('landline-chosen');
            // Asked for, not there yet.
            if (c.audio === where && c.moving)
                button.add_style_class_name('landline-moving');
            else
                button.remove_style_class_name('landline-moving');
            button.accessible_role = Atk.Role.RADIO_BUTTON;
        }
    };
    setAudio(card);
    return {
        update: c => {
            head.setSubtitle(c);
            setAudio(c);
        },
    };
}

// --- pairing code ------------------------------------------------------------

function pairCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    box.add_child(new St.Label({
        text: _('Same number on both?'),
        style_class: 'landline-title landline-centered',
        x_align: Clutter.ActorAlign.CENTER,
    }));
    const text = wrapped(
        _('Your iPhone shows a number too. If they match, tap Pair on the phone and confirm here.'),
        'landline-sub landline-centered');
    text.clutter_text.line_alignment = Pango.Alignment.CENTER;
    box.add_child(text);
    const code = String(card.code);
    box.add_child(new St.Label({
        text: code.length === 6 ? `${code.slice(0, 3)} ${code.slice(3)}` : code,
        style_class: 'landline-code',
        x_align: Clutter.ActorAlign.CENTER,
    }));
    const differ = actionButton(host, _('They Differ'), null, '', () => host.model.confirmPairing(false));
    const match = actionButton(host, _('They Match'), null, 'landline-primary',
        () => host.model.confirmPairing(true));
    box.add_child(pair(differ, match));
    return {};
}

// --- problems ----------------------------------------------------------------

function problemCard(menu, card, host) {
    const box = cardBox(menu);
    box.add_child(wrapped(card.title, 'landline-title'));
    box.add_child(wrapped(card.text, 'landline-text'));
    for (const [index, step] of (card.steps ?? []).entries()) {
        const row = new St.BoxLayout({style_class: 'landline-step'});
        row.add_child(avatar(String(index + 1), 'landline-step-number'));
        row.add_child(wrapped(step, 'landline-sub'));
        box.add_child(row);
    }
    if (card.action) {
        box.add_child(actionButton(host, card.action.label, null, 'landline-primary',
            () => host.actions.run(card.action.id, card)));
    }
    return {};
}

// --- the short menu at rest ----------------------------------------------------

// A row with a switch that flips in place: the menu stays open, so the new
// state is seen.
function switchItem(menu, label, active, onToggle) {
    const toggle = new PopupMenu.Switch(active);
    const item = new StayOpenItem(() => {
        toggle.state = !toggle.state;
        item.accessible_name = label;
        if (toggle.state)
            item.add_accessible_state(Atk.StateType.CHECKED);
        else
            item.remove_accessible_state(Atk.StateType.CHECKED);
        onToggle(toggle.state);
    });
    item.accessible_role = Atk.Role.CHECK_MENU_ITEM;
    if (active)
        item.add_accessible_state(Atk.StateType.CHECKED);
    item.add_child(new St.Label({text: label, x_expand: true, y_align: Clutter.ActorAlign.CENTER}));
    item.add_child(toggle);
    menu.addMenuItem(item);
    return item;
}

function restMenu(menu, card, host) {
    const _ = host._;

    // One line: the glyph on the left, the name of the iPhone in the middle
    // of the menu, the link and the battery on the right.
    const top = plainItem('landline-top');
    const glyph = new St.Icon({
        gicon: host.icon('landline-symbolic'),
        style_class: `landline-top-glyph ${card.linked ? 'landline-top-glyph-on' : ''}`,
    });
    const name = new St.Label({text: card.name, style_class: 'landline-title'});
    name.clutter_text.ellipsize = Pango.EllipsizeMode.END;
    const state = new St.Label({text: card.state, style_class: 'landline-top-state'});
    let battery = null;
    if (card.battery !== null) {
        battery = new St.Label({
            // Translators: battery level of the iPhone.
            text: _('%d %%').replace('%d', card.battery).replace('%%', '%'),
            style_class: 'landline-battery-label',
        });
        if (card.battery < 15)
            battery.add_style_class_name('landline-battery-low');
    }
    const row = new TopRow(glyph, name, state, battery);
    top.add_child(row);
    top.accessible_name = [
        card.name, card.statusLine,
        // Translators: read aloud after the name of the iPhone; %d is the battery level.
        card.battery !== null ? _('battery %d %%').replace('%d', card.battery).replace('%%', '%') : '',
    ].filter(t => t).join(', ');
    menu.addMenuItem(top);

    for (const notice of card.notices) {
        const item = plainItem('landline-notice');
        item.add_child(wrapped(notice, 'landline-sub'));
        menu.addMenuItem(item);
    }

    if (card.permissionAction) {
        menu.addAction(_('Finish Setting Up This Computer…'),
            () => host.actions.run('permission', card));
    } else if (card.setupAction) {
        menu.addAction(_('Continue Setup…'), () => host.actions.run('link', card));
    }
    if (card.fixAction)
        menu.addAction(_('Fix It'), () => host.actions.run('fix', card));

    for (const thread of card.threads) {
        const item = new StayOpenItem(() => host.model.openThread(thread.thread));
        item.add_child(avatar(thread.initials, ''));
        const text = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        const line = new St.BoxLayout({style_class: 'landline-row-line'});
        line.add_child(new St.Label({text: thread.name, style_class: 'landline-row-name', x_expand: true}));
        line.add_child(new St.Label({
            text: thread.unread ? _('new') : clock(thread.timestamp),
            style_class: thread.unread ? 'landline-new' : 'landline-when',
            y_align: Clutter.ActorAlign.CENTER,
        }));
        text.add_child(line);
        text.add_child(new St.Label({text: thread.preview, style_class: 'landline-sub'}));
        item.add_child(text);
        item.accessible_name = `${thread.name}, ${thread.preview}`;
        menu.addMenuItem(item);
    }
    if (card.threads.length === 0 && card.linked) {
        const item = new PopupMenu.PopupMenuItem(_('No conversations yet'), {reactive: false, can_focus: false});
        menu.addMenuItem(item);
    }

    menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

    const calls = new PopupMenu.PopupSubMenuMenuItem(_('Recent calls'));
    if (card.history.length === 0) {
        calls.menu.addMenuItem(new PopupMenu.PopupMenuItem(_('No recent calls'), {reactive: false, can_focus: false}));
    }
    for (const entry of card.history) {
        const item = plainItem('landline-history');
        const iconName = {
            missed: 'call-missed-symbolic',
            dialed: 'call-outgoing-symbolic',
            outgoing: 'call-outgoing-symbolic',
        }[entry.type] ?? 'call-incoming-symbolic';
        item.add_child(new St.Icon({icon_name: iconName, style_class: 'popup-menu-icon'}));
        item.add_child(new St.Label({text: entry.name, x_expand: true, y_align: Clutter.ActorAlign.CENTER}));
        item.add_child(new St.Label({
            text: entry.type === 'missed' ? `${_('Missed')} · ${clock(entry.timestamp)}` : clock(entry.timestamp),
            style_class: 'landline-sub',
            y_align: Clutter.ActorAlign.CENTER,
        }));
        calls.menu.addMenuItem(item);
    }
    menu.addMenuItem(calls);

    if (card.hotspot) {
        // The tethering profile NetworkManager keeps for the iPhone.
        const state = card.hotspot.state;
        switchItem(menu, _('Use iPhone’s Internet Connection'), state !== 'off',
            on => host.actions.run('hotspot', {on}));
        const line = state === 'connecting' ? _('Connecting…')
            : state === 'on' ? _('Connected: this computer is online through the iPhone.')
                : card.hotspot.failed
                    // Translators: %s is "Personal Hotspot", as the iPhone writes it.
                    ? _('Turn on %s on your iPhone, then try again.').replace('%s', host.phoneLabel('hotspot'))
                    : '';
        if (line) {
            const item = plainItem('landline-notice landline-under-switch');
            const text = wrapped(line, 'landline-sub');
            if (card.hotspot.failed && state === 'off')
                text.add_style_class_name('landline-error');
            item.add_child(text);
            menu.addMenuItem(item);
        }
    }
    if (card.phoneAudioHere !== null && card.phoneAudioHere !== undefined) {
        switchItem(menu, _('Play iPhone audio on this computer'),
            card.phoneAudioHere === true, state => host.model.setPhoneAudio(state));
    }
    switchItem(menu, _('Mute iPhone on this computer'), card.muted === true,
        state => host.actions.run('mute', {muted: state}));

    if (card.composeAction) {
        const item = new StayOpenItem(() => host.model.startCompose());
        item.add_child(new St.Label({text: _('New Message…'), x_expand: true}));
        menu.addMenuItem(item);
    }
    // No "Forget This iPhone…" here: one slip of the hand away from the
    // other rows. It lives in Phone Settings.
    menu.addAction(_('Phone Settings'), () => host.actions.run('settings', card));

    return {};
}
