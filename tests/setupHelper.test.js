// SPDX-License-Identifier: GPL-2.0-or-later
// L'assistant système vu de l'extension : gjs -m tests/setupHelper.test.js
// Rien n'est élevé ni changé : les fichiers sont dans un dossier temporaire,
// l'assistant est tools/faux-landline-setup avec un état temporaire, et pkexec
// est le faux de tools/shell-test/bin (placé en tête du PATH avant tout).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {SetupHelper, ownedByRoot} from '../extension/lib/setupHelper.js';
import {run} from '../extension/lib/subprocess.js';

const loop = new GLib.MainLoop(null, false);
let failed = 0;
let count = 0;
function check(label, got, want) {
    count++;
    const a = JSON.stringify(got);
    const b = JSON.stringify(want);
    if (a === b) {
        print(`ok   ${label}`);
    } else {
        failed++;
        print(`FAIL ${label}\n     got  ${a}\n     want ${b}`);
    }
}
const sleep = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const dir = GLib.dir_make_tmp('landline-test-XXXXXX');
const fake = `${here}/../tools/faux-landline-setup`;
const hold = `${dir}/attente`;
const pkexecLog = `${dir}/pkexec.log`;
GLib.setenv('PATH', `${here}/../tools/shell-test/bin:${GLib.getenv('PATH')}`, true);
GLib.setenv('FAUX_SETUP_STATE', `${dir}/reglages.json`, true);
GLib.setenv('FAUX_PKEXEC_HOLD', hold, true);
GLib.setenv('FAUX_PKEXEC_LOG', pkexecLog, true);
const text = path => {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]);
    } catch (e) {
        return '';
    }
};
const alive = pid => GLib.file_test(`/proc/${pid}`, GLib.FileTest.EXISTS);
async function until(test, ms = 5000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (test())
            return true;
        await sleep(20);
    }
    return false;
}

async function main() {
    // --- the check made before anything is handed to pkexec
    check('a program of root that nobody else can write', await ownedByRoot('/usr/bin/env'), true);
    const mine = `${dir}/mine`;
    GLib.file_set_contents(mine, '#!/bin/sh\n');
    GLib.chmod(mine, 0o755);
    check('an executable of the user is refused', await ownedByRoot(mine), false);
    check('a missing file is refused', await ownedByRoot(`${dir}/nothing`), false);
    check('a directory writable by all is refused', await ownedByRoot('/tmp'), false);
    const cancelled = new Gio.Cancellable();
    cancelled.cancel();
    check('a cancelled look is refused', await ownedByRoot('/usr/bin/env', cancelled), false);

    // --- the look does not block: the main loop turns while it is away
    let turns = 0;
    const idle = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
        turns++;
        return GLib.SOURCE_CONTINUE;
    });
    await ownedByRoot('/usr/bin/env');
    GLib.source_remove(idle);
    check('the main loop ran during the look', turns > 0, true);

    // --- status, with the helper of the tests
    const helper = new SetupHelper();
    helper._rootCopy = async () => null;
    helper._statusPath = async () => fake;
    const status = await helper.status();
    check('status: read, nothing can be elevated, command to run by hand',
        [status.ready, status.elevation, status.command], [false, false, `sudo ${GLib.shell_quote(fake)} apply`]);
    check('apply without a helper of root: nothing is run', await helper.apply(), 'unavailable');
    helper._statusPath = async () => null;
    check('no helper at all: no status', await helper.status(), null);

    // --- a process is ended with its cancellable, not only the wait for it
    const pidFile = `${dir}/pid`;
    const cancellable = new Gio.Cancellable();
    let outcome = 'running';
    run(['sh', '-c', `echo $$ > ${pidFile}; exec sleep 30`], cancellable)
        .then(() => (outcome = 'ended'), () => (outcome = 'rejected'));
    await until(() => text(pidFile).trim() !== '');
    const pid = text(pidFile).trim();
    check('a long process is running', [outcome, alive(pid)], ['running', true]);
    cancellable.cancel();
    await until(() => outcome !== 'running', 200);
    check('cancelled: the wait is over at once', outcome, 'rejected');
    check('cancelled: the process is gone', await until(() => !alive(pid), 2000), true);
    let started = true;
    await run(['sh', '-c', `touch ${dir}/started`], cancellable).catch(() => (started = false));
    await sleep(100);
    check('already cancelled: nothing is started', [started, GLib.file_test(`${dir}/started`, GLib.FileTest.EXISTS)], [false, false]);
    const plain = await run(['sh', '-c', 'echo hello; exit 3'], null, true);
    check('exit status and output', [plain.exited, plain.status, plain.stdout], [true, 3, 'hello\n']);

    // --- apply through (the fake) pkexec, then the same with the extension
    // disabled while the password is being asked
    helper._rootCopy = async () => fake;
    helper._statusPath = async () => fake;
    check('before apply: the steadier link is not set up, and that alone is not "ready"',
        (await helper.status()).le_params, false);
    check('apply: done', await helper.apply(), 'done');
    check('apply: the settings are in place', (await helper.status()).ready, true);
    check('apply: the steadier link is set up in the same pass', (await helper.status()).le_params, true);
    GLib.spawn_command_line_sync(`${fake} revert`);
    check('put back for the next check', (await helper.status()).ready, false);
    GLib.file_set_contents(hold, '');
    const lines = () => text(pkexecLog).split('\n').filter(l => l).length;
    const before = lines();
    let result = 'waiting';
    helper.apply().then(r => (result = r));
    await until(() => lines() > before);
    check('pkexec is waiting for the password', result, 'waiting');
    helper.destroy();
    await sleep(100);
    check('disabled meanwhile: the wait is over', result, 'failed');
    GLib.unlink(hold);
    await sleep(600);
    const after = new SetupHelper();
    after._rootCopy = async () => fake;
    after._statusPath = async () => fake;
    check('disabled meanwhile: pkexec was ended, nothing was changed', (await after.status()).ready, false);
    after.destroy();

    // --- after destroy(): nothing is started
    const calls = lines();
    check('destroyed: no status', await helper.status(), null);
    check('destroyed: apply is not run', [await helper.apply(), lines() - calls], ['failed', 0]);
    await sleep(50);
}

main().catch(e => {
    failed++;
    printerr(`${e}\n${e.stack}`);
}).finally(() => {
    GLib.spawn_command_line_sync(`rm -rf ${dir}`);
    print(`\n${count - failed}/${count} checks passed`);
    loop.quit();
});
loop.run();
if (failed > 0)
    imports.system.exit(1);
