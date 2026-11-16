// SPDX-License-Identifier: GPL-2.0-or-later
// How names, numbers and durations are written.

// Two letters for a name; nothing for a number, which gets a pictogram.
export function initials(name) {
    const words = String(name ?? '').trim().split(/\s+/).filter(w => /\p{L}/u.test(w));
    if (words.length === 0)
        return '';
    const first = [...words[0]][0];
    const last = words.length > 1 ? [...words[words.length - 1]][0] : '';
    return (first + last).toUpperCase();
}

export function firstName(name) {
    const text = String(name ?? '').trim();
    if (!/\p{L}/u.test(text))
        return text;
    return text.split(/\s+/)[0];
}

export function formatDuration(ms) {
    const total = Math.max(0, Math.floor(ms / 1000));
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const two = n => String(n).padStart(2, '0');
    return h > 0 ? `${h}:${two(m)}:${two(s)}` : `${two(m)}:${two(s)}`;
}

export function digits(number) {
    return String(number ?? '').replace(/[^0-9]/g, '');
}

function grouped(text, sizes) {
    const out = [];
    let at = 0;
    for (const size of sizes) {
        if (at >= text.length)
            break;
        out.push(text.slice(at, at + size));
        at += size;
    }
    if (at < text.length)
        out.push(text.slice(at));
    return out.join(' ');
}

// A phone number as people write it: +41 79 000 00 01. Anything that is not
// a full phone number (short code, name, e-mail address) comes back as it is.
export function formatNumber(text) {
    const raw = String(text ?? '').trim();
    if (!/^\+?[\d\s().\-/]{7,}$/.test(raw))
        return raw;
    const d = digits(raw);
    if (raw.startsWith('+')) {
        if (d.startsWith('41') && d.length === 11)
            return `+41 ${grouped(d.slice(2), [2, 3, 2, 2])}`;
        if (d.startsWith('33') && d.length === 11)
            return `+33 ${grouped(d.slice(2), [1, 2, 2, 2, 2])}`;
        if (d.startsWith('1') && d.length === 11)
            return `+1 ${grouped(d.slice(1), [3, 3, 4])}`;
        if (d.length < 8)
            return raw;
        const rest = d.slice(2);
        const sizes = Array(Math.floor(rest.length / 3)).fill(3);
        if (rest.length % 3 === 1 && sizes.length > 0)
            sizes[sizes.length - 1] = 4;
        return `+${d.slice(0, 2)} ${grouped(rest, sizes)}`;
    }
    if (d.length === 10 && d.startsWith('0'))
        return grouped(d, [3, 3, 2, 2]);
    return raw;
}

// How a correspondent is shown, whatever the engine knows about them: a
// contact name, a bare number, a short code or an iMessage address.
export function sender(name, address, thread) {
    let label = String(name ?? '').trim() || String(address ?? '').trim() ||
        String(thread ?? '').replace(/^(tel|email):/, '');
    const number = /^\+?[\d\s().\-/]{3,}$/.test(label);
    if (number)
        label = formatNumber(label);
    const email = !number && /^[^\s@]+@[^\s@]+$/.test(label);
    const local = email ? label.split('@')[0] : '';
    return {
        name: label,
        short: email ? local : number ? label : firstName(label),
        initials: number ? '' : initials(email ? local.replace(/[._-]+/g, ' ') : label),
        shortCode: /^\d{3,6}$/.test(label),
    };
}
