// SPDX-License-Identifier: GPL-2.0-or-later
// What drops under the indicator: the short menu at rest, the reply card, the
// two call cards, the pairing code and the cards that explain a problem.
// Each builder empties the menu, fills it and returns
// {update(card), focus()}; either may be missing.

import Cairo from 'cairo';
import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
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
// On the right, the battery: its level in words, then a small battery lying
// on its side (upright, it would look like the phone of the icon).
const TOP_GAP = 10;
const TOP_MIN_NAME = 64;
const GAUGE_GAP = 5;

const TopRow = GObject.registerClass(
class PhoneTopRow extends St.Widget {
    _init(glyph, name, battery, gauge) {
        super._init({x_expand: true, y_align: Clutter.ActorAlign.CENTER});
        this.glyph = glyph;
        this.nameLabel = name;
        this.batteryLabel = battery;
        this.gauge = gauge;
        for (const child of [glyph, name, battery, gauge]) {
            if (child)
                this.add_child(child);
        }
    }

    _widths() {
        const natural = actor => actor ? actor.get_preferred_width(-1)[1] : 0;
        const battery = natural(this.batteryLabel);
        const gauge = natural(this.gauge);
        return {
            glyph: natural(this.glyph),
            name: natural(this.nameLabel),
            battery,
            gauge,
            right: battery + (battery && gauge ? GAUGE_GAP : 0) + gauge,
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

        // One baseline for the two texts, set by the name in the middle
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
        if (this.gauge) {
            x -= w.gauge;
            place(this.gauge, x, w.gauge);
            x -= GAUGE_GAP;
        }
        if (this.batteryLabel)
            onLine(this.batteryLabel, x - w.battery, w.battery);
    }
});

// The battery of the iPhone, lying on its side, filled to its level, in the
// color of its style class (red when low, dim when the level is old). A
// lightning bolt is cut out of it while it charges, when that is known.
const BatteryGauge = GObject.registerClass(
class PhoneBatteryGauge extends St.DrawingArea {
    _init(level, charging) {
        super._init({style_class: 'landline-battery-gauge', y_align: Clutter.ActorAlign.CENTER});
        this._level = Math.max(0, Math.min(100, level));
        this._charging = charging === true;
    }

    vfunc_repaint() {
        const cr = this.get_context();
        const [width, height] = this.get_surface_size();
        const color = this.get_theme_node().get_foreground_color();
        cr.setSourceRGBA(color.red / 255, color.green / 255, color.blue / 255, color.alpha / 255);
        const unit = height / 11;
        const rounded = (x, y, w, h, r) => {
            cr.newSubPath();
            cr.arc(x + w - r, y + r, r, -Math.PI / 2, 0);
            cr.arc(x + w - r, y + h - r, r, 0, Math.PI / 2);
            cr.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI);
            cr.arc(x + r, y + r, r, Math.PI, 3 * Math.PI / 2);
            cr.closePath();
        };
        // The case, and its terminal on the right.
        const stroke = Math.max(1, Math.round(unit * 1.1));
        const nub = 2 * unit;
        const bodyW = width - nub - stroke;
        rounded(stroke / 2, stroke / 2, bodyW, height - stroke, 3 * unit);
        cr.setLineWidth(stroke);
        cr.stroke();
        rounded(width - nub - stroke / 2, height * 0.33, nub + stroke / 2, height * 0.34, unit * 0.8);
        cr.fill();
        // The charge: a gap inside the case, then the level.
        const inset = stroke + unit;
        const innerW = bodyW + stroke - 2 * inset;
        const filled = this._level > 0 ? Math.max(unit * 1.2, innerW * this._level / 100) : 0;
        if (filled > 0) {
            rounded(inset, inset, filled, height - 2 * inset, 1.5 * unit);
            cr.fill();
        }
        if (this._charging) {
            // A bolt cut out of everything drawn.
            cr.setOperator(Cairo.Operator.CLEAR);
            const cx = stroke / 2 + bodyW / 2;
            const cy = height / 2;
            cr.moveTo(cx + 1.2 * unit, cy - 4.6 * unit);
            cr.lineTo(cx - 2.4 * unit, cy + 0.7 * unit);
            cr.lineTo(cx - 0.1 * unit, cy + 0.7 * unit);
            cr.lineTo(cx - 1.2 * unit, cy + 4.6 * unit);
            cr.lineTo(cx + 2.4 * unit, cy - 0.7 * unit);
            cr.lineTo(cx + 0.1 * unit, cy - 0.7 * unit);
            cr.closePath();
            cr.fill();
        }
        cr.$dispose();
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
    case 'dial':
        return dialCard(menu, card, host);
    case 'dialing':
        return dialingCard(menu, card, host);
    case 'phonelink':
        return phoneLinkCard(menu, card, host);
    case 'note':
        return noteCard(menu, card, host);
    case 'menu':
        return restMenu(menu, card, host);
    default:
        // A card with no content of its own ('direct': the click starts a
        // dialog). The indicator closes the menu; nothing is built (#52).
        return {};
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

// The icon of an iPhone app, as AppIcons found it: the one of the App Store,
// with the rounded corners of the iPhone (drawn by St, from the PNG of the
// cache), or the one of the same app on this computer. null when there is
// none. size: 'pill', 'small' (the menu) or 'large' (a card).
export function appIconActor(found, size) {
    const style_class = `landline-app-icon landline-app-icon-${size}`;
    // A path that a CSS string could not hold is shown square.
    if (found?.file && !/["\\\n]/.test(found.file)) {
        const bin = new St.Bin({style_class, y_align: Clutter.ActorAlign.CENTER});
        bin.set_style(`background-image: url("${found.file}");`);
        return bin;
    }
    const gicon = found?.file ? Gio.icon_new_for_string(found.file) : found?.gicon;
    if (!gicon)
        return null;
    return new St.Icon({gicon, style_class: `${style_class} landline-app-icon-plain`, y_align: Clutter.ActorAlign.CENTER});
}

// The icon of an app, or its initials in a disc until there is one. It
// follows the icons that come while it is shown.
function appAvatar(host, appId, initials, size, avatarClass) {
    const slot = new St.Bin({y_align: Clutter.ActorAlign.CENTER});
    let key = null;
    const fill = () => {
        const found = host.appIcon?.(appId) ?? null;
        const next = found?.file ?? found?.gicon?.to_string() ?? '';
        if (next === key)
            return;
        key = next;
        const old = slot.child;
        slot.child = appIconActor(found, size) ?? avatar(initials, avatarClass);
        old?.destroy();
    };
    fill();
    const unwatch = host.watchAppIcons?.(fill);
    if (unwatch)
        slot.connect('destroy', unwatch);
    return slot;
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

// Avatar (or what stands in its place), a name and a line under it.
function header(card, avatarClass, lead = null) {
    const row = new St.BoxLayout({style_class: 'landline-header'});
    row.add_child(lead ?? avatar(card.initials, avatarClass));
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

// A button that cannot be used right now: dimmed, out of the keyboard's way.
function setUsable(button, usable) {
    button.reactive = usable;
    button.can_focus = usable;
    button.opacity = usable ? 255 : 110;
}

// A small round button holding one icon.
function roundButton(iconName, label, callback) {
    const button = new St.Button({
        style_class: 'button landline-round',
        child: new St.Icon({icon_name: iconName, style_class: 'landline-round-icon'}),
        can_focus: true,
        accessible_name: label,
        y_align: Clutter.ActorAlign.CENTER,
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
    if (card.callable) {
        // Opens the card to place a call on this person; nothing is dialled
        // by this click.
        head.row.add_child(roundButton('call-start-symbolic', _('Call'),
            () => host.model.startDial({number: card.callable, name: card.name})));
    }
    head.row.add_child(roundButton('window-close-symbolic', _('Close'), () => host.close()));
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
        box.add_child(wrapped(card.arriving
            ? _('Getting the conversation from the iPhone… You can answer in a moment.')
            : _('This conversation cannot be answered from here.'), 'landline-sub'));
    }

    // A reply is on its way: the next one waits for it.
    const setBusy = c => {
        setUsable(send, !c.busy);
        for (const chip of chipButtons)
            setUsable(chip, !c.busy);
    };
    setBusy(card);

    return {
        update: c => {
            head.setSubtitle(subtitleOf(c));
            fill(c, true);
            setBusy(c);
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
    head.add_child(roundButton('window-close-symbolic', _('Close'), () => host.close()));
    box.add_child(head);

    const search = lookup(box, card, {
        nameOrNumber: _('Name or number'),
        empty: _('Type the first letters of a contact, or a phone number.'),
        none: _('No contact with that name. A full phone number works too.'),
        onQuery: text => host.model.composeQuery(text),
        onPick: index => host.model.composePick(index),
    });

    return {
        update: search.fill,
        focus: () => search.entry.grab_key_focus(),
    };
}

// A field and the matches under it: shared by "New Message…" and "Call…".
// `icon`, when given, ends each line (what a click on it does).
function lookup(box, card, {nameOrNumber, empty, none, onQuery, onPick, icon = null}) {
    const entry = new St.Entry({
        style_class: 'landline-entry landline-search',
        hint_text: nameOrNumber,
        text: card.query,
        can_focus: true,
        x_expand: true,
        accessible_name: nameOrNumber,
    });
    entry.clutter_text.connect('text-changed', () => onQuery(entry.text));
    entry.clutter_text.connect('activate', () => onPick(0));
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
            list.add_child(wrapped(c.query ? none : empty, 'landline-sub'));
            return;
        }
        c.results.forEach((result, index) => {
            const row = new St.BoxLayout({
                style_class: 'landline-row',
                x_expand: true,
                x_align: icon ? Clutter.ActorAlign.FILL : Clutter.ActorAlign.START,
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
            if (icon) {
                row.add_child(new St.Icon({
                    icon_name: icon,
                    style_class: 'landline-result-icon',
                    y_align: Clutter.ActorAlign.CENTER,
                }));
            }
            const button = new St.Button({
                style_class: 'button landline-result',
                child: row,
                can_focus: true,
                x_expand: true,
                accessible_name: [result.name, result.detail].filter(t => t).join(', '),
            });
            button.connect('clicked', () => onPick(index));
            list.add_child(button);
        });
    };
    fill(card);
    return {entry, fill};
}

// --- placing a call ---------------------------------------------------------------

// "Call…": a contact looked up or a number typed, and where the sound goes.
// A click on a line places the call.
function dialCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);

    const head = new St.BoxLayout({style_class: 'landline-header'});
    head.add_child(new St.Label({
        text: _('Call'),
        style_class: 'landline-title',
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    }));
    head.add_child(roundButton('window-close-symbolic', _('Close'), () => host.close()));
    box.add_child(head);

    const search = lookup(box, card, {
        nameOrNumber: _('Name or number'),
        empty: _('Type the first letters of a contact, or a phone number.'),
        none: _('No contact with a phone number by that name. A number works too.'),
        onQuery: text => host.model.dialQuery(text),
        onPick: index => host.model.dialPick(index),
        icon: 'call-start-symbolic',
    });

    // Where the sound of this call goes. Not shown with an engine that
    // cannot put it anywhere but where its own setting says.
    let setAudio = () => {};
    if (card.audio !== null && card.audio !== undefined) {
        box.add_child(new St.Label({text: _('Sound of the call'), style_class: 'landline-sub landline-choice-title'}));
        const choice = (label, where) => {
            const button = new St.Button({
                style_class: 'button landline-choice',
                label,
                can_focus: true,
                x_expand: true,
            });
            button.accessible_role = Atk.Role.RADIO_BUTTON;
            button.connect('clicked', () => host.model.setDialAudio(where));
            return button;
        };
        const toComputer = choice(_('This computer'), 'computer');
        const toPhone = choice(_('iPhone'), 'phone');
        box.add_child(pair(toComputer, toPhone));
        setAudio = c => {
            for (const [button, where] of [[toComputer, 'computer'], [toPhone, 'phone']]) {
                if (c.audio === where)
                    button.add_style_class_name('landline-chosen');
                else
                    button.remove_style_class_name('landline-chosen');
            }
        };
        setAudio(card);
    }

    return {
        update: c => {
            search.fill(c);
            setAudio(c);
        },
        focus: () => search.entry.grab_key_focus(),
    };
}

// The call on its way: how far the engine is, and a way out until the
// number is dialled. The same card says why a call could not be placed.
function dialingCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    const head = header(card, 'landline-avatar-large landline-avatar-accent');
    box.add_child(head.row);

    const cancel = actionButton(host, _('Cancel'), null, '', () => host.model.cancelDial());
    box.add_child(cancel);
    const close = actionButton(host, _('Close'), null, '', () => host.close());
    const again = actionButton(host, _('Try Again'), 'call-start-symbolic', 'landline-primary',
        () => host.model.retryDial());
    const afterwards = pair(close, again);
    box.add_child(afterwards);

    const set = c => {
        head.setSubtitle(c);
        cancel.visible = !c.failed && c.cancellable;
        afterwards.visible = c.failed;
    };
    set(card);
    return {
        update: set,
        focus: () => {
            const button = card.failed ? again : cancel;
            if (button.mapped)
                button.grab_key_focus();
        },
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
    const hereLabel = here.child.get_last_child();
    const onPhone = actionButton(host, _('Answer on iPhone'), 'landline-symbolic',
        'landline-answer landline-answer-phone', () => host.model.answer('phone'));
    // One under the other: each label is read in full.
    box.add_child(here);
    box.add_child(onPhone);
    box.add_child(decline);
    const hint = wrapped('', 'landline-sub');
    box.add_child(hint);
    // The call may be announced before this computer is connected for its
    // sound: "Answer here" then waits, the two others work at once.
    const set = c => {
        head.setSubtitle(c);
        const waiting = c.answerHere === 'connecting';
        const missing = c.answerHere === 'unavailable';
        // Translators: on the button that takes the call on this computer,
        // while this computer is still connecting for the sound.
        hereLabel.text = waiting ? _('Answer here · Connecting…')
            // Translators: on the same button, when it could not connect.
            : missing ? _('Answer here · unavailable') : _('Answer here');
        here.accessible_name = hereLabel.text;
        setUsable(here, !waiting && !missing);
        setUsable(onPhone, c.answerOnPhone !== false);
        setUsable(decline, c.canDecline !== false);
        hint.text = waiting
            ? _('This computer is connecting for the sound. Decline and Answer on iPhone work right away.')
            : missing
                ? _('This computer couldn’t connect in time. Answer on the iPhone: the sound can be moved here afterwards.')
                : _('Here: the call plays through this computer. On iPhone: pick up the phone to talk.');
    };
    set(card);
    return {
        update: set,
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
        // Picked up on the iPhone before this computer was connected: the
        // sound can come here once it is.
        const waiting = c.computerReady === false;
        hint.text = c.hint || (waiting ? _('This computer is still connecting. The sound can be moved here in a moment.') : '');
        hint.visible = hint.text !== '';
        setUsable(toComputer, !waiting);
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

// --- a notification of another app ---------------------------------------------

function noteCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    const {row} = header({name: card.appName, subtitle: clock(card.timestamp)}, '',
        appAvatar(host, card.appId, card.initials, 'large', ''));
    box.add_child(row);
    if (card.title)
        box.add_child(wrapped(card.title, 'landline-note-title'));
    if (card.body) {
        const body = new St.Label({text: card.body, style_class: 'landline-body', x_expand: true});
        clamped(body, 8, () => {});
        box.add_child(body);
    }
    // What Bluetooth did not carry, and where it is: "Voice message — play
    // it in Signal".
    const handoff = card.handoff;
    if (handoff?.media)
        box.add_child(wrapped(handoff.media, 'landline-sub landline-note-media'));
    // The official app is not on this computer: said once, in one line.
    if (card.suggestion) {
        const hint = new St.BoxLayout({
            style_class: 'landline-note-suggestion',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        hint.add_child(wrapped(card.suggestion.text, 'landline-sub'));
        const words = new St.BoxLayout({style_class: 'landline-internet-actions'});
        words.add_child(wordButton(card.suggestion.get, () => {
            host.actions.run('get-app', card);
            host.close();
        }));
        words.add_child(wordButton(_('Don’t Suggest Apps'), () => host.model.stopSuggesting()));
        hint.add_child(words);
        box.add_child(hint);
    }
    const open = handoff?.open && handoff.label;
    const dismiss = actionButton(host, _('Dismiss'), null, open ? '' : 'landline-primary', () => {
        host.model.clearNotification(card.uid);
        host.close();
    });
    dismiss.accessible_name = card.canClear ? _('Dismiss, here and on the iPhone') : _('Dismiss');
    if (open) {
        // Opening the app is not dismissing the notification: nothing is
        // cleared on the iPhone.
        const button = actionButton(host, handoff.label, null, 'landline-primary', () => {
            host.actions.run('open-app', card);
            host.close();
        });
        if (handoff.conversation) {
            // Translators: %s is "Open in Signal" or the like.
            button.accessible_name = _('%s, on this conversation').replace('%s', handoff.label);
        }
        // Side by side when both labels fit; a long one ("Open in
        // Telegram Web") gets the whole width, above Dismiss.
        if (handoff.label.length <= 15) {
            box.add_child(pair(dismiss, button));
        } else {
            box.add_child(button);
            box.add_child(dismiss);
        }
    } else {
        box.add_child(dismiss);
    }
    return {};
}

// --- a phone link opened on this computer ---------------------------------------

// "Call Camille Martin?": a tel: or callto: link was clicked somewhere.
// Nothing is dialled before "Call"; then the card of "Call…" takes over.
function phoneLinkCard(menu, card, host) {
    const _ = host._;
    const box = cardBox(menu);
    const head = header(card, 'landline-avatar-large landline-avatar-accent');
    box.add_child(head.row);
    const why = wrapped(card.why, 'landline-sub landline-error');
    box.add_child(why);

    // Where the sound goes: shown while the call can be placed, with an
    // engine that can be told.
    const audioTitle = new St.Label({text: _('Sound of the call'), style_class: 'landline-sub landline-choice-title'});
    box.add_child(audioTitle);
    const choice = (label, where) => {
        const button = new St.Button({
            style_class: 'button landline-choice',
            label,
            can_focus: true,
            x_expand: true,
        });
        button.accessible_role = Atk.Role.RADIO_BUTTON;
        button.connect('clicked', () => host.model.setPhoneLinkAudio(where));
        return button;
    };
    const toComputer = choice(_('This computer'), 'computer');
    const toPhone = choice(_('iPhone'), 'phone');
    const audio = pair(toComputer, toPhone);
    box.add_child(audio);

    // The iPhone may come or go while the card is open: both rows are
    // there, one is shown.
    const cancel = actionButton(host, _('Cancel'), null, '', () => host.model.cancelPhoneLink());
    // The Enter or Space that confirmed "Open Landline?" in the browser, or
    // a key typed as the card drops, must never dial (#53): the keyboard
    // focus starts on Cancel, and a key on Call is not taken in the first
    // moments. A click, or Tab then a key, places the call.
    const shownAt = GLib.get_monotonic_time();
    const call = actionButton(host, _('Call'), 'call-start-symbolic', 'landline-answer', () => {
        if (byKey(Clutter.get_current_event()) && GLib.get_monotonic_time() - shownAt < PHONE_LINK_KEY_GUARD_US)
            return;
        host.model.confirmPhoneLink();
    });
    const ask = pair(cancel, call);
    box.add_child(ask);
    const close = actionButton(host, _('Close'), null, 'landline-primary', () => host.model.cancelPhoneLink());
    box.add_child(close);

    const set = c => {
        head.setSubtitle(c);
        why.text = c.why;
        why.visible = c.why !== '';
        const choosing = c.audio !== null && c.audio !== undefined;
        audioTitle.visible = choosing;
        audio.visible = choosing;
        for (const [button, where] of [[toComputer, 'computer'], [toPhone, 'phone']]) {
            if (c.audio === where)
                button.add_style_class_name('landline-chosen');
            else
                button.remove_style_class_name('landline-chosen');
        }
        ask.visible = c.canCall;
        close.visible = !c.canCall;
    };
    set(card);
    return {
        update: set,
        focus: () => {
            const button = card.canCall ? cancel : close;
            if (button.mapped)
                button.grab_key_focus();
        },
    };
}

// How long after the phone link card shows a key on "Call" is not taken.
const PHONE_LINK_KEY_GUARD_US = 500 * 1000;

function byKey(event) {
    const type = event?.type();
    return type === Clutter.EventType.KEY_PRESS || type === Clutter.EventType.KEY_RELEASE;
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
    if (card.secondary) {
        box.add_child(actionButton(host, card.secondary.label, null, '',
            () => host.actions.run(card.secondary.id, card)));
    }
    return {};
}

// --- the short menu at rest ----------------------------------------------------

// A row with a switch that flips in place: the menu stays open, so the new
// state is seen.
function switchItem(menu, label, active, onToggle) {
    const toggle = new PopupMenu.Switch(active);
    // The switch of GNOME 51 is reactive: a click or a drag on its knob
    // moves it without activating the row (#38). Whatever moves it, the row,
    // the knob or the keyboard, goes through its state.
    const item = new StayOpenItem(() => toggle.toggle());
    toggle.connect('notify::state', () => {
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

// "Internet from iPhone": one switch, and under it what is happening, why
// it failed and the one thing to do; the iPhone's Personal Hotspot screen
// drawn when the iPhone did not share (model/internet.js).
function internetSection(menu, internet, host) {
    const _ = host._;
    switchItem(menu, _('Internet from iPhone'), internet.on, on => host.model.setInternet(on));
    if (!internet.line)
        return;
    const item = plainItem('landline-notice landline-under-switch');
    const box = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
    const line = wrapped(internet.line, 'landline-sub');
    if (internet.error)
        line.add_style_class_name('landline-error');
    box.add_child(line);
    if (internet.remedy)
        box.add_child(wrapped(internet.remedy, 'landline-sub'));
    const art = internet.guide && internet.guideShown ? host.phoneArt?.('hotspot') : null;
    if (art) {
        box.add_child(new St.Icon({
            gicon: art,
            icon_size: 150,
            style_class: 'landline-internet-art',
            x_align: Clutter.ActorAlign.CENTER,
        }));
    }
    const buttons = new St.BoxLayout({style_class: 'landline-internet-actions'});
    if (internet.action) {
        buttons.add_child(wordButton(internet.action.label, () => {
            if (internet.action.id === 'keep')
                host.model.keepInternet();
            else
                host.model.setInternet(true);
        }));
    }
    if (internet.guide) {
        buttons.add_child(wordButton(internet.guideShown ? _('Hide') : _('Show Me How'),
            () => host.model.toggleInternetGuide()));
    }
    if (buttons.get_n_children() > 0)
        box.add_child(buttons);
    item.add_child(box);
    menu.addMenuItem(item);
}

// Under "Play iPhone audio on this computer": where the sound stands. In the
// lean link mode the classic link opens for it, which takes a few seconds.
function phoneAudioLines(menu, audio, host) {
    const {_} = host;
    const lines = [];
    switch (audio?.state) {
    case 'connecting':
        lines.push([_('Connecting…'), false]);
        break;
    case 'playing':
        lines.push([_('Playing on this computer.'), false]);
        if (audio.hint) {
            lines.push([
                // Translators: the first %s is "Control Center" as the iPhone
                // writes it, the second the name of this computer.
                _('If the music stays on the iPhone, open %s, touch the AirPlay button and choose “%s”.')
                    .replace('%s', host.phoneLabel('controlCenter')).replace('%s', audio.computer),
                false]);
        }
        break;
    case 'standby':
        // Lean mode (#43): the link is let go after a silence, and not
        // opened again for the choice alone.
        lines.push([_('Not connected for sound right now.'), false]);
        break;
    case 'failed':
        lines.push([_('The iPhone did not connect for sound. Check that its Bluetooth is on and that it is nearby, then try again.'), true]);
        break;
    default:
        break;
    }
    for (const [line, error] of lines) {
        const item = plainItem('landline-notice landline-under-switch');
        const text = wrapped(line, 'landline-sub');
        if (error)
            text.add_style_class_name('landline-error');
        item.add_child(text);
        menu.addMenuItem(item);
    }
    if (audio?.state === 'standby' || audio?.state === 'failed') {
        const item = new StayOpenItem(() => host.model.connectPhoneAudio());
        item.add_child(new St.Label({text: _('Connect for Sound'), x_expand: true,
            y_align: Clutter.ActorAlign.CENTER}));
        menu.addMenuItem(item);
    }
}

// How old the battery level is, for the screen reader.
function staleness(minutes, _) {
    if (minutes < 2)
        return _('last known a moment ago');
    if (minutes < 120)
        // Translators: %d is a number of minutes, 2 or more.
        return _('last known %d minutes ago').replace('%d', minutes);
    // Translators: %d is a number of hours, 2 or more.
    return _('last known %d hours ago').replace('%d', Math.floor(minutes / 60));
}

// A small button made of words, at the end of a heading.
function wordButton(label, callback) {
    const button = new St.Button({
        style_class: 'button landline-word',
        label,
        can_focus: true,
        y_align: Clutter.ActorAlign.CENTER,
    });
    button.connect('clicked', callback);
    return button;
}

// One notification of another app, as a line: who or what, when, the first
// words. A click opens its card.
function noteRow(note, host) {
    const _ = host._;
    const item = new StayOpenItem(() => host.model.openNote(note.uid));
    item.add_style_class_name('landline-note');
    const text = new St.BoxLayout({
        orientation: Clutter.Orientation.VERTICAL,
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    });
    const line = new St.BoxLayout({style_class: 'landline-row-line'});
    const excerpt = note.body.replace(/\s+/g, ' ').trim();
    line.add_child(new St.Label({
        text: note.title || excerpt || note.appName,
        style_class: 'landline-row-name',
        x_expand: true,
    }));
    line.add_child(new St.Label({
        text: clock(note.timestamp),
        style_class: 'landline-when',
        y_align: Clutter.ActorAlign.CENTER,
    }));
    text.add_child(line);
    if (note.title && excerpt)
        text.add_child(new St.Label({text: excerpt, style_class: 'landline-sub'}));
    item.add_child(text);
    item.accessible_name = [note.appName, note.title, excerpt].filter(t => t).join(', ');
    return item;
}

// "On your iPhone": what waits there right now, by app, the latest first.
function phoneSection(menu, phone, host) {
    const _ = host._;
    menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
    const head = plainItem('landline-phone-head');
    head.add_child(new St.Label({
        text: _('On your iPhone'),
        style_class: 'landline-section',
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    }));
    if (phone.clearable) {
        const clear = wordButton(_('Clear All'), () => host.model.clearAllNotifications());
        clear.accessible_name = _('Clear all, here and on the iPhone');
        head.add_child(clear);
    }
    menu.addMenuItem(head);
    // The lines scroll past a certain height: a long list never pushes the
    // rest of the menu off the screen.
    const holder = plainItem('landline-phone-list');
    const list = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
    const scroll = new St.ScrollView({
        style_class: 'landline-phone-scroll',
        hscrollbar_policy: St.PolicyType.NEVER,
        vscrollbar_policy: St.PolicyType.AUTOMATIC,
        overlay_scrollbars: true,
        x_expand: true,
        child: list,
    });
    holder.add_child(scroll);
    // The lines leave the scroll view before it goes: destroyed with them
    // inside, it is laid out once more after its scroll bars are gone.
    holder.connect('destroy', () => {
        scroll.child = null;
        list.destroy();
    });
    menu.addMenuItem(holder);
    for (const group of phone.groups) {
        const app = new St.BoxLayout({style_class: 'landline-app'});
        app.add_child(appAvatar(host, group.appId, group.initials, 'small', 'landline-app-avatar'));
        app.add_child(new St.Label({
            text: group.appName,
            style_class: 'landline-app-name',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        list.add_child(app);
        for (const note of group.notes) {
            const item = noteRow(note, host);
            // Translators: read aloud on the button that dismisses a notification; %s is its title.
            item.add_child(roundButton('window-close-symbolic', _('Dismiss %s').replace('%s', note.title || note.appName),
                () => host.model.clearNotification(note.uid)));
            list.add_child(item);
        }
    }
    if (phone.hidden > 0) {
        const more = new StayOpenItem(() => host.model.showAllNotes());
        // Translators: %d is how many notifications wait on the iPhone.
        more.add_child(new St.Label({text: _('Show all (%d)').replace('%d', phone.total)}));
        menu.addMenuItem(more);
    }
}

// "Earlier": notifications that are no longer on the iPhone, kept in memory
// because Phone Settings asked for it. Nothing to do with them but read.
function earlierMenu(menu, earlier, host) {
    const _ = host._;
    const sub = new PopupMenu.PopupSubMenuMenuItem(_('Earlier'));
    // First, so it is in reach however long the list is.
    const clear = new StayOpenItem(() => host.model.clearHistory());
    clear.add_child(new St.Label({text: _('Clear history')}));
    sub.menu.addMenuItem(clear);
    for (const entry of earlier) {
        const item = plainItem('landline-earlier');
        item.add_child(appAvatar(host, entry.appId, entry.initials, 'small', 'landline-app-avatar'));
        const text = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        const excerpt = entry.body.replace(/\s+/g, ' ').trim();
        const line = new St.BoxLayout({style_class: 'landline-row-line'});
        line.add_child(new St.Label({
            text: [entry.appName, entry.title].filter(t => t).join(' · '),
            style_class: 'landline-app-name',
            x_expand: true,
        }));
        line.add_child(new St.Label({
            text: clock(entry.timestamp),
            style_class: 'landline-when',
            y_align: Clutter.ActorAlign.CENTER,
        }));
        text.add_child(line);
        if (excerpt)
            text.add_child(new St.Label({text: excerpt, style_class: 'landline-sub'}));
        item.add_child(text);
        item.accessible_name = [entry.appName, entry.title, excerpt].filter(t => t).join(', ');
        sub.menu.addMenuItem(item);
    }
    menu.addMenuItem(sub);
}

function restMenu(menu, card, host) {
    const _ = host._;

    // One line: the glyph on the left, the name of the iPhone in the middle
    // of the menu, the battery on the right. Not a word about the link.
    const top = plainItem('landline-top');
    const glyph = new St.Icon({
        gicon: host.icon('landline-symbolic'),
        style_class: `landline-top-glyph ${card.linked ? 'landline-top-glyph-on' : ''}`,
    });
    const name = new St.Label({text: card.name, style_class: 'landline-title'});
    name.clutter_text.ellipsize = Pango.EllipsizeMode.END;
    let battery = null;
    let gauge = null;
    if (card.battery !== null) {
        battery = new St.Label({
            text: card.batteryStale
                // Translators: battery level of the iPhone as it was last
                // known; the link that reports it is down.
                ? _('~%d %%').replace('%d', card.battery).replace('%%', '%')
                // Translators: battery level of the iPhone.
                : _('%d %%').replace('%d', card.battery).replace('%%', '%'),
            style_class: 'landline-battery-label',
        });
        gauge = new BatteryGauge(card.battery, card.batteryCharging);
        const tone = card.batteryStale ? 'landline-battery-stale' : card.batteryLow ? 'landline-battery-low' : '';
        if (tone) {
            battery.add_style_class_name(tone);
            gauge.add_style_class_name(tone);
        }
    }
    const row = new TopRow(glyph, name, battery, gauge);
    top.add_child(row);
    // Nothing about the link when all is well: the icon says it.
    top.accessible_name = [
        card.name,
        // Translators: read aloud after the name of the iPhone; %d is the battery level.
        card.battery !== null ? _('battery %d %%').replace('%d', card.battery).replace('%%', '%') : '',
        card.batteryCharging === true ? _('charging') : '',
        card.battery !== null && card.batteryStale ? staleness(card.batteryMinutes, _) : '',
    ].filter(t => t).join(', ');
    menu.addMenuItem(top);

    if (card.paused) {
        // Notifications do not arrive: said first, with why and what to do.
        const item = plainItem('landline-notice landline-paused');
        const text = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, x_expand: true});
        text.add_child(new St.Label({text: _('Notifications paused'), style_class: 'landline-paused-title'}));
        // Where the gesture is made, in the words the iPhone itself uses.
        const where = card.paused.onPhone
            // Translators: %s is "Settings", as the iPhone writes it.
            ? _('On the iPhone: %s › Bluetooth.').replace('%s', host.phoneLabel('settings')) : '';
        // What to do, short; why is left to the screen reader.
        const todo = [card.paused.remedy, where].filter(t => t).join(' ');
        text.add_child(wrapped(todo, 'landline-sub'));
        item.add_child(text);
        item.accessible_name = `${_('Notifications paused')}. ${[card.paused.text, todo].filter(t => t).join(' ')}`;
        menu.addMenuItem(item);
    }

    // What is not well, one short line each, and right under them the one
    // thing to do about it. Nothing at all when all is well.
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
    if (card.updateAction)
        menu.addAction(_('Update the Engine…'), () => host.actions.run('install-engine', card));

    // What one comes here to do, first and in full view: place a call,
    // write a message.
    if (card.callAction || card.composeAction) {
        const item = plainItem('landline-start');
        const buttons = [];
        if (card.callAction) {
            buttons.push(actionButton(host, _('Call…'), 'call-start-symbolic', 'landline-start-button',
                () => host.model.startDial()));
        }
        if (card.composeAction) {
            buttons.push(actionButton(host, _('New Message…'), 'mail-message-new-symbolic', 'landline-start-button',
                () => host.model.startCompose()));
        }
        // Side by side, each as wide as its words ask for: neither is cut.
        const row = new St.BoxLayout({style_class: 'landline-pair', x_expand: true});
        for (const button of buttons)
            row.add_child(button);
        item.add_child(row);
        menu.addMenuItem(item);
    }

    // Asked once ever, while no other app opens phone links (#50). Phone
    // Settings › Calls keeps the same choice.
    if (card.phoneLinksOffer) {
        const item = plainItem('landline-notice');
        const hint = new St.BoxLayout({
            style_class: 'landline-note-suggestion',
            orientation: Clutter.Orientation.VERTICAL,
            x_expand: true,
        });
        hint.add_child(wrapped(_('Call the phone numbers you click in a browser or another app through your iPhone?'),
            'landline-sub'));
        const words = new St.BoxLayout({style_class: 'landline-internet-actions'});
        words.add_child(wordButton(_('Use Landline'), () => {
            host.model.answerPhoneLinksOffer();
            host.actions.run('use-phone-links', card);
        }));
        words.add_child(wordButton(_('Not Now'), () => host.model.answerPhoneLinksOffer()));
        hint.add_child(words);
        item.add_child(hint);
        menu.addMenuItem(item);
    }

    if (card.unread > 0) {
        // The top bar only shows that something waits: how many is said here.
        const item = plainItem('landline-notice');
        item.add_child(new St.Label({
            text: card.unread === 1 ? _('1 unread message')
                // Translators: %d is a number of messages, 2 or more.
                : _('%d unread messages').replace('%d', card.unread),
            style_class: 'landline-new',
        }));
        menu.addMenuItem(item);
    }

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
        if (thread.callable) {
            // Opens the card to call this person; nothing is dialled yet.
            item.add_child(roundButton('call-start-symbolic',
                // Translators: read aloud on the call button of a conversation; %s is a name or a number.
                _('Call %s').replace('%s', thread.name),
                () => host.model.startDial({number: thread.callable, name: thread.name})));
        }
        item.accessible_name = `${thread.name}, ${thread.preview}`;
        menu.addMenuItem(item);
    }
    if (card.threads.length === 0 && card.linked) {
        const item = new PopupMenu.PopupMenuItem(_('No conversations yet'), {reactive: false, can_focus: false});
        menu.addMenuItem(item);
    }

    if (card.phone)
        phoneSection(menu, card.phone, host);
    if (card.earlier?.length > 0)
        earlierMenu(menu, card.earlier, host);

    menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

    const calls = new PopupMenu.PopupSubMenuMenuItem(_('Recent calls'));
    if (card.history.length === 0) {
        calls.menu.addMenuItem(new PopupMenu.PopupMenuItem(_('No recent calls'), {reactive: false, can_focus: false}));
    }
    for (const entry of card.history) {
        // A line with a number opens the card to call it back; nothing is
        // dialled by this click.
        const item = entry.number
            ? new StayOpenItem(() => host.model.startDial({number: entry.number, name: entry.contact}))
            : plainItem('landline-history');
        if (entry.number) {
            item.add_style_class_name('landline-history');
            // Translators: read aloud on a line of the recent calls; %s is a name or a number.
            item.accessible_name = _('Call %s').replace('%s', entry.name);
        }
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
        if (entry.number) {
            // What a click on the line does.
            item.add_child(new St.Icon({
                icon_name: 'call-start-symbolic',
                style_class: 'landline-result-icon',
                y_align: Clutter.ActorAlign.CENTER,
            }));
        }
        calls.menu.addMenuItem(item);
    }
    menu.addMenuItem(calls);

    if (card.internet)
        internetSection(menu, card.internet, host);
    if (card.phoneAudioHere !== null && card.phoneAudioHere !== undefined) {
        switchItem(menu, _('Play iPhone audio on this computer'),
            card.phoneAudioHere === true, state => host.model.setPhoneAudio(state));
        phoneAudioLines(menu, card.phoneAudio, host);
    }
    switchItem(menu, _('Mute iPhone on this computer'), card.muted === true,
        state => host.actions.run('mute', {muted: state}));

    // No "Forget This iPhone…" here: one slip of the hand away from the
    // other rows. It lives in Phone Settings.
    menu.addAction(_('Phone Settings'), () => host.actions.run('settings', card));

    return {};
}
