// SPDX-License-Identifier: GPL-2.0-or-later
// "Reply in app": for a notification of another messaging app, a link that
// opens the official client of that app on this computer, on the right
// conversation. Only where the link is reliable: the app is built on phone
// numbers, the sender is one person, and that person is found for certain in
// the contacts of the iPhone. No Shell or GLib import.

// iOS bundle identifier -> how the conversation with a phone number opens.
// number: digits only, country code first, no "+".
const APPS = {
    'net.whatsapp.WhatsApp': {scheme: 'https', uri: number => `https://web.whatsapp.com/send?phone=${number}`},
    'net.whatsapp.WhatsAppSMB': {scheme: 'https', uri: number => `https://web.whatsapp.com/send?phone=${number}`},
    'ph.telegra.Telegraph': {scheme: 'tg', uri: number => `tg://resolve?phone=${number}`},
    'org.whispersystems.signal': {scheme: 'sgnl', uri: number => `sgnl://signal.me/#p/+${number}`},
};

export function replyApp(appId) {
    return Object.hasOwn(APPS, appId) ? APPS[appId] : null;
}

// {uri, scheme} or null.
export function replyLink(appId, number) {
    const app = replyApp(appId);
    if (!app || !/^\d{8,15}$/.test(String(number ?? '')))
        return null;
    return {uri: app.uri(number), scheme: app.scheme};
}

// A phone number written in full, with its country code: its digits.
function international(text) {
    const raw = String(text ?? '').replace(/^tel:/, '').trim();
    if (!/^\+[\d\s().\-/]{8,}$/.test(raw))
        return null;
    const digits = raw.replace(/\D/g, '');
    return digits.length >= 8 && digits.length <= 15 ? digits : null;
}

// Who a notification is from, as far as its texts tell: {number} when the
// title is itself a full phone number (a sender who is not a contact),
// {name} when it looks like one person, null when it cannot be one for
// sure: a group (the app then fills the subtitle, or writes "Name @ Group"),
// a "~nickname" the app shows for strangers, or nothing at all.
export function replySender(title, subtitle) {
    const name = String(title ?? '').replace(/\s+/g, ' ').trim();
    if (!name || String(subtitle ?? '').trim())
        return null;
    if (/^~/.test(name) || /\s@\s|[:,]\s|\s[&+]\s/.test(name))
        return null;
    const number = international(name);
    if (number)
        return {number};
    if (/^[+\d\s().\-/]+$/.test(name))
        return null;                       // a number without its country code
    return {name};
}

// The one phone number of the one contact called exactly `name`, or null:
// two contacts with that name, or a contact with two numbers, is not sure.
export function contactNumber(name, contacts) {
    const wanted = String(name ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
    if (!wanted)
        return null;
    const same = (Array.isArray(contacts) ? contacts : []).filter(contact =>
        String(contact?.name ?? '').replace(/\s+/g, ' ').trim().toLowerCase() === wanted);
    if (same.length !== 1)
        return null;
    const numbers = new Set();
    for (const address of Array.isArray(same[0].addresses) ? same[0].addresses : []) {
        if (!String(address).startsWith('tel:'))
            continue;
        const number = international(address);
        // A number without a country code cannot be trusted to a link.
        if (!number)
            return null;
        numbers.add(number);
    }
    return numbers.size === 1 ? [...numbers][0] : null;
}
