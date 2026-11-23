// SPDX-License-Identifier: GPL-2.0-or-later
// The words of the "classic link on demand" part of the engine's socket, in
// one place: commands, events, fields and their values. The model never
// writes one of these as a bare string, so a name the engine spells
// differently is changed here and nowhere else.
//
// An engine from before all this sends none of the fields and none of the
// events: every reader below then answers null, and the model keeps the
// rules it had ("both links stay up").

export const COMMAND = {
    setLinkMode: 'bt_set_link_mode',     // {mode}
    lease: 'bt_lease',                   // {name, held}
    callDial: 'bt_call_dial',            // {number, audio?, operation_id?}
    callCancel: 'bt_call_cancel',        // {operation_id}
    callAction: 'bt_call_action',        // {action, path?, uid?, audio?}
};

export const EVENT = {
    sendProgress: 'bt_send_progress',    // {operation_id, thread, stage}
    incomingCall: 'bt_incoming_call',    // {uid, state, caller, answer_here, ...}
    callProgress: 'bt_call_progress',    // {operation_id, number, stage}
    callResult: 'bt_call_result',        // {action, success, operation_id?, reason?}
};

// Fields added to bt_status and bt_connection_changed.
export const FIELD = {
    linkMode: 'link_mode',
    linkModeEffective: 'link_mode_effective',
    linkModeReason: 'link_mode_reason',
    classicState: 'classic_state',
    classicLeases: 'classic_leases',
    classicOpenedBy: 'classic_opened_by',
    messages: 'messages',
    contacts: 'contacts',
    callsAvailable: 'calls_available',
    lastSync: 'last_sync',
    nextSync: 'next_sync',
    // The link that carries the notifications.
    leState: 'le_state',
    leCause: 'le_cause',
    leDetail: 'le_detail',
    // LE links that ended before they were of any use, since the last one
    // that served (diagnostic; also how long the engine holds off).
    leStrikes: 'le_strikes',
    // The iPhone has not come back since this computer's Bluetooth
    // controller was reset (#59): 'computer' or 'phone', whose Bluetooth to
    // turn off and on; false otherwise.
    leAfterReset: 'le_after_reset',
};

// `classic_state`.
export const CLASSIC = {
    idle: 'idle', opening: 'opening', open: 'open', lingering: 'lingering', closing: 'closing', failed: 'failed',
};

export const LINK_MODE = {lean: 'lean', full: 'full'};
export const LINK_MODE_REASON = {
    noLe: 'no_le',
    notificationsOff: 'notifications_off',
    phoneKeepsLink: 'phone_keeps_link',
};

// `messages` and `contacts`.
export const SERVICE = {
    ready: 'ready', onDemand: 'on_demand', forbidden: 'forbidden',
    unavailable: 'unavailable', unknown: 'unknown',
};

// `calls_available`.
export const CALLS = {ready: 'ready', onDemand: 'on_demand', controlOnly: 'control_only', off: 'off'};

export const LEASE = {reading: 'reading', setup: 'setup'};

export const SEND_STAGE = {connecting: 'connecting', sending: 'sending'};

// bt_incoming_call.
export const RING_STATE = {ringing: 'ringing', ended: 'ended'};
export const ANSWER_HERE = {connecting: 'connecting', ready: 'ready', unavailable: 'unavailable'};
export const RING_FIELD = {
    uid: 'uid', state: 'state', caller: 'caller', canAnswerOnPhone: 'can_answer_on_phone',
    canDecline: 'can_decline', answerHere: 'answer_here', path: 'path', since: 'since',
    // In the answer to bt_list_calls: the call ringing right now, or null.
    listed: 'incoming',
};

export const CALL_ACTION = {answer: 'answer', decline: 'decline', hangup: 'hangup'};
export const CALL_AUDIO = {here: 'here', phone: 'phone'};

// bt_call_progress, and why a call could not be placed.
export const DIAL_STAGE = {connecting: 'connecting', handsFree: 'hands_free', dialing: 'dialing'};
// bt_call_progress again, after the call was placed or answered: where its
// sound is, and a call the iPhone accepted that never showed up.
export const CALL_STAGE = {audioHere: 'audio_here', audioFailed: 'audio_failed', noCall: 'no_call'};
// Why the sound did not come here.
export const AUDIO_REASON = {noVoiceLink: 'no_voice_link', voiceLinkUnused: 'voice_link_unused'};
// In `calls` of bt_connection_changed: what was asked for the call
// ('here', 'phone' or null), and whether it was asked here and did not come.
export const CALLS_FIELD = {audioWanted: 'audio_wanted', audioFailed: 'audio_failed'};
export const DIAL_REASON = {
    noLink: 'no_link', noHandsFree: 'no_hands_free', refused: 'refused',
    cancelled: 'cancelled', busy: 'busy', callsOff: 'calls_off',
};

// `le_state` and `le_cause`.
export const LE_STATE = {
    connected: 'connected', waiting: 'waiting', paused: 'paused', unavailable: 'unavailable', off: 'off',
};
export const LE_CAUSE = {
    afterResume: 'after_resume', phoneSilent: 'phone_silent', phoneAway: 'phone_away',
    unresolvedAddress: 'unresolved_address', advertOffAir: 'advert_off_air',
    // With `waiting`: a link that has just been lost; links that died
    // unused, and the iPhone is being called again (`le_strikes` > 0); the
    // engine leaving the iPhone alone for a moment after such links (gone
    // by default since #29, kept for an engine that still does it).
    reconnecting: 'reconnecting', holdingOff: 'holding_off', unsteady: 'unsteady',
};
