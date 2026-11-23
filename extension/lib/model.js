// SPDX-License-Identifier: GPL-2.0-or-later
// The state model: what the extension knows and what it shows, with no Shell
// or GLib import. Engine events come in through handleEvent(), user actions
// through the public methods, and view() says what the top bar, the card and
// the setup dialog must show. Time comes from the `now` function.
//
// The class is assembled from one module per subject, each adding its methods
// to the one before; they all work on the same object.

import {ModelCore} from './model/core.js';
import {withQuiet} from './model/quiet.js';
import {withDemand} from './model/demand.js';
import {withLink} from './model/link.js';
import {withReconnect} from './model/reconnect.js';
import {withMessages} from './model/messages.js';
import {withNotifications} from './model/notifications.js';
import {withCalls} from './model/calls.js';
import {withDial} from './model/dial.js';
import {withPhoneLink} from './model/phoneLink.js';
import {withDaily} from './model/daily.js';
import {withInternet} from './model/internet.js';
import {withTour} from './model/tour.js';
import {withObex} from './model/obex.js';
import {withForget} from './model/forget.js';
import {withIcon} from './model/icon.js';
import {withView} from './model/view.js';

export * from './model/constants.js';
export * as Protocol from './model/protocol.js';
export {reconnectReason, holdOffSeconds, RECONNECT_REASONS} from './model/reconnect.js';
export {INTERNET_AWAY_WINDOW_MS} from './model/internet.js';
export {initials, firstName, formatDuration, formatNumber, sender} from './model/format.js';

const parts = [withQuiet, withDemand, withLink, withReconnect, withMessages, withNotifications, withCalls, withDial, withPhoneLink, withDaily,
    withInternet, withTour, withObex, withForget, withIcon, withView];

export class Model extends parts.reduce((Base, part) => part(Base), ModelCore) {}
