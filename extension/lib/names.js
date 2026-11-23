// SPDX-License-Identifier: GPL-2.0-or-later
// The names this extension shares with the engine and its installer, in one
// place. The uuid, settings schema and gettext domain live in metadata.json.

// Prefix of what the extension writes to the journal.
export const LOG_PREFIX = 'landline';

// The systemd user unit of the engine.
export const ENGINE_UNIT = 'landline-engine.service';

// The socket of the engine, below $XDG_RUNTIME_DIR.
export const SOCKET_PATH = ['landline', 'landlined.sock'];

// The system setup helper. Only the copy owned by root is ever run through
// pkexec; the ones below the home directory are only asked for their status:
// first the one of an engine installed from Phone Settings, then the one
// scripts/install-local.sh leaves.
export const ROOT_HELPER = '/usr/local/libexec/landline/landline-setup';
export const LOCAL_HELPERS = [
    ['.local', 'lib', 'landline', 'engine', 'system', 'landline-setup'],
    ['.local', 'lib', 'landline', 'landline-setup'],
];
