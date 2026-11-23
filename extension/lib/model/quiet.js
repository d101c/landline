// SPDX-License-Identifier: GPL-2.0-or-later
// Silence: when nothing may pop up.
//
// Do Not Disturb, a shared or recorded screen, or "Mute iPhone on this
// computer": nothing pops up. No pill that grows, no name, no text, no
// card that comes down, no battery warning. The icon stays as it is at
// rest, still, with no bubble; unread messages are counted in the menu,
// which only opens when the user asks and still shows everything. Coming out of it
// replays nothing.

export const withQuiet = Base => class extends Base {
    // {muted, dnd, sharing, followDnd, whileSharing, callsThroughDnd}, any
    // subset.
    setQuiet(change) {
        const before = this._hush();
        Object.assign(this._quiet, change);
        if (this._hush() && !before)
            this._hushNow();
        this._changed();
    }

    // null, or why nothing may show: 'muted', 'sharing' or 'dnd'.
    _hush() {
        const q = this._quiet;
        if (q.muted)
            return 'muted';
        if (q.sharing && q.whileSharing)
            return 'sharing';
        if (q.dnd && q.followDnd)
            return 'dnd';
        return null;
    }

    // Calls may be let through Do Not Disturb, and only through that.
    _hushCalls() {
        const hush = this._hush();
        return hush !== null && !(hush === 'dnd' && this._quiet.callsThroughDnd);
    }

    // Silence begins: what was on its way to the top bar is put away.
    _hushNow() {
        const t = this._transient;
        if (t?.kind === 'message')
            this._unread.set(t.message.handle, t.message.thread);
        if (t && t.kind !== 'failed')
            this._transient = null;
    }
};
