// SPDX-License-Identifier: GPL-2.0-or-later
// The system helper `landline-setup`: it knows whether the Bluetooth
// settings of this computer allow an iPhone to be linked (`status`, no
// privilege needed) and changes them (`apply` and `revert`, as root).
//
// Only the copy installed in a directory of root is ever run through pkexec.
// Without it nothing is elevated from here: the commands are shown instead.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {LOCAL_HELPERS, LOG_PREFIX, ROOT_HELPER} from './names.js';
import {run} from './subprocess.js';

Gio._promisify(Gio.File.prototype, 'query_info_async');

// pkexec: 126 = dismissed, 127 = not authorized.
const PKEXEC_REFUSED = [126, 127];
// The helper: 2 = files written but one activation step failed.
const HELPER_PARTIAL = 2;
const WRITABLE_BY_OTHERS = 0o022;

// File attributes, read without blocking the caller; null when the file is
// not there or the query was cancelled.
async function attributes(path, names, cancellable) {
    try {
        return await Gio.File.new_for_path(path).query_info_async(
            names, Gio.FileQueryInfoFlags.NONE, GLib.PRIORITY_DEFAULT, cancellable);
    } catch (e) {
        return null;
    }
}

// Whether path is an executable file of root that nobody else can write.
export async function ownedByRoot(path, cancellable = null) {
    const info = await attributes(path, 'unix::uid,unix::mode,access::can-execute', cancellable);
    return info !== null &&
        info.get_attribute_uint32('unix::uid') === 0 &&
        (info.get_attribute_uint32('unix::mode') & WRITABLE_BY_OTHERS) === 0 &&
        info.get_attribute_boolean('access::can-execute');
}

async function executable(path, cancellable = null) {
    const info = await attributes(path, 'access::can-execute', cancellable);
    return info !== null && info.get_attribute_boolean('access::can-execute');
}

// Resolves to the installed helper, when it is a file of root that nobody
// else can write; to null otherwise.
export async function rootHelper(cancellable = null) {
    return await ownedByRoot(ROOT_HELPER, cancellable) ? ROOT_HELPER : null;
}

// Resolves to 'done', 'partial' (settings written, one could not be turned
// on), 'refused' (password not given), 'failed', or 'unavailable' when there
// is no helper of root to run.
//
// Cancelled while the password is being asked, pkexec is ended and nothing
// is changed. Cancelled later, the helper is already running as root and
// out of reach: it finishes writing the settings, and nobody waits for it.
export async function runAsRoot(path, verb, cancellable) {
    if (!path)
        return 'unavailable';
    try {
        const {exited, status} = await run(['pkexec', path, verb], cancellable);
        if (!exited)
            return 'failed';
        if (status === 0)
            return 'done';
        if (PKEXEC_REFUSED.includes(status))
            return 'refused';
        return status === HELPER_PARTIAL ? 'partial' : 'failed';
    } catch (e) {
        if (!cancellable?.is_cancelled())
            console.warn(`${LOG_PREFIX}: ${verb} of the Bluetooth settings failed: ${e.message}`);
        return 'failed';
    }
}

export class SetupHelper {
    constructor() {
        this._cancellable = new Gio.Cancellable();
    }

    _rootCopy() {
        return rootHelper(this._cancellable);
    }

    // Where `status` is asked: the copy of root, else one the engine's
    // install leaves in the home directory (never elevated).
    async _statusPath() {
        const root = await this._rootCopy();
        if (root)
            return root;
        for (const parts of LOCAL_HELPERS) {
            const local = GLib.build_filenamev([GLib.get_home_dir(), ...parts]);
            if (await executable(local, this._cancellable))
                return local;
        }
        return null;
    }

    // Resolves to {experimental, ertm, ready, persistent: {…}, required: {…},
    // elevation, command}, or null when there is no helper, it did not answer
    // properly, or the extension was disabled meanwhile. elevation: the
    // settings can be changed from here, with the password window of the
    // system.
    async status() {
        const path = await this._statusPath();
        if (!path || this._cancellable.is_cancelled())
            return null;
        try {
            const {exited, status: code, stdout} = await run([path, 'status'], this._cancellable, true);
            if (!exited || code !== 0)
                return null;
            const status = JSON.parse(stdout);
            if (!status || typeof status.ready !== 'boolean')
                return null;
            status.elevation = await this._rootCopy() !== null;
            // What to run by hand when nothing can be elevated from here.
            status.command = status.elevation ? '' : `sudo ${GLib.shell_quote(path)} apply`;
            return status;
        } catch (e) {
            if (!this._cancellable.is_cancelled())
                console.warn(`${LOG_PREFIX}: setup helper status failed: ${e.message}`);
            return null;
        }
    }

    async apply() {
        return runAsRoot(await this._rootCopy(), 'apply', this._cancellable);
    }

    // Ends what is running (see runAsRoot() for the one exception) and lets
    // nothing more start.
    destroy() {
        this._cancellable.cancel();
    }
}
