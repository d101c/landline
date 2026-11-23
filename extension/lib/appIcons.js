// SPDX-License-Identifier: GPL-2.0-or-later
// The real icons of the iPhone apps, for their notifications. Each
// notification names its app by its identifier (net.whatsapp.WhatsApp); the
// public lookup of the App Store gives the icon of that app. It is asked once
// per app, without blocking the Shell, and kept on disk in
// ~/.cache/landline/app-icons/<identifier>.png, 128 pixels wide. An app the
// App Store does not know is remembered as such (<identifier>.none) and
// asked again a day later at the earliest.
//
// Phone Settings can turn this off ("show-app-icons"): then nothing is asked
// and nothing of the cache is shown. Either way, an app installed on this
// computer lends its own icon to the same app of the iPhone, from the few
// that are known below. No brand artwork ships with the extension.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Soup from 'gi://Soup?version=3.0';

// The size asked of Apple's image server; drawn smaller, rounded, by St.
export const ICON_SIZE = 128;
// A failure is not tried again before this.
export const RETRY_MS = 24 * 3600 * 1000;
export const LOOKUP_URL = 'https://itunes.apple.com/lookup';
// Far more than an icon of 128 pixels needs.
export const MAX_BYTES = 512 * 1024;
// The answer of the lookup for one app is a few tens of kilobytes.
export const MAX_LOOKUP_BYTES = 1024 * 1024;
const CHUNK = 64 * 1024;
const TIMEOUT_S = 20;
const USER_AGENT = 'landline-extension';
const PNG_MAGIC = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

// iPhone app -> the desktop files of the same app on this computer (the
// usual package, Flatpak and Snap names), the first one installed lends its
// icon.
export const DESKTOP_APPS = {
    'net.whatsapp.WhatsApp': ['whatsapp-desktop', 'com.rtosta.zapzap', 'io.github.mimbrero.WhatsAppDesktop',
        'com.github.eneshecan.WhatsAppForLinux', 'whatsapp-for-linux', 'whatsapp'],
    'org.whispersystems.signal': ['signal-desktop', 'org.signal.Signal', 'signal-desktop_signal-desktop'],
    'ph.telegra.Telegraph': ['org.telegram.desktop', 'telegram-desktop', 'telegramdesktop',
        'telegram-desktop_telegram-desktop'],
    'com.microsoft.skype.teams': ['com.microsoft.Teams', 'teams', 'teams-for-linux',
        'com.github.IsmaelMartinez.teams_for_linux', 'teams-for-linux_teams-for-linux'],
    'com.hammerandchisel.discord': ['com.discordapp.Discord', 'discord', 'discord_discord'],
    'com.tinyspeck.chatlyio': ['com.slack.Slack', 'slack', 'slack_slack'],
    'com.facebook.Messenger': ['com.sindresorhus.Caprine', 'caprine'],
    'com.apple.mobilemail': ['org.gnome.Evolution', 'org.mozilla.Thunderbird', 'thunderbird',
        'thunderbird_thunderbird', 'org.gnome.Geary'],
    'com.apple.mobilecal': ['org.gnome.Calendar'],
};

export function desktopCandidates(appId) {
    return Object.hasOwn(DESKTOP_APPS, appId) ? DESKTOP_APPS[appId] : [];
}

// The country of the App Store: the region of the locale (fr_CH -> ch),
// from the first list that has one; "us" when none does.
export function countryFromLocale(...lists) {
    for (const list of lists) {
        for (const name of list ?? []) {
            const match = /^[a-z]{2,3}_([A-Za-z]{2})(?:[.@]|$)/.exec(name);
            if (match)
                return match[1].toLowerCase();
        }
    }
    return 'us';
}

// The name an app's files take in the cache: the identifier itself, when it
// is one (letters, digits, dots, dashes); null otherwise.
export function iconFileName(appId) {
    if (typeof appId !== 'string' || appId === 'unknown' || appId.length > 200 ||
        !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(appId) || appId.includes('..'))
        return null;
    return appId;
}

// The address of the icon at `size` pixels, in PNG, from an answer of the
// lookup. Apple's image server draws the size and format the end of the
// path names (…/512x512bb.jpg).
export function artworkUrl(result, size = ICON_SIZE) {
    const url = result?.artworkUrl512 ?? result?.artworkUrl100 ?? result?.artworkUrl60;
    if (typeof url !== 'string')
        return null;
    return url.replace(/\/\d+x\d+bb\.(?:jpg|jpeg|png|webp)$/, `/${size}x${size}bb.png`);
}

// Only Apple's image server, over HTTPS.
export function fromAppStore(url) {
    const uri = GLib.Uri.parse(url, GLib.UriFlags.NONE);
    return uri.get_scheme() === 'https' && /(^|\.)mzstatic\.com$/.test(uri.get_host() ?? '');
}

// An answer that is not worth asking again soon (the app is unknown, the
// image is not one): remembered on disk. Anything else (no network, a
// server error) is remembered in memory only.
class Definitive extends Error {}

// obj.method(...args, callback) as a promise, finished by obj[finish].
function call(obj, method, finish, ...args) {
    return new Promise((resolve, reject) => {
        obj[method](...args, (o, res) => {
            try {
                resolve(o[finish](res));
            } catch (e) {
                reject(e);
            }
        });
    });
}

const cancelled = e => e instanceof GLib.Error && e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED);

export class AppIcons {
    // dir: where the icons are kept. country: of the App Store.
    // desktopIcon(desktopId): the icon of that installed app, or null.
    // onChange(): an icon came, or the setting changed.
    // lookupUrl, artworkAllowed, now: for the tests.
    constructor({dir, country = 'us', enabled = true, desktopIcon = null, onChange = null,
        lookupUrl = LOOKUP_URL, artworkAllowed = fromAppStore, now = () => Date.now()}) {
        this._dir = Gio.File.new_for_path(dir);
        this._country = country;
        this._enabled = enabled === true;
        this._desktopIcon = desktopIcon;
        this.onChange = onChange;
        this._lookupUrl = lookupUrl;
        this._artworkAllowed = artworkAllowed;
        this._now = now;
        this._cancellable = new Gio.Cancellable();
        this._session = null;
        // identifier -> {state: 'busy' | 'ready' | 'failed', path, retryAt}
        this._entries = new Map();
        this._desktop = new Map();
    }

    get enabled() {
        return this._enabled;
    }

    // Turned off: what is on its way is dropped, nothing is asked any more
    // and the icons of the App Store are no longer shown.
    setEnabled(enabled) {
        enabled = enabled === true;
        if (enabled === this._enabled)
            return;
        this._enabled = enabled;
        this._stop();
        this.onChange?.();
    }

    // What to show for this app right now: {file} (the icon of the App
    // Store, a PNG to round), {gicon} (the one of the same app on this
    // computer) or null (the initials). An icon not known yet is looked for,
    // once; onChange() says when it comes.
    icon(appId) {
        const file = this._stored(appId);
        if (file)
            return {file};
        const gicon = this._installed(appId);
        return gicon ? {gicon} : null;
    }

    destroy() {
        this.onChange = null;
        this._stop();
        this._cancellable = null;
        this._desktop.clear();
    }

    // The apps of this computer changed (Gio.AppInfoMonitor): an app found
    // missing may be there now, one found may be gone (#55).
    appsChanged() {
        if (this._desktop.size === 0)
            return;
        this._desktop.clear();
        this.onChange?.();
    }

    _stop() {
        this._cancellable?.cancel();
        this._cancellable = new Gio.Cancellable();
        this._session?.abort();
        this._session = null;
        this._entries.clear();
    }

    _installed(appId) {
        if (!this._desktopIcon)
            return null;
        if (!this._desktop.has(appId)) {
            let gicon = null;
            for (const id of desktopCandidates(appId)) {
                gicon = this._desktopIcon(id);
                if (gicon)
                    break;
            }
            this._desktop.set(appId, gicon);
        }
        return this._desktop.get(appId);
    }

    _stored(appId) {
        if (!this._enabled || !this._cancellable)
            return null;
        const name = iconFileName(appId);
        if (!name)
            return null;
        const entry = this._entries.get(name);
        if (entry?.state === 'ready')
            return entry.path;
        if (!entry || (entry.state === 'failed' && this._now() >= entry.retryAt))
            this._resolve(name);
        return null;
    }

    // One search per app at a time: the entry says it is on its way.
    _resolve(name) {
        const entry = {state: 'busy', path: null, retryAt: 0};
        this._entries.set(name, entry);
        const cancellable = this._cancellable;
        this._find(name, cancellable).then(found => {
            if (cancellable.is_cancelled() || this._entries.get(name) !== entry)
                return;
            if (found.path) {
                entry.state = 'ready';
                entry.path = found.path;
                this.onChange?.();
            } else {
                entry.state = 'failed';
                entry.retryAt = found.retryAt;
            }
        }).catch(e => {
            if (cancelled(e) || cancellable.is_cancelled() || this._entries.get(name) !== entry)
                return;
            entry.state = 'failed';
            entry.retryAt = this._now() + RETRY_MS;
            console.warn(`landline: no icon for ${name}: ${e.message}`);
        });
    }

    async _find(name, cancellable) {
        const png = this._dir.get_child(`${name}.png`);
        const known = await this._info(png, cancellable);
        if (known && known.get_size() > 0)
            return {path: png.get_path()};
        const marker = this._dir.get_child(`${name}.none`);
        const marked = await this._info(marker, cancellable);
        if (marked) {
            const at = marked.get_modification_date_time()?.to_unix() * 1000;
            if (at && this._now() < at + RETRY_MS)
                return {retryAt: at + RETRY_MS};
        }
        let bytes;
        try {
            bytes = await this._download(name, cancellable);
        } catch (e) {
            if (!(e instanceof Definitive))
                throw e;
            // Its date is what counts; empty contents would not be written.
            await this._write(marker, new GLib.Bytes(new TextEncoder().encode('\n')), cancellable);
            return {retryAt: this._now() + RETRY_MS};
        }
        await this._write(png, bytes, cancellable);
        if (marked)
            await call(marker, 'delete_async', 'delete_finish', GLib.PRIORITY_DEFAULT, cancellable).catch(() => {});
        return {path: png.get_path()};
    }

    async _info(file, cancellable) {
        try {
            return await call(file, 'query_info_async', 'query_info_finish', 'standard::size,time::modified',
                Gio.FileQueryInfoFlags.NONE, GLib.PRIORITY_DEFAULT, cancellable);
        } catch (e) {
            if (cancelled(e))
                throw e;
            return null;
        }
    }

    // {bytes, message}: the body of `url`, `maxBytes` at most. A redirection is not followed:
    // only the address that was checked is ever read (#55).
    async _get(url, cancellable, maxBytes) {
        if (!this._session)
            this._session = new Soup.Session({user_agent: USER_AGENT, timeout: TIMEOUT_S});
        const message = Soup.Message.new('GET', url);
        if (!message)
            throw new Definitive(`not an address: ${url}`);
        message.add_flags(Soup.MessageFlags.NO_REDIRECT);
        const asked = message.get_uri();
        const stream = await call(this._session, 'send_async', 'send_finish',
            message, GLib.PRIORITY_DEFAULT, cancellable);
        try {
            const status = message.get_status();
            if (status !== Soup.Status.OK) {
                // A server that is busy or failing may answer tomorrow.
                if (status >= 500 || status === 429)
                    throw new Error(`HTTP ${status}`);
                throw new Definitive(`HTTP ${status}`);
            }
            // Never anything but the server that was checked.
            const final = message.get_uri();
            if (final.get_scheme() !== asked.get_scheme() || final.get_host() !== asked.get_host() ||
                final.get_port() !== asked.get_port())
                throw new Definitive('answered from another address');
            // Too big by its own say: not read at all.
            const headers = message.get_response_headers();
            if (headers.get_encoding() === Soup.Encoding.CONTENT_LENGTH && headers.get_content_length() > maxBytes)
                throw new Definitive(`too big (${headers.get_content_length()} bytes)`);
            return {bytes: await this._read(stream, maxBytes, cancellable), message};
        } finally {
            stream.close_async(GLib.PRIORITY_DEFAULT, null, (o, res) => {
                try {
                    o.close_finish(res);
                } catch (e) {}
            });
        }
    }

    // Read in chunks; more than `maxBytes` and the rest is not read.
    async _read(stream, maxBytes, cancellable) {
        const chunks = [];
        let size = 0;
        for (;;) {
            const chunk = await call(stream, 'read_bytes_async', 'read_bytes_finish',
                Math.min(CHUNK, maxBytes + 1 - size), GLib.PRIORITY_DEFAULT, cancellable);
            const data = chunk.toArray();
            if (data.length === 0)
                break;
            size += data.length;
            if (size > maxBytes)
                throw new Definitive(`too big (more than ${maxBytes} bytes)`);
            chunks.push(data);
        }
        const all = new Uint8Array(size);
        let at = 0;
        for (const data of chunks) {
            all.set(data, at);
            at += data.length;
        }
        return new GLib.Bytes(all);
    }

    async _download(appId, cancellable) {
        const query = `bundleId=${encodeURIComponent(appId)}&country=${encodeURIComponent(this._country)}`;
        const {bytes: answer} = await this._get(`${this._lookupUrl}?${query}`, cancellable, MAX_LOOKUP_BYTES);
        let result;
        try {
            const json = JSON.parse(new TextDecoder().decode(answer.toArray()));
            result = json?.results?.[0];
        } catch (e) {
            throw new Definitive('the answer is not JSON');
        }
        if (!result || String(result.bundleId ?? '').toLowerCase() !== appId.toLowerCase())
            throw new Definitive('unknown to the App Store');
        const url = artworkUrl(result);
        if (!url || !this._artworkAllowed(url))
            throw new Definitive('no icon');
        const {bytes, message} = await this._get(url, cancellable, MAX_BYTES);
        const [type] = message.get_response_headers().get_content_type();
        const data = bytes.toArray();
        if (type !== 'image/png' || data.length === 0 ||
            PNG_MAGIC.some((b, i) => data[i] !== b))
            throw new Definitive('not a PNG image');
        return bytes;
    }

    async _write(file, bytes, cancellable) {
        await this._makeDir(this._dir, cancellable);
        await call(file, 'replace_contents_bytes_async', 'replace_contents_finish',
            bytes, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, cancellable);
    }

    async _makeDir(dir, cancellable) {
        try {
            await call(dir, 'make_directory_async', 'make_directory_finish', GLib.PRIORITY_DEFAULT, cancellable);
        } catch (e) {
            if (e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS))
                return;
            const parent = dir.get_parent();
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND) || !parent)
                throw e;
            await this._makeDir(parent, cancellable);
            await this._makeDir(dir, cancellable);
        }
    }
}
