// SPDX-License-Identifier: GPL-2.0-or-later
// The iPhone's Internet on this computer: one switch.
//
// NetworkManager does the connecting (lib/hotspot.js gives its state here
// through setHotspot()); this decides what the switch means and what the
// menu and the icon say:
//   - turned on while the iPhone is away, it waits and connects as soon as
//     the iPhone is back, instead of failing;
//   - a failure is told with its reason and the one thing to do, the
//     iPhone's Personal Hotspot screen drawn when the iPhone did not share;
//   - when the iPhone walks away NetworkManager loses the link and the
//     connection stops by itself: the menu says so and offers to reconnect
//     when it is back.
// The switch asks the extension through onInternet(on); nothing here talks
// to NetworkManager.

// After the connection fell, the iPhone found away this soon still counts
// as having walked away: the link to the engine may notice it later than
// NetworkManager.
export const INTERNET_AWAY_WINDOW_MS = 60 * 1000;

export const withInternet = Base => class extends Base {
    // The address NetworkManager knows the iPhone by.
    get phoneAddress() {
        return this._status?.device_address || '';
    }

    // From lib/hotspot.js: {state: 'unavailable' | 'off' | 'connecting' | 'on',
    // failed, reason}.
    setHotspot(hotspot) {
        const before = this._hotspot;
        this._hotspot = hotspot ?? null;
        const now = this._now();
        if (hotspot?.state === 'on') {
            // It works: the drawing is no longer offered by itself.
            this._internetWorked = true;
            this._internetWish = false;
            this._internetLost = null;
        }
        const failedNow = hotspot?.failed === true &&
            (before?.failed !== true || before.reason !== hotspot.reason || before.state !== 'off');
        if (failedNow) {
            this._internetFailure = {seen: false};
            this._internetGuide = null;
            this._internetLost = hotspot.reason === 'lost' ? {at: now, away: this._internetAway()} : null;
        } else if (hotspot?.failed !== true) {
            this._internetFailure = null;
        }
        this._changed();
    }

    // The iPhone is known not to be here: no use asking it now.
    _internetAway() {
        return ['away', 'reconnecting', 'bluetooth'].includes(this._link());
    }

    // The switch of the menu.
    setInternet(on) {
        this._internetLost = null;
        this._internetGuide = null;
        this._internetFailure = null;
        if (!on) {
            this._internetWish = false;
            this.onInternet?.(false);
        } else if (this._internetAway()) {
            // Waits for the iPhone; said in the menu.
            this._internetWish = true;
        } else {
            this._internetWish = false;
            this.onInternet?.(true);
        }
        this._changed();
    }

    // "Reconnect When It's Back", after the iPhone walked away.
    keepInternet() {
        this.setInternet(true);
    }

    // "Show Me How" / "Hide".
    toggleInternetGuide() {
        this._internetGuide = !this._internetGuideShown();
        this._changed();
    }

    // Called at every change (core._changed()).
    _internetWatch() {
        const lost = this._internetLost;
        if (lost && !lost.away && this._internetAway() && this._now() - lost.at < INTERNET_AWAY_WINDOW_MS)
            lost.away = true;
        if (this._internetWish && this._link() === 'linked' && this._hotspot && this._hotspot.state !== 'unavailable') {
            this._internetWish = false;
            this.onInternet?.(true);
        }
    }

    // The menu was opened: a failure told on the icon has been seen.
    _internetSeen() {
        if (this._internetFailure)
            this._internetFailure.seen = true;
    }

    // Shown under the switch unless the user said: until it has worked once.
    _internetGuideShown() {
        return this._internetGuide ?? !this._internetWorked;
    }

    // What the icon shows for it: 'internet' (online through the iPhone, or
    // on its way: `moving`), 'internet_failed' until the menu is opened, or
    // null.
    _internetIcon() {
        const h = this._hotspot;
        if (!h || h.state === 'unavailable' || !this._status?.device_address)
            return null;
        if (h.state === 'on' || h.state === 'connecting')
            return 'internet';
        if (h.failed && this._internetFailure && !this._internetFailure.seen && !this._internetWish)
            return 'internet_failed';
        return null;
    }

    // The entry of the menu, or null when NetworkManager has no profile for
    // this iPhone:
    //   on        the switch
    //   state     'off', 'connecting', 'on', 'waiting' or 'failed'
    //   line      one sentence under it ('' for none); error: said as a failure
    //   remedy    the one thing to do, or ''
    //   guide     the drawing of Settings › Personal Hotspot is offered;
    //             guideShown: drawn now
    //   action    {id: 'retry' | 'keep', label} or null
    _internetView(battery) {
        const _ = this._;
        const h = this._hotspot;
        if (!h || h.state === 'unavailable' || !this._status?.device_address)
            return null;
        const view = {on: false, state: h.state, line: '', error: false, remedy: '', guide: false,
            guideShown: false, action: null};
        const retry = {id: 'retry', label: _('Try Again')};
        if (h.state === 'connecting') {
            view.on = true;
            view.line = _('Connecting to your iPhone…');
            return view;
        }
        if (h.state === 'on') {
            view.on = true;
            view.line = battery === null || battery === undefined
                ? _('Online through your iPhone')
                // Translators: %d is the battery level of the iPhone.
                : _('Online through your iPhone · battery %d %%').replace('%d', battery).replace('%%', '%');
            return view;
        }
        if (this._internetWish) {
            view.on = true;
            view.state = 'waiting';
            view.line = _('Connects as soon as your iPhone is back.');
            return view;
        }
        // A failure the user has acted on since is not told any more.
        if (!h.failed || !this._internetFailure)
            return view;
        view.state = 'failed';
        view.error = true;
        // The words of the iPhone, as an English sentence names them.
        const where = `${this.phoneLabel?.('settings') ?? 'Settings'} › ${this.phoneLabel?.('hotspot') ?? 'Personal Hotspot'}`;
        const allow = this.phoneLabel?.('allowOthers') ?? 'Allow Others to Join';
        switch (h.reason) {
        case 'lost':
            if (this._internetLost?.away) {
                view.error = false;
                view.line = _('Stopped when your iPhone went out of range.');
                view.action = {id: 'keep', label: _('Reconnect When It’s Back')};
                return view;
            }
            view.line = _('Your iPhone stopped sharing its connection.');
            break;
        case 'refused':
        case 'dropped':
            if (this._internetAway()) {
                view.line = _('Your iPhone is not nearby.');
                view.remedy = _('Bring it closer, then try again.');
                view.action = retry;
                return view;
            }
            view.line = _('Your iPhone did not share its connection.');
            break;
        case 'no-internet':
            view.line = _('Your iPhone answered, but gave no Internet.');
            view.remedy = _('Check that Mobile Data is on, on your iPhone. Then try again.');
            view.action = retry;
            return view;
        case 'timeout':
            view.line = _('Your iPhone did not answer in time.');
            view.remedy = _('Unlock it and keep it close, then try again.');
            view.action = retry;
            return view;
        default:
            view.line = _('This computer could not start the connection.');
            view.remedy = _('Try again in a moment.');
            view.action = retry;
            return view;
        }
        // The iPhone said no: the one switch to turn on there, drawn.
        // Translators: the first %s is "Settings › Personal Hotspot", the
        // second "Allow Others to Join", as the iPhone writes them.
        view.remedy = _('On your iPhone, open %s and turn on %s. Then try again.')
            .replace('%s', where).replace('%s', allow);
        view.guide = true;
        view.guideShown = this._internetGuideShown();
        view.action = retry;
        return view;
    }
};
