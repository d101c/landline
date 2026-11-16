// SPDX-License-Identifier: GPL-2.0-or-later
// Delays, limits and identifiers of the state model.

export const MESSAGE_PILL_MS = 7000;
export const SENT_PILL_MS = 2000;
export const NOTICE_PILL_MS = 7000;
export const SEND_TIMEOUT_MS = 30000;
export const RECENT_MESSAGE_MS = 10 * 60 * 1000;
export const LOW_BATTERY = 15;
// The setup tour: every step stays on screen at least this long.
export const TOUR_MIN_STEP_MS = 1500;
// After "paired", how long both links may take to come up before the
// pairing is called broken; shorter when the engine said "already paired".
export const TOUR_VERIFY_MS = 45000;
export const TOUR_ALREADY_PAIRED_MS = 15000;
export const TOUR_TICK_MS = 1200;
// Links up, but messages and contacts both fail for another reason than a
// switch still off on the iPhone: after this long it is called a fault.
export const OBEX_STUCK_MS = 20000;
// How long a restart of the engine may take to bring the link back.
export const FIX_TIMEOUT_MS = 45000;
export const UNPAIR_TIMEOUT_MS = 15000;
// On the step of the switches, how often the engine is asked to try the
// messages and contacts again, so a switch just turned on ticks at once.
export const RETRY_PROFILES_MS = 3000;
// The indicator at rest: a new state is shown once it has held this long, so
// a link that comes and goes does not make the top bar blink.
export const LINK_SETTLE_MS = 3000;
// Links that drop more than FLAP_LIMIT times within FLAP_WINDOW_MS are not a
// link: the indicator settles on "Link again" until the user acts or the
// link has held for STEADY_MS.
export const FLAP_WINDOW_MS = 30000;
export const FLAP_LIMIT = 2;
export const STEADY_MS = 60000;
// A setting just sent to the engine: for this long its older value, still
// on its way back, is not taken for a change made elsewhere.
export const SETTING_ECHO_MS = 8000;
// After a click on "iPhone" or "This computer" in a call, how long the sound
// may take to move before the card says it did not. A call answered with
// "Answer here" gets longer: the iPhone opens the voice link after answering.
export const AUDIO_MOVE_MS = 3000;
export const AUDIO_ANSWER_MS = 6000;
export const FRESH_MESSAGES = 3;
export const COMPOSE_RESULTS = 6;

export const MESSAGES_APP = 'com.apple.MobileSMS';
export const PHONE_APP = 'com.apple.mobilephone';
export const CATEGORY_INCOMING_CALL = 1;
export const CATEGORY_MISSED_CALL = 2;
export const MAX_SEEN = 4000;
export const MAX_SHOWN_NOTES = 2000;

// Sent once, in this order, each time the socket opens. After that
// everything arrives as events.
export const STARTUP_COMMANDS = [
    {command: 'subscribe'},
    {command: 'bt_status'},
    {command: 'bt_list_devices'},
    {command: 'bt_list_notifications'},
    {command: 'bt_list_calls'},
    {command: 'bt_list_threads'},
    {command: 'bt_list_call_history'},
    // Where the sound goes, and whether the iPhone is here. An engine from
    // before these just says OK.
    {command: 'bt_audio'},
    {command: 'bt_presence'},
];
