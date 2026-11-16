// SPDX-License-Identifier: GPL-2.0-or-later
// The one way this extension runs a program: an argument vector (no shell),
// awaited without blocking, and ended when its cancellable is cancelled.
// No Shell import.

import Gio from 'gi://Gio';

Gio._promisify(Gio.Subprocess.prototype, 'communicate_utf8_async');
Gio._promisify(Gio.Subprocess.prototype, 'wait_async');

// Runs argv and resolves to {exited, status, stdout}: whether it ended by
// itself, its exit status, and what it printed when `capture` is set.
//
// Cancelling does two things: the wait ends at once (the promise rejects),
// and the process is sent SIGKILL. A process that has become root's by then
// (the helper behind pkexec, once the password was accepted) cannot be
// signalled by the user and runs to its end.
export async function run(argv, cancellable = null, capture = false) {
    if (cancellable?.is_cancelled())
        throw new Gio.IOErrorEnum({code: Gio.IOErrorEnum.CANCELLED, message: 'Cancelled'});
    const proc = Gio.Subprocess.new(argv, Gio.SubprocessFlags.STDERR_SILENCE |
        (capture ? Gio.SubprocessFlags.STDOUT_PIPE : Gio.SubprocessFlags.STDOUT_SILENCE));
    const cancelId = cancellable ? cancellable.connect(() => proc.force_exit()) : 0;
    try {
        let stdout = null;
        if (capture)
            [stdout] = await proc.communicate_utf8_async(null, cancellable);
        else
            await proc.wait_async(cancellable);
        const exited = proc.get_if_exited();
        return {exited, status: exited ? proc.get_exit_status() : -1, stdout};
    } finally {
        if (cancelId)
            cancellable.disconnect(cancelId);
    }
}
