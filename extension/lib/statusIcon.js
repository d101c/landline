// SPDX-License-Identifier: GPL-2.0-or-later
// The icon of the top bar: the layers of iconArt.js, one actor each, stacked
// in a box as large as any other icon of the top bar. What moves is moved by
// one Clutter timeline that places these actors at each frame (position,
// angle, size, opacity): no image is drawn again while it runs, and nothing
// runs at all while the icon is still, hidden, or when the desktop has its
// animations turned off.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {DIM, GRID, MOTIONS, iconLayers, layerSvg} from './iconArt.js';

const INKS = ['amber', 'green', 'red'];

const hex = color => `#${[color.red, color.green, color.blue]
    .map(part => part.toString(16).padStart(2, '0')).join('')}`;

// The easings of CSS, as curves from (0, 0) to (1, 1) with two handles.
const CURVES = {
    'ease-out': [0, 0, 0.58, 1],
    'ease-in-out': [0.42, 0, 0.58, 1],
};

function ease(name, t) {
    const curve = CURVES[name];
    if (!curve || t <= 0 || t >= 1)
        return Math.min(1, Math.max(0, t));
    const [x1, y1, x2, y2] = curve;
    const at = (a, b, u) => 3 * a * u * (1 - u) ** 2 + 3 * b * u ** 2 * (1 - u) + u ** 3;
    // The point of the curve whose x is t, by halving.
    let low = 0;
    let high = 1;
    for (let i = 0; i < 14; i++) {
        const middle = (low + high) / 2;
        if (at(x1, x2, middle) < t)
            low = middle;
        else
            high = middle;
    }
    return at(y1, y2, (low + high) / 2);
}

// The value of a track at `phase` (0 to 1) of its loop.
function valueAt(frames, phase, easing) {
    for (let i = 1; i < frames.length; i++) {
        const [end, to] = frames[i];
        if (phase <= end) {
            const [start, from] = frames[i - 1];
            return end === start ? to : from + (to - from) * ease(easing, (phase - start) / (end - start));
        }
    }
    return frames[frames.length - 1][1];
}

export const StatusIcon = GObject.registerClass(
class LandlineStatusIcon extends St.Widget {
    _init() {
        super._init({
            style_class: 'landline-icon',
            layout_manager: new Clutter.BinLayout(),
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._style = 'phone';
        this._state = 'bare';
        this._dim = false;
        this._moving = false;
        this._palette = null;              // ink -> '#rrggbb', from the stylesheet
        this._layers = [];                 // [{key, layer, actor}], bottom first
        this._timeline = null;             // runs only while something moves

        this.connect('style-changed', () => this._onStyleChanged());
        this.connect('notify::mapped', () => this._syncMotion());
        St.Settings.get().connectObject('notify::enable-animations', () => this._syncMotion(), this);
        this.connect('destroy', () => this._stop());
    }

    // style: 'phone' or 'handset'; state: a name of iconArt.js; moving:
    // whether its layers play their loops; dim: drawn like an absent phone.
    setState({style, state, moving = false, dim = false}) {
        const redraw = style !== this._style || state !== this._state || dim !== this._dim;
        if (!redraw && moving === this._moving)
            return;
        this._style = style;
        this._state = state;
        this._dim = dim;
        this._moving = moving;
        if (redraw)
            this._draw();
        else
            this._syncMotion();
    }

    // The colors come from the stylesheet: the glyph has the color of the
    // text around it, the three inks are custom properties, so a pill of
    // another color can ask for other inks.
    _onStyleChanged() {
        const node = this.get_theme_node();
        const glyph = node.get_foreground_color();
        const palette = {glyph: hex(glyph)};
        for (const ink of INKS) {
            const [found, color] = node.lookup_color(`-landline-${ink}`, true);
            palette[ink] = hex(found ? color : glyph);
        }
        if (JSON.stringify(palette) === JSON.stringify(this._palette))
            return;
        this._palette = palette;
        this._draw();
    }

    _draw() {
        if (!this._palette)
            return;
        this._stop();
        const old = new Map(this._layers.map(entry => [entry.key, entry]));
        this._layers = iconLayers(this._style, this._state).map(layer => {
            const color = this._palette[layer.ink] ?? this._palette.glyph;
            const key = `${color} ${layer.art}`;
            const kept = old.get(key);
            old.delete(key);
            const actor = kept?.actor ?? new St.Icon({
                style_class: 'landline-icon-layer',
                gicon: Gio.BytesIcon.new(new GLib.Bytes(new TextEncoder().encode(layerSvg(layer, color)))),
            });
            return {key, layer, actor};
        });
        for (const entry of old.values())
            entry.actor.destroy();
        this._layers.forEach(({actor}, index) => {
            if (actor.get_parent() === this)
                this.set_child_at_index(actor, index);
            else
                this.insert_child_at_index(actor, index);
        });
        this._syncMotion();
    }

    // The opacity of a layer when it does not move.
    _alpha(layer) {
        return 255 * (layer.alpha ?? 1) * (this._dim ? DIM : 1);
    }

    get running() {
        return this._timeline !== null;
    }

    _syncMotion() {
        const wanted = this._moving && this.mapped && St.Settings.get().enable_animations &&
            this._layers.some(({layer}) => layer.motion);
        this._stop();
        if (wanted)
            this._start();
    }

    // Every layer back where it is drawn, nothing left running.
    _stop() {
        if (this._timeline) {
            this._timeline.stop();
            this._timeline.disconnectObject(this);
            this._timeline = null;
        }
        for (const {layer, actor} of this._layers) {
            const [px, py] = layer.pivot ?? [GRID / 2, GRID / 2];
            actor.set_pivot_point(px / GRID, py / GRID);
            actor.set({
                translation_x: 0, translation_y: 0, rotation_angle_z: 0, scale_x: 1, scale_y: 1,
                // A wave only exists while it moves.
                opacity: layer.still === false ? 0 : Math.round(this._alpha(layer)),
            });
        }
    }

    // The timeline is only a clock: it follows the frames of the screen this
    // actor is on, and stops with it. Each frame places the layers where
    // their loops are at that instant.
    _start() {
        this._startedAt = GLib.get_monotonic_time();
        this._timeline = new Clutter.Timeline({actor: this, duration: 1000, repeat_count: -1});
        this._timeline.connectObject('new-frame', () => this._pose(), this);
        this._pose();
        this._timeline.start();
    }

    _pose() {
        const elapsed = (GLib.get_monotonic_time() - this._startedAt) / 1000;
        for (const {layer, actor} of this._layers) {
            const motion = MOTIONS[layer.motion];
            if (!motion)
                continue;
            // Before its first loop a delayed layer waits where the loop starts.
            const phase = Math.max(0, elapsed - (layer.delay ?? 0)) % motion.duration / motion.duration;
            // Grid units to pixels of the stage.
            const unit = actor.width / GRID;
            for (const [track, frames] of Object.entries(motion.tracks)) {
                const value = valueAt(frames, phase, motion.ease);
                if (track === 'x')
                    actor.translation_x = value * unit;
                else if (track === 'y')
                    actor.translation_y = value * unit;
                else if (track === 'rotation')
                    actor.rotation_angle_z = value;
                else if (track === 'scale')
                    actor.set_scale(value, value);
                else if (track === 'opacity')
                    actor.opacity = Math.round(value * this._alpha(layer));
            }
        }
    }
});
