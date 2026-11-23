// SPDX-License-Identifier: GPL-2.0-or-later
// The drawings of the top bar icon, on a 16 × 16 grid: two styles, one
// drawing per state, each made of layers that may move. No Shell or GLib
// import: statusIcon.js turns the layers into actors.
//
// A layer is {art, ink, alpha, pivot, motion, delay, still}:
//   art     SVG markup, `{c}` standing for its color
//   ink     'glyph' (the color of the top bar text), 'amber', 'green', 'red'
//   alpha   its opacity when it does not move (1 when left out)
//   pivot   the point it turns or grows around, in grid units
//   motion  a name of MOTIONS, played in a loop while the icon moves
//   delay   ms before its first loop
//   still   false: only drawn while it moves (a wave)

export const ICON_STYLES = ['phone', 'handset'];
export const ICON_STATES = ['bare', 'linked', 'away', 'reconnecting', 'unread', 'ringing', 'call', 'missed', 'voicemail',
    'paused', 'battery', 'internet', 'internet_failed', 'attention'];
export const GRID = 16;
// An absent phone is the same drawing, dimmed.
export const DIM = 0.45;
// On its way back: between absent and there.
export const HALF = 0.7;

// Loops, as keyframes [position in the loop, value]. x and y are in grid
// units, rotation in degrees, scale and opacity are factors.
export const MOTIONS = {
    // The phone vibrates, then rests.
    buzz: {
        duration: 1500, ease: 'linear',
        tracks: {
            x: [[0, 0], [0.06, -0.7], [0.12, 0.7], [0.18, -0.7], [0.24, 0.7], [0.30, -0.7],
                [0.36, 0.7], [0.42, -0.7], [0.48, 0.7], [0.60, 0], [1, 0]],
        },
    },
    // The handset rocks on its cradle, less and less, then rests.
    shake: {
        duration: 1500, ease: 'ease-in-out',
        tracks: {
            rotation: [[0, 0], [0.06, -13], [0.12, 12], [0.18, -11], [0.24, 10], [0.30, -8],
                [0.36, 7], [0.42, -4], [0.50, 0], [1, 0]],
        },
    },
    // A line of the ringing comes and goes.
    arc: {
        duration: 1500, ease: 'ease-out',
        tracks: {opacity: [[0, 0], [0.20, 1], [0.55, 1], [0.75, 0], [1, 0]]},
    },
    // A slow wave leaves the dot.
    sonar: {
        duration: 2200, ease: 'ease-out',
        tracks: {
            scale: [[0, 0.4], [0.80, 1.9], [1, 1.9]],
            opacity: [[0, 0.9], [0.80, 0], [1, 0]],
        },
    },
    // The bubble hops once every three seconds.
    pop: {
        duration: 3000, ease: 'ease-in-out',
        tracks: {
            y: [[0, 0], [0.70, 0], [0.78, -2.2], [0.86, 0], [0.92, -0.8], [1, 0]],
            scale: [[0, 1], [0.70, 1], [0.78, 1.12], [0.86, 0.96], [0.92, 1.03], [1, 1]],
        },
    },
    spin: {
        duration: 3200, ease: 'linear',
        tracks: {rotation: [[0, 0], [1, 360]]},
    },
    // A wave lights up, then fades back: the inner one first, the outer
    // one a little later, as if calling out and waiting for an answer.
    call_out: {
        duration: 1800, ease: 'ease-in-out',
        tracks: {opacity: [[0, 0.25], [0.22, 1], [0.55, 0.25], [1, 0.25]]},
    },
    // A dotted ring turns slowly, like a search.
    search: {
        duration: 2400, ease: 'linear',
        tracks: {rotation: [[0, 0], [1, 360]]},
    },
    blink: {
        duration: 2200, ease: 'ease-in-out',
        tracks: {opacity: [[0, 1], [0.45, 1], [0.55, 0.2], [1, 0.2]]},
    },
};

const stroke = width => `fill="none" stroke="{c}" stroke-width="${width}"`;
const round = 'stroke-linecap="round"';

// --- the pieces ---------------------------------------------------------

const PHONE = `<rect x="4.4" y="1.4" width="7.2" height="13.2" rx="1.9" ${stroke(1.4)}/>` +
    `<path d="M7.1 12.2h1.8" ${stroke(1.3)} ${round}/>`;
const HANDSET = '<path fill="{c}" d="M3.1 2.5c.5-.5 1.3-.5 1.8 0l1.3 1.4c.4.5.4 1.2 0 1.7l-.8.8c.9 1.8 2.3 3.1 4.1 4' +
    'l.8-.8c.5-.4 1.2-.4 1.7 0l1.4 1.3c.5.5.5 1.3 0 1.8l-.7.7c-.8.8-2 1-3.1.6-3.4-1.2-6.3-4.1-7.5-7.5' +
    '-.4-1.1-.2-2.3.6-3.1z"/>';
const BUBBLE = 'M0 1.6A1.6 1.6 0 0 1 1.6 0h6.8A1.6 1.6 0 0 1 10 1.6v3.8A1.6 1.6 0 0 1 8.4 7H4.2L1.8 9.2V7h-.2' +
    'A1.6 1.6 0 0 1 0 5.4z';

const dot = (cx, cy, r) => `<circle cx="${cx}" cy="${cy}" r="${r}" fill="{c}"/>`;
const ring = (cx, cy, r, width = 1.2) => `<circle cx="${cx}" cy="${cy}" r="${r}" ${stroke(width)}/>`;

// The bubble of a message, 10 × 9.2 before scaling; it hops around its middle.
const bubble = (x, y, scale) => ({
    art: `<path transform="translate(${x} ${y}) scale(${scale})" fill="{c}" d="${BUBBLE}"/>`,
    ink: 'amber', motion: 'pop', pivot: [x + 5 * scale, y + 4.6 * scale],
});

const missed = (x, y) => ({
    art: `<path transform="translate(${x} ${y})" d="M0 0l4 4M4 .6V4H.6" ${stroke(1.4)} ${round} stroke-linejoin="round"/>`,
    ink: 'red',
});

// A battery nearly empty, 6.4 × 3.8.
const lowBattery = (x, y) => ({
    art: `<g transform="translate(${x} ${y})"><rect x=".55" y=".55" width="4.6" height="2.7" rx=".8" ${stroke(1.1)}/>` +
        `<path d="M6.3 1.3v1.2" ${stroke(1.1)} ${round}/><rect x="1.3" y="1.2" width="1.1" height="1.4" fill="{c}"/></g>`,
    ink: 'red',
});

// Something to do (a setting missing, messages unavailable, a switch off
// on the iPhone): a disc, an exclamation mark cut out of it, centred on
// (cx, cy). Larger than the dot of "linked", and never plain.
const attention = (cx, cy) => ({
    art: `<path fill="{c}" fill-rule="evenodd" d="M${cx - 3.2} ${cy}a3.2 3.2 0 1 0 6.4 0a3.2 3.2 0 1 0 -6.4 0z` +
        `M${cx - 0.6} ${cy - 2.1}h1.2v2.6h-1.2z` +
        `M${cx - 0.65} ${cy + 1.25}h1.3v1.2h-1.3z"/>`,
    ink: 'amber',
});

// Paused: two upright bars, from (x, y), `h` tall.
const pauseBars = (x, y, h) => ({
    art: `<path d="M${x} ${y}v${h}M${x + 2.8} ${y}v${h}" ${stroke(1.5)} ${round}/>`,
    ink: 'amber', motion: 'blink',
});

// A dot with a wave that leaves it.
const wave = (cx, cy, r, ink) => [
    {art: ring(cx, cy, 2.1), ink, motion: 'sonar', pivot: [cx, cy], still: false},
    {art: dot(cx, cy, r), ink},
];

// Two spools and the tape between them, drawn around (0, 0) and placed by
// `at` = [x, y, scale]. Each spool turns around its own axis.
const tape = ([x, y, scale]) => {
    const place = `transform="translate(${x} ${y}) scale(${scale})"`;
    const spool = cx => ({
        art: `<g ${place} ${stroke(1.2)}><circle cx="${cx}" cy="0" r="2"/><path d="M${cx} -2v1" ${round}/></g>`,
        ink: 'amber', motion: 'spin', pivot: [x + cx * scale, y],
    });
    return [
        {art: `<path ${place} d="M-3 2h6" ${stroke(1.2)}/>`, ink: 'amber'},
        spool(-3), spool(3),
    ];
};

const glyph = (art, more = {}) => ({art, ink: 'glyph', pivot: [8, 8], ...more});

// Internet through the iPhone: an arrow up and an arrow down, side by side,
// from (x, y), `h` tall, `gap` apart (their heads, at opposite ends, do
// not meet). Still, both are drawn; on its way, they take turns (the same
// pulse as the waves of "reconnecting").
const updown = (x, y, h, ink, moves = true, gap = 2.7) => {
    const arrow = (cx, up) => {
        const [from, to] = up ? [y + h, y] : [y, y + h];
        const head = up ? 1.2 : -1.2;
        return `<path d="M${cx} ${from}V${to}M${cx - 1.1} ${to + head}l1.1 ${-head} 1.1 ${head}" ` +
            `${stroke(1.25)} ${round} stroke-linejoin="round"/>`;
    };
    return [
        {art: arrow(x + 1.1, true), ink, ...moves ? {motion: 'call_out'} : {}},
        {art: arrow(x + 1.1 + gap, false), ink, ...moves ? {motion: 'call_out', delay: 450} : {}},
    ];
};

// Two waves beside the phone, going out from (cx, cy): ")" and "))".
// Still, both show at once, half bright: still not the absent phone.
const callOut = (cx, cy) => [2.6, 4.6].map((r, i) => {
    const dx = (r * Math.cos(Math.PI * 50 / 180)).toFixed(2);
    const dy = (r * Math.sin(Math.PI * 50 / 180)).toFixed(2);
    return {
        art: `<path d="M${(cx + +dx).toFixed(2)} ${(cy - dy).toFixed(2)}a${r} ${r} 0 0 1 0 ${(2 * dy).toFixed(2)}" ` +
            `${stroke(1.3)} ${round}/>`,
        ink: 'glyph', alpha: 0.75, motion: 'call_out', delay: i * 260,
    };
});

// An open ring, three quarters of it, where the dot of "linked" sits:
// turning, it searches; still, it is neither the dot nor the closed ring of
// the absent phone.
const searching = (cx, cy, r) => ({
    art: `<circle cx="${cx}" cy="${cy}" r="${r}" ${stroke(1.3)} ${round} ` +
        `stroke-dasharray="${(2 * Math.PI * r * 0.62).toFixed(2)} ${(2 * Math.PI * r * 0.38).toFixed(2)}"/>`,
    ink: 'glyph', motion: 'search', pivot: [cx, cy],
});

// --- the two styles -----------------------------------------------------

// A · Phone: a smartphone drawn in outline, with nothing beside it while
// all is well. A sign only appears when something happens.
const phone = {
    bare: [glyph(PHONE)],
    linked: [glyph(PHONE)],
    away: [glyph(PHONE, {alpha: DIM})],
    // The phone steps aside for two waves that go out and come back.
    reconnecting: [glyph(`<g transform="translate(-2 0)">${PHONE}</g>`, {alpha: HALF}), ...callOut(9.2, 8)],
    unread: [glyph(PHONE), bubble(8.4, 0.2, 0.74)],
    ringing: [
        glyph(PHONE, {motion: 'buzz'}),
        {art: `<path d="M2.3 5.2v5.6M13.7 5.2v5.6" ${stroke(1.2)} ${round}/>`, ink: 'amber', motion: 'arc'},
        {art: `<path d="M.7 6.4v3.2M15.3 6.4v3.2" ${stroke(1.2)} ${round}/>`, ink: 'amber', motion: 'arc', delay: 180},
    ],
    call: [glyph(PHONE), ...wave(13, 3, 2, 'green')],
    missed: [glyph(PHONE), missed(10.4, 0.8)],
    // The phone steps aside for the spools.
    voicemail: [glyph(`<g transform="translate(-1.5 0)">${PHONE}</g>`), ...tape([12.26, 1.7, 0.62])],
    // The phone steps aside for the two bars of a pause.
    paused: [glyph(`<g transform="translate(-2 0)">${PHONE}</g>`), pauseBars(11.6, 1.3, 4.4)],
    battery: [glyph(`<g transform="translate(-1.5 0)">${PHONE}</g>`), lowBattery(9.4, 0.4)],
    // ...and for the amber disc of something to do.
    attention: [glyph(`<g transform="translate(-2.6 0)">${PHONE}</g>`), attention(12.7, 3.6)],
    // The phone steps aside for two arrows as tall as its screen, in the
    // color of the glyph: a state, not news. Red when it did not connect.
    internet: [glyph(`<g transform="translate(-3.4 0)">${PHONE}</g>`), ...updown(9.9, 3.2, 9.6, 'glyph', true, 3)],
    internet_failed: [glyph(`<g transform="translate(-3.4 0)">${PHONE}</g>`), ...updown(9.9, 3.2, 9.6, 'red', false, 3)],
};

// B · Handset: the receiver of a landline phone. It keeps a small amber dot
// while linked, rocks when it rings, turns green during a call.
const handset = {
    bare: [glyph(HANDSET)],
    linked: [glyph(HANDSET), {art: dot(12.6, 3.4, 2), ink: 'amber'}],
    away: [glyph(HANDSET, {alpha: DIM}), {art: ring(12.6, 3.4, 1.5), ink: 'glyph', alpha: DIM}],
    // Where "linked" puts its dot, a dotted ring that turns.
    reconnecting: [glyph(HANDSET, {alpha: HALF}), searching(12.6, 3.4, 2)],
    unread: [glyph(HANDSET), bubble(8, 0.3, 0.76)],
    ringing: [
        glyph(HANDSET, {motion: 'shake'}),
        {art: `<path d="M10.6 4.6a3.6 3.6 0 0 1 0 5" ${stroke(1.3)} ${round}/>`, ink: 'amber', motion: 'arc'},
        {art: `<path d="M12.5 2.9a6 6 0 0 1 0 8.4" ${stroke(1.3)} ${round}/>`, ink: 'amber', motion: 'arc', delay: 180},
    ],
    call: [{art: HANDSET, ink: 'green', pivot: [8, 8]}, ...wave(12.6, 3.4, 1.6, 'green')],
    missed: [glyph(HANDSET), missed(9.8, 1)],
    voicemail: tape([8, 8, 1]),
    // Where "linked" puts its dot: the bars of a pause, or the disc of
    // something to do, larger than the dot and marked.
    paused: [glyph(HANDSET), pauseBars(11.2, 1, 4.4)],
    battery: [glyph(HANDSET), lowBattery(9.4, 0.6)],
    attention: [glyph(HANDSET), attention(12.6, 3.5)],
    // Where "linked" puts its amber dot, the two arrows, amber.
    internet: [glyph(HANDSET), ...updown(10.3, 0.7, 6, 'amber')],
    internet_failed: [glyph(HANDSET), ...updown(10.3, 0.7, 6, 'red', false)],
};

const ART = {phone, handset};

// The layers of one state, bottom first. An unknown style or state falls
// back on the phone and on the bare glyph.
export function iconLayers(style, state) {
    const set = ART[style] ?? ART.phone;
    return set[state] ?? set.bare;
}

// One layer as a whole SVG image; color is '#rrggbb'.
export function layerSvg(layer, color) {
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${GRID}" height="${GRID}" viewBox="0 0 ${GRID} ${GRID}">` +
        `${layer.art.replaceAll('{c}', color)}</svg>`;
}

// One state as a single still image, all its layers stacked: what Phone
// Settings shows to choose a style. `inks` gives a color per ink; `pad` is
// the margin around the drawing, in grid units, filled with `background`
// (rounded) when there is one; `size` is the side of the image in pixels.
// A layer that only exists while it moves (a wave) is drawn fainter.
export function stateSvg(style, state, inks, {size = GRID, pad = 0, background = ''} = {}) {
    const side = GRID + 2 * pad;
    const layers = iconLayers(style, state).map(layer => {
        const alpha = (layer.alpha ?? 1) * (layer.still === false ? 0.5 : 1);
        return `<g opacity="${alpha}">${layer.art.replaceAll('{c}', inks[layer.ink] ?? inks.glyph)}</g>`;
    }).join('');
    const tile = background
        ? `<rect x="${-pad}" y="${-pad}" width="${side}" height="${side}" rx="${side / 5}" fill="${background}"/>` : '';
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" ` +
        `viewBox="${-pad} ${-pad} ${side} ${side}">${tile}${layers}</svg>`;
}
