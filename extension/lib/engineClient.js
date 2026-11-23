// SPDX-License-Identifier: GPL-2.0-or-later
// Client of the Bluetooth engine: a Unix socket carrying one JSON object per
// line. No Shell import.
//
// The protocol has no request identifier and answers most commands with a
// bare `OK` line. Commands are therefore sent one at a time: the next one
// leaves when the previous one got its `OK`, or its direct reply for the
// commands that have one, or after a delay. Every JSON line, reply or
// broadcast, is handed to onEvent() and told apart by its `command` field.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {LOG_PREFIX, SOCKET_PATH} from './names.js';

// Commands answered by a JSON line instead of `OK`, and the name it carries.
const DIRECT_REPLIES = {
    subscribe: 'protocol_info',
    unsubscribe: 'unsubscribed',
    protocol_info: 'protocol_info',
    state_snapshot: 'state_snapshot',
    bt_status: 'bt_status',
    bt_connection: 'bt_connection_changed',
    bt_list_devices: 'bt_devices',
    bt_list_notifications: 'bt_notifications',
    bt_list_threads: 'bt_threads',
    bt_list_messages: 'bt_messages',
    bt_list_contacts: 'bt_contacts',
    bt_list_calls: 'bt_calls',
    bt_list_call_history: 'bt_call_history',
    bt_diagnostics: 'bt_diagnostics',
    bt_call_action: 'bt_call_result',
    bt_call_tones: 'bt_call_result',
};

// Newer commands: an engine that knows them answers with the object, an
// older one with a bare OK. Either ends the wait.
const OPTIONAL_REPLIES = {
    bt_audio: 'bt_audio',
    bt_presence: 'bt_presence',
};

// Commands whose answer is not waited for. Placing a call ends with a
// bt_call_result that may be half a minute away, and "cancel" must be able to
// leave meanwhile: the result is taken when it comes, like any event.
const UNACKNOWLEDGED = new Set(['bt_call_dial']);

// Call commands block the engine for up to 10 s.
const ACK_TIMEOUT_MS = 12000;
const RETRY_DELAYS_MS = [1000, 2000, 4000, 8000, 15000];

export function defaultSocketPath() {
    return GLib.build_filenamev([GLib.get_user_runtime_dir(), ...SOCKET_PATH]);
}

export class EngineClient {
    // onEvent(object): every JSON line received.
    // onState(state): 'connected' | 'absent'.
    constructor({path, onEvent, onState, hello = [], retryDelays = RETRY_DELAYS_MS, ackTimeout = ACK_TIMEOUT_MS}) {
        this._path = path;
        this._onEvent = onEvent;
        this._onState = onState;
        this._hello = hello;
        this._retryDelays = retryDelays;
        this._ackTimeout = ackTimeout;

        this._running = false;
        this._state = 'idle';
        this._attempt = 0;
        this._connecting = false;
        this._connection = null;
        this._input = null;
        this._cancellable = null;
        this._retryId = 0;
        this._ackId = 0;
        this._queue = [];
        this._waiting = null;
        this._writing = false;
        this._decoder = new TextDecoder('utf-8');
        this._encoder = new TextEncoder();
    }

    get state() {
        return this._state;
    }

    start() {
        if (this._running)
            return;
        this._running = true;
        this._connect();
    }

    // Closes the socket and removes every source. Nothing is called back
    // afterwards.
    stop() {
        this._running = false;
        this._clearRetry();
        this._drop();
        this._state = 'idle';
    }

    // Try now instead of waiting for the next attempt.
    retryNow() {
        if (!this._running || this._connection || this._connecting)
            return;
        this._clearRetry();
        this._connect();
    }

    // Queue one command. Dropped when the engine is not connected: the state
    // is read again in full at the next connection.
    send(command) {
        if (!this._connection)
            return false;
        this._queue.push(command);
        this._pump();
        return true;
    }

    _setState(state) {
        if (this._state === state)
            return;
        this._state = state;
        this._onState?.(state);
    }

    _connect() {
        this._connecting = true;
        this._cancellable = new Gio.Cancellable();
        const cancellable = this._cancellable;
        const client = new Gio.SocketClient();
        const address = new Gio.UnixSocketAddress({path: this._path});
        client.connect_async(address, cancellable, (source, result) => {
            let connection;
            try {
                connection = source.connect_finish(result);
            } catch (e) {
                if (cancellable.is_cancelled())
                    return;
                this._connecting = false;
                this._failed();
                return;
            }
            if (cancellable.is_cancelled()) {
                connection.close(null);
                return;
            }
            this._connecting = false;
            this._attempt = 0;
            this._connection = connection;
            this._input = new Gio.DataInputStream({
                base_stream: connection.get_input_stream(),
                close_base_stream: false,
            });
            this._queue = [...this._hello];
            this._waiting = null;
            this._setState('connected');
            this._readNext(cancellable);
            this._pump();
        });
    }

    _failed() {
        this._drop();
        if (!this._running)
            return;
        this._setState('absent');
        const delay = this._retryDelays[Math.min(this._attempt, this._retryDelays.length - 1)];
        this._attempt++;
        this._clearRetry();
        this._retryId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delay, () => {
            this._retryId = 0;
            if (this._running)
                this._connect();
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearRetry() {
        if (this._retryId) {
            GLib.source_remove(this._retryId);
            this._retryId = 0;
        }
    }

    _clearAck() {
        if (this._ackId) {
            GLib.source_remove(this._ackId);
            this._ackId = 0;
        }
    }

    _drop() {
        this._clearAck();
        this._cancellable?.cancel();
        this._cancellable = null;
        this._connecting = false;
        if (this._connection) {
            try {
                this._connection.close(null);
            } catch (e) {
                // Already closed by the other end.
            }
        }
        this._connection = null;
        this._input = null;
        this._queue = [];
        this._waiting = null;
        this._writing = false;
    }

    _readNext(cancellable) {
        this._input.read_line_async(GLib.PRIORITY_DEFAULT, cancellable, (stream, result) => {
            let bytes;
            try {
                [bytes] = stream.read_line_finish(result);
            } catch (e) {
                if (!cancellable.is_cancelled())
                    this._failed();
                return;
            }
            if (cancellable.is_cancelled())
                return;
            if (bytes === null) {
                // End of stream: the engine stopped.
                this._failed();
                return;
            }
            this._line(this._decoder.decode(bytes).trim());
            if (!cancellable.is_cancelled() && this._input)
                this._readNext(cancellable);
        });
    }

    _line(text) {
        if (text === '')
            return;
        if (text === 'OK') {
            if (this._waiting?.reply === 'OK' || this._waiting?.optional)
                this._acknowledged();
            return;
        }
        let event;
        try {
            event = JSON.parse(text);
        } catch (e) {
            // Its length only: the line may carry the text of a message.
            console.warn(`${LOG_PREFIX}: unreadable line from the engine (${text.length} characters)`);
            return;
        }
        if (!event || typeof event !== 'object')
            return;
        const waiting = this._waiting;
        if (waiting && (event.command === waiting.reply ||
            (event.command === 'error' && event.request === waiting.name)))
            this._acknowledged();
        try {
            this._onEvent?.(event);
        } catch (e) {
            console.error(e);
        }
    }

    _acknowledged() {
        this._clearAck();
        this._waiting = null;
        this._pump();
    }

    _pump() {
        if (!this._connection || this._waiting || this._writing || this._queue.length === 0)
            return;
        const command = this._queue.shift();
        const name = String(command.command ?? '');
        this._waiting = {
            name, reply: DIRECT_REPLIES[name] ?? OPTIONAL_REPLIES[name] ?? 'OK',
            optional: name in OPTIONAL_REPLIES,
        };
        this._writing = true;
        const cancellable = this._cancellable;
        const data = this._encoder.encode(`${JSON.stringify(command)}\n`);
        this._connection.get_output_stream().write_all_async(
            data, GLib.PRIORITY_DEFAULT, cancellable, (stream, result) => {
                try {
                    stream.write_all_finish(result);
                } catch (e) {
                    if (!cancellable.is_cancelled())
                        this._failed();
                    return;
                }
                if (cancellable.is_cancelled())
                    return;
                this._writing = false;
                if (UNACKNOWLEDGED.has(name))
                    this._waiting = null;
                // The reply may have been read before this callback ran.
                if (!this._waiting) {
                    this._pump();
                    return;
                }
                this._clearAck();
                this._ackId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._ackTimeout, () => {
                    this._ackId = 0;
                    console.warn(`${LOG_PREFIX}: no answer to ${name}, moving on`);
                    this._waiting = null;
                    this._pump();
                    return GLib.SOURCE_REMOVE;
                });
            });
    }
}
