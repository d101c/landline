// SPDX-License-Identifier: GPL-2.0-or-later
// Handing a notification over to the official app of this computer (#49).
//
// Bluetooth carries the text of a notification, never its voice message or
// its photo. The official desktop app of that service, when it is installed,
// is a linked device of the account itself and holds them: the card of the
// notification offers to open it, on the right conversation when that is
// sure (lib/replyLinks.js), and says what kind of media is waiting there.
// Where the service has an official web client, that is the way instead.
// Only official apps and web clients, never a third-party client or
// wrapper of the service (an account can be banned for those): a link
// handler counts only when it is one of the official apps listed here.
// WhatsApp has no official app for Linux: WhatsApp Web, in the browser.
//
// No Shell or GLib import: what is installed is asked through a backend
// (extension.js gives the Shell's, the tests a fake one).

import {replyLink} from './replyLinks.js';

// One entry per service. desktop: the desktop file ids of its official app
// (system package, Flatpak, Snap). scheme: the link scheme that app opens.
// web: its official web client. get: where its official desktop app is
// found, for a suggestion when it is missing.
export const OFFICIAL_APPS = {
    whatsapp: {
        name: 'WhatsApp',
        bundles: ['net.whatsapp.WhatsApp', 'net.whatsapp.WhatsAppSMB'],
        // There is no official WhatsApp app for Linux: its web client is
        // the way, never a third-party wrapper (#54).
        desktop: [],
        web: {name: 'WhatsApp Web', home: 'https://web.whatsapp.com/', conversation: true},
        get: null,
    },
    signal: {
        name: 'Signal',
        bundles: ['org.whispersystems.signal'],
        desktop: ['signal-desktop.desktop', 'org.signal.Signal.desktop', 'signal-desktop_signal-desktop.desktop',
            'signal.desktop'],
        scheme: 'sgnl',
        web: null,
        get: {name: 'Signal Desktop', url: 'https://signal.org/download/'},
    },
    telegram: {
        name: 'Telegram',
        bundles: ['ph.telegra.Telegraph'],
        desktop: ['org.telegram.desktop.desktop', 'telegramdesktop.desktop', 'telegram-desktop.desktop',
            'telegram-desktop_telegram-desktop.desktop'],
        scheme: 'tg',
        web: {name: 'Telegram Web', home: 'https://web.telegram.org/'},
        get: {name: 'Telegram Desktop', url: 'https://desktop.telegram.org/'},
    },
    teams: {
        name: 'Teams',
        bundles: ['com.microsoft.skype.teams'],
        desktop: ['com.microsoft.Teams.desktop', 'teams.desktop', 'teams-insiders.desktop'],
        scheme: 'msteams',
        web: {name: null, home: 'https://teams.microsoft.com/'},
        // Microsoft no longer makes a Teams app for Linux: the web client is the way.
        get: null,
    },
    discord: {
        name: 'Discord',
        bundles: ['com.hammerandchisel.discord'],
        desktop: ['com.discordapp.Discord.desktop', 'discord.desktop', 'discord_discord.desktop'],
        scheme: 'discord',
        web: {name: null, home: 'https://discord.com/app'},
        get: {name: 'Discord', url: 'https://discord.com/download'},
    },
    slack: {
        name: 'Slack',
        bundles: ['com.tinyspeck.chatlyio'],
        desktop: ['com.slack.Slack.desktop', 'slack.desktop', 'slack_slack.desktop'],
        scheme: 'slack',
        web: {name: null, home: 'https://app.slack.com/client'},
        get: {name: 'Slack', url: 'https://slack.com/downloads/linux'},
    },
    messenger: {
        name: 'Messenger',
        bundles: ['com.facebook.Messenger'],
        // Meta no longer makes a desktop app: messenger.com is the way.
        desktop: [],
        web: {name: 'Messenger', home: 'https://www.messenger.com/'},
        get: null,
    },
};

// The key of OFFICIAL_APPS for an iOS bundle identifier, or null.
export function serviceOf(bundleId) {
    for (const [key, entry] of Object.entries(OFFICIAL_APPS)) {
        if (entry.bundles.includes(bundleId))
            return key;
    }
    return null;
}

// Every link scheme worth asking the system about.
export function schemes() {
    return [...new Set(Object.values(OFFICIAL_APPS).map(e => e.scheme).filter(s => s)), 'https'];
}

// Every desktop file id worth looking up.
export function desktopIds() {
    return Object.values(OFFICIAL_APPS).flatMap(e => e.desktop);
}

// How a notification of `bundleId` is handed over, from what is installed:
// found = {apps: {desktopId: name}, handlers: {scheme: desktopId | null},
// web: whether something opens https links}. null for a service that is
// not listed. Otherwise:
//   {service, key, via, app, links, web, home, webName, missing, get}
// app: the desktop file id of the official app to open;
// links: that app opens the links of its service (a conversation can be
// opened in it); web: the web client is the way; missing: nothing on this
// computer opens it; get: where to find the official app, when it is not
// installed (null when it is, or when there is none for Linux).
export function appRoute(bundleId, found) {
    const key = serviceOf(bundleId);
    if (!key)
        return null;
    const entry = OFFICIAL_APPS[key];
    const apps = found?.apps ?? {};
    const handler = entry.scheme ? found?.handlers?.[entry.scheme] ?? null : null;
    // The handler of the scheme first, if it is one of ours; else any
    // installed one. A handler that is not listed is never used.
    const pick = list => list.find(id => id === handler) ?? list.find(id => Object.hasOwn(apps, id)) ?? null;
    const base = {service: entry.name, key, app: null, links: false, web: false, home: null, webName: null,
        missing: false, get: null};
    const official = pick(entry.desktop);
    if (official)
        return {...base, via: entry.name, app: official, links: handler === official};
    if (entry.web && found?.web !== false) {
        return {...base, via: entry.web.name ?? entry.name, web: true, home: entry.web.home, webName: entry.web.name,
            links: entry.web.conversation === true, get: entry.get};
    }
    return {...base, via: null, missing: true, get: entry.get};
}

// The link that opens the conversation with `number` (digits, country code
// first) where `route` leads, or null: only where that link is reliable.
export function conversationLink(bundleId, route, number) {
    if (!route?.links || !/^\d{8,15}$/.test(String(number ?? '')))
        return null;
    return route.app || route.web ? replyLink(bundleId, number)?.uri ?? null : null;
}

// --- what a notification stands for -----------------------------------------

// The texts the apps put in a notification instead of a media, in English
// and in French: "🎤 Voice message", "📷 Photo", "Vidéo", "Sticker".
const MEDIA_EMOJI = [
    ['voice', /^(?:🎤|🎙)/u],
    ['photo', /^(?:📷|📸|🖼)/u],
    ['video', /^(?:🎥|📹|📼)/u],
    ['file', /^(?:📄|📎|📁)/u],
];
const MEDIA_WORDS = [
    ['voice', 'voice message|voice note|audio message|audio|message vocal|message audio|note vocale|mémo vocal'],
    ['photo', 'photo|image|picture'],
    ['video', 'video message|video|message vidéo|vidéo'],
    ['file', 'document|file|attachment|fichier|pièce jointe'],
    ['sticker', 'sticker|autocollant|gif'],
].map(([kind, words]) => [kind,
    // The word alone, or with a duration or a size: "Voice message (0:12)".
    new RegExp(`^(?:${words})(?:\\s*[(·•,-]?\\s*\\d[\\d:.,]*\\s*[a-zA-Z]{0,3}\\s*\\)?)?\\.?$`, 'iu')]);
// Messenger and others say it in a sentence.
const MEDIA_SENTENCES = [
    ['voice', /\b(?:sent (?:you )?a voice message|a envoyé un message vocal)\b/iu],
    ['photo', /\b(?:sent (?:you )?a photo|a envoyé une photo)\b/iu],
    ['video', /\b(?:sent (?:you )?a video|a envoyé une vidéo)\b/iu],
    ['file', /\b(?:sent (?:you )?an attachment|a envoyé une pièce jointe)\b/iu],
];

// 'voice', 'photo', 'video', 'file', 'sticker' or null: an emoji first,
// the word alone (or with a duration), or a sentence that says it; in a
// group, after "Name: ". A message that only starts with "Photo" is text.
export function mediaKind(body) {
    const text = String(body ?? '').trim();
    if (!text)
        return null;
    const named = /^[^:\n]{1,40}:\s+(.+)$/su.exec(text);
    for (const candidate of named ? [text, named[1].trim()] : [text]) {
        for (const [kind, pattern] of [...MEDIA_EMOJI, ...MEDIA_WORDS, ...MEDIA_SENTENCES]) {
            if (pattern.test(candidate))
                return kind;
        }
    }
    return null;
}

// --- what is installed ---------------------------------------------------------

// Asks the backend once, keeps the answer, and asks again when told that
// the installed apps changed. backend:
//   lookupApp(id) -> its name, or null when not installed (cheap: the
//     Shell keeps the list in memory);
//   defaultFor(scheme) -> Promise of the desktop id that opens it, or null;
//   isRunning(id) -> whether that app has a window open now.
// onChange() is called after each look that changed something.
export class AppFinder {
    constructor(backend, onChange = () => {}) {
        this._backend = backend;
        this._onChange = onChange;
        this._found = null;
        this._asked = 0;
        this._stopped = false;
    }

    // Looks again; concurrent looks are fine, the latest one wins.
    async refresh() {
        const asked = ++this._asked;
        const apps = {};
        for (const id of desktopIds()) {
            const name = this._backend.lookupApp(id);
            if (name)
                apps[id] = name;
        }
        const handlers = {};
        for (const scheme of schemes()) {
            try {
                handlers[scheme] = await this._backend.defaultFor(scheme) ?? null;
            } catch (e) {
                handlers[scheme] = null;
            }
            if (this._stopped || asked !== this._asked)
                return;
        }
        const found = {apps, handlers, web: handlers.https !== null};
        const changed = JSON.stringify(found) !== JSON.stringify(this._found);
        this._found = found;
        if (changed)
            this._onChange();
    }

    // null until the first look is over.
    route(bundleId) {
        return this._found ? appRoute(bundleId, this._found) : null;
    }

    // The official app of that service is open on this computer right now
    // (and notifies by itself). Never true for a web client: whether a tab
    // of the browser shows it cannot be known.
    running(bundleId) {
        const route = this.route(bundleId);
        return !!route?.app && this._backend.isRunning(route.app) === true;
    }

    stop() {
        this._stopped = true;
        this._onChange = () => {};
    }
}
