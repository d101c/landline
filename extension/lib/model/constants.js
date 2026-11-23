// SPDX-License-Identifier: GPL-2.0-or-later
// Delays, limits and identifiers of the state model.

export const MESSAGE_PILL_MS = 7000;
export const SENT_PILL_MS = 2000;
// A notification of another app: how long its pill stays, and within how
// long the next ones join it ("3 notifications") instead of replacing it.
export const NOTE_PILL_MS = 6000;
export const NOTE_BURST_MS = 5000;
// The iPhone is back with notifications that came while it was away: the
// engine sends them one by one, the summary waits until they stop coming,
// then stays this long.
export const AWAY_SETTLE_MS = 2500;
export const SUMMARY_PILL_MS = 7000;
export const SUMMARY_APPS = 3;
// "Earlier", when Phone Settings asks for it: notifications that left the
// iPhone, kept in memory only, this long and no more than this many.
export const EARLIER_KEEP_MS = 12 * 60 * 60 * 1000;
export const EARLIER_MAX = 200;
export const EARLIER_SHOWN = 40;
// "On your iPhone", in the menu: how many notifications before "Show all".
export const PHONE_NOTES_SHOWN = 6;
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
// "Reconnecting", between away and linked (model/reconnect.js). A sign of
// the iPhone (seen by the engine, a link coming up, the engine calling it
// again) keeps it said this long; after this computer woke up, with an
// engine that does not say how the link is doing, this long.
export const RECONNECT_HOLD_MS = 60000;
export const RECONNECT_SLEEP_MS = 120000;
// A link up again, waiting for the iPhone to serve it: at most this long
// before it is called linked anyway (an iPhone that shares nothing).
export const RECONNECT_SETUP_MS = 15000;
// Never said longer than this in a row: after that, away (or "Link
// again") and why, until a link serves or all is calm again.
export const RECONNECT_MAX_MS = 5 * 60 * 1000;
// Nothing under way for this long, the time said so far is forgotten: a
// reason that goes away for a pass or two does not start the five minutes
// again.
export const RECONNECT_CALM_MS = 5000;
// From linked, a drop must last this long before it is called
// reconnecting: a served link that blinks stays linked.
export const RECONNECT_AFTER_LINK_MS = 10000;
// A setting just sent to the engine: for this long its older value, still
// on its way back, is not taken for a change made elsewhere.
export const SETTING_ECHO_MS = 8000;
// After a click on "iPhone" or "This computer" in a call, how long the sound
// may take to move before the card says it did not. A call answered with
// "Answer here" gets longer: the iPhone opens the voice link after answering.
export const AUDIO_MOVE_MS = 3000;
export const AUDIO_ANSWER_MS = 6000;
// "Play iPhone audio on this computer" turned on: how long the menu says
// "Connecting…" before it says the iPhone did not come. The engine may
// open the classic link (20 s at most), wait for hands-free (8 s), then
// let the link settle before it opens the sound (5 s).
export const PHONE_AUDIO_WAIT_MS = 40000;
// The classic link on demand. A lease on it is renewed this often while it
// is wanted (the engine lets one lapse after 300 s).
export const LEASE_RENEW_MS = 4 * 60 * 1000;
// How long the last battery level known is still worth showing.
export const BATTERY_KEEP_MS = 12 * 60 * 60 * 1000;
// A message shown from its notification waits this long for the message
// itself, which brings the conversation it belongs to.
export const PROVISIONAL_MS = 60000;
// "Answer here" waits for the hands-free link; the engine gives up after
// 15 s and says so. Should it stay silent, the card stops waiting too.
export const ANSWER_HERE_MS = 17000;
// A call being placed: the engine allows 20 s for the link and 15 s more
// for the hands-free link. After that the card stops waiting.
export const DIAL_TIMEOUT_MS = 45000;
// Placed, says the engine: how long the call may take to show up.
// The engine says so itself after 20 s (bt_call_progress, no_call).
export const DIAL_PLACED_MS = 25000;
// The icon of the top bar moves for this long after something new arrived
// (a message, a voicemail, notifications pausing), then stays still: nothing
// keeps the top bar repainting while nothing happens.
export const ATTENTION_MS = 60000;
export const FRESH_MESSAGES = 3;
export const COMPOSE_RESULTS = 6;

export const MESSAGES_APP = 'com.apple.MobileSMS';
export const PHONE_APP = 'com.apple.mobilephone';
export const CATEGORY_INCOMING_CALL = 1;
export const CATEGORY_MISSED_CALL = 2;
export const CATEGORY_VOICEMAIL = 3;
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
