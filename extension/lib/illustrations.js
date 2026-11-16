// SPDX-License-Identifier: GPL-2.0-or-later
// The drawings of the iPhone screens shown by the setup tour. Each one is an
// SVG template in illustrations/, with a few {{names}} filled in here: the
// name of this computer as the iPhone lists it, the pairing number, which
// switches are already on, and {{L_names}} for what the iPhone writes, in the
// language of the iPhone (lib/phoneLabels.js). The element to tap is ringed
// in amber.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {LOG_PREFIX} from './names.js';
import {PHONE_LABELS} from './phoneLabels.js';

Gio._promisify(Gio.File.prototype, 'load_contents_async');

const ON = '#34c759';
const OFF = '#48484a';

function escape(text) {
    return String(text ?? '')
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// The drawings are small: a long name is cut like the iPhone would cut it.
function fit(text, max) {
    const chars = [...String(text ?? '').replace(/\s+/g, ' ').trim()];
    return chars.length > max ? `${chars.slice(0, max - 1).join('')}…` : chars.join('');
}

const TEMPLATES = ['bluetooth', 'request', 'info', 'lock'];

export class Illustrations {
    // dir: the Gio.File of illustrations/. onLoaded() is called when the
    // templates have been read, so that a tour already on screen is drawn again.
    constructor(dir, language = 'en', onLoaded = null) {
        this._templates = new Map();
        this._cancellable = new Gio.Cancellable();
        this._onLoaded = onLoaded;
        this.setLanguage(language);
        this._load(dir).catch(e => {
            if (!this._cancellable.is_cancelled())
                console.warn(`${LOG_PREFIX}: illustrations could not be read: ${e.message}`);
        });
    }

    // The language of the iPhone: 'en', 'fr'…
    setLanguage(language) {
        this.language = PHONE_LABELS[language] ? language : 'en';
        this.labels = PHONE_LABELS[this.language];
    }

    async _load(dir) {
        const decoder = new TextDecoder();
        await Promise.all(TEMPLATES.map(async name => {
            const [bytes] = await dir.get_child(`${name}.svg`).load_contents_async(this._cancellable);
            this._templates.set(name, decoder.decode(bytes));
        }));
        this._onLoaded?.();
    }

    // Empty until the template has been read.
    _template(name) {
        return this._templates.get(name) ?? '<svg xmlns="http://www.w3.org/2000/svg"/>';
    }

    destroy() {
        this._cancellable.cancel();
        this._onLoaded = null;
    }

    _icon(name, values) {
        const svg = this._template(name).replace(/\{\{(\w+)\}\}/g, (all, key) =>
            escape(key.startsWith('L_') ? this.labels[key.slice(2)] : values[key] ?? ''));
        return Gio.BytesIcon.new(new GLib.Bytes(new TextEncoder().encode(svg)));
    }

    // Settings › Bluetooth. host: this computer in the list, or null while
    // the iPhone does not know it yet. state: 'connected', 'notConnected' or
    // nothing. ring: the ⓘ next to it is the thing to tap.
    bluetooth({host = null, state = '', ring = false} = {}) {
        return this._icon('bluetooth', {
            host: fit(host ?? '', 13),
            state: this.labels[state] ?? '',
            hostOpacity: host ? '1' : '0',
            infoRing: ring ? '1' : '0',
            myHeight: host ? '49' : '24',
            otherDy: host ? '0' : '-25',
        });
    }

    // The "Bluetooth Pairing Request" alert, Pair ringed.
    request({host, code}) {
        const name = fit(host || 'computer', 12);
        const [l1, l2, l3] = this.labels.request.map(line => line.replace('%s', name));
        return this._icon('request', {l1, l2, l3, code: fit(code, 8)});
    }

    // The page behind the ⓘ, in the order of the real one (iOS 26): Show
    // Message Notifications, Sync Contacts, the Groups, Device Type, Share
    // System Notifications, Forget This Device. switches: {notifications,
    // messages, contacts}, those still off are ringed; forget: ring "Forget
    // This Device" instead; bare: what the iPhone shows when this computer
    // offers neither messages nor contacts (Device Type and Share System
    // Notifications only).
    info({host, switches = {}, forget = false, bare = false}) {
        const values = {
            host: fit(host || 'This computer', 22),
            rf: forget ? '1' : '0',
            sharing: bare ? '0' : '1',
            lowerDy: bare ? '-98' : '0',
        };
        for (const [id, letter] of [['messages', 'm'], ['contacts', 'c'], ['notifications', 'n']]) {
            const on = switches[id] === true;
            values[`t${letter}`] = on ? ON : OFF;
            values[`k${letter}`] = on ? '193.5' : '184.5';
            values[`r${letter}`] = on || forget || (bare && letter !== 'n') ? '0' : '1';
        }
        return this._icon('info', values);
    }

    // The lock screen with a message on it.
    lock({title, text}) {
        return this._icon('lock', {title: fit(title, 18), text: fit(text, 26)});
    }
}
