// SPDX-License-Identifier: GPL-2.0-or-later
// Phone links (tel:, callto:) opened on this computer call through the
// iPhone (#50). No Shell or GLib import.
//
// The way a link reaches the Shell: a desktop file in the user's
// applications folder (lib/phoneLinks.js writes it, from Phone Settings)
// runs `gdbus call` on the session bus, on an object the extension exports
// on the Shell's own connection while it is enabled and the option is on.
// Disabled, the object is gone: the call fails at once with "no such
// object", and nothing else happens. Nothing runs as root, nothing stays in
// the background.

export const BUS_NAME = 'org.gnome.Shell';
export const OBJECT_PATH = '/io/github/d101c/Landline';
export const INTERFACE = 'io.github.d101c.Landline.PhoneLinks';
export const INTERFACE_XML = `<node>
  <interface name="${INTERFACE}">
    <method name="Call">
      <arg type="s" direction="in" name="uri"/>
    </method>
  </interface>
</node>`;

export const DESKTOP_ID = 'landline-phone-links.desktop';
export const SCHEMES = ['tel', 'callto'];

// The desktop file. %u is the link, given as one argument; gdbus takes a
// bare string for an "s" argument.
export function desktopEntry(name, comment) {
    return [
        '[Desktop Entry]',
        'Type=Application',
        `Name=${oneLine(name)}`,
        `Comment=${oneLine(comment)}`,
        `Exec=gdbus call --session --dest ${BUS_NAME} --object-path ${OBJECT_PATH} --method ${INTERFACE}.Call %u`,
        'Icon=call-start-symbolic',
        'NoDisplay=true',
        'Terminal=false',
        `MimeType=${SCHEMES.map(s => `x-scheme-handler/${s};`).join('')}`,
        // Written by Landline: Phone Settings › "Use Landline for phone links" removes it.
        'X-Landline-Phone-Links=true',
        '',
    ].join('\n');
}

function oneLine(text) {
    return String(text ?? '').replace(/[\r\n]+/g, ' ').trim();
}

// Longest link worth reading.
export const MAX_LINK = 256;

// The number a phone link calls, as the engine wants it: "+41790000001",
// "0790000001", or '' when the link holds no number that may be called.
// tel:+41-79-000-00-01, tel:+41 (0)79 000 00 01, tel:0790000001;phone-context=+41,
// callto:+41790000001, callto://+41790000001, %20 for spaces. Parameters
// after ";" (phone-context, ext, isub) are dropped: a local number is
// dialled as written, the iPhone knows its own country. A Skype name, a
// vanity number with letters, and anything with * or # (a service code
// from a web page must never reach the iPhone) are not numbers.
export function phoneLinkNumber(uri) {
    let text = String(uri ?? '').trim();
    if (!text || text.length > MAX_LINK)
        return '';
    const scheme = /^(tel|callto):(?:\/\/)?/i.exec(text);
    if (!scheme)
        return '';
    text = text.slice(scheme[0].length);
    try {
        text = decodeURIComponent(text);
    } catch (e) {
        return '';
    }
    text = text.split(/[;?]/)[0].trim();
    // "+41 (0)79…": the trunk zero in brackets is not dialled after a
    // country code.
    text = text.replace(/^(\+\d{1,3})[\s.-]*\(0\)/, '$1');
    if (!/^\+?[\d\s().\-/]+$/.test(text))
        return '';
    const digits = text.replace(/\D/g, '');
    if (digits.length < 3 || digits.length > 15)
        return '';
    return `${text.startsWith('+') ? '+' : ''}${digits}`;
}

// --- how often a link may open the card (#56) ---------------------------------

// The same link again this soon: a double click, the card is already there.
export const PHONE_LINK_GAP_MS = 1000;
// At most this many cards opened by links in PHONE_LINK_WINDOW_MS: any
// program of the session can open a link, none may keep the card coming.
export const PHONE_LINK_BURST = 5;
export const PHONE_LINK_WINDOW_MS = 60 * 1000;

// take(uri, shown) for each link that arrives, `shown` saying whether the
// card of a phone link is open right now:
//   'show'  open the card (another link replaces the one shown);
//   'same'  the link of the card shown, again within a second: nothing to do;
//   'hold'  too many cards in a short time: this one is not shown (the
//           caller says so in the journal).
export class PhoneLinkGate {
    constructor(now = () => Date.now()) {
        this._now = now;
        this._last = null;
        this._opened = [];
    }

    take(uri, shown) {
        const now = this._now();
        const text = String(uri ?? '').slice(0, MAX_LINK + 1);
        if (shown && this._last?.uri === text && now - this._last.at < PHONE_LINK_GAP_MS) {
            this._last.at = now;
            return 'same';
        }
        this._opened = this._opened.filter(at => now - at < PHONE_LINK_WINDOW_MS);
        if (this._opened.length >= PHONE_LINK_BURST)
            return 'hold';
        this._opened.push(now);
        this._last = {uri: text, at: now};
        return 'show';
    }
}
