// SPDX-License-Identifier: GPL-2.0-or-later
// Les règles de l'installation du moteur, sans rien installer :
//   gjs -m tests/engineInstall.test.js
import GLib from 'gi://GLib';

import {
    STEPS, activeStep, advance, bootstrapArgv, bootstrapResult, bootstrapScript, classify,
    compareVersions, manualCommands, manualRollbackCommands, manualSystemCommands, newFlow, obexDropinText,
    parseVersion, releaseIsPinned,
    remedies, retryFrom, sameSum, shellEngineState, unitDirectives, unitText,
} from '../extension/lib/engineInstall.js';
import {ENGINE_RELEASE} from '../extension/lib/engineRelease.js';

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

const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const read = path => new TextDecoder().decode(GLib.file_get_contents(`${here}/../${path}`)[1]);
const A = 'a'.repeat(64), B = 'b'.repeat(64), C = 'c'.repeat(64);
const release = {version: '0.2.0', arch: 'x86_64', url: 'https://example.invalid/e.tar.zst', sha256: A, setupSha256: B, leParamsSha256: C};

// --- versions
check('versions: numbers, not letters', compareVersions('0.2.0', '0.10.0'), -1);
check('versions: equal with a missing zero', compareVersions('0.2', '0.2.0'), 0);
check('versions: newer', compareVersions('1.0.0', '0.9.9'), 1);
check('versions: a line end does not count', compareVersions('0.2.0\n', '0.2.0'), 0);
check('versions: not a version', [parseVersion('v0.2'), compareVersions('dev', '0.2.0')], [null, null]);

// --- what is installed, and what to offer
const kind = facts => {
    const c = classify(facts, release);
    return [c.state, c.action, c.needsConfirm];
};
check('nothing installed: Install', kind({daemon: false}), ['absent', 'install', false]);
check('engine gone, the previous one there: put it back', kind({daemon: false, previous: true}), ['interrupted', 'restore', false]);
check('installed by hand (no VERSION): Replace, after asking', kind({daemon: true, version: null}), ['manual', 'replace', true]);
check('older: Update', kind({daemon: true, version: '0.1.0'}), ['outdated', 'update', false]);
check('the pinned one: Reinstall only', kind({daemon: true, version: '0.2.0\n'}), ['current', 'reinstall', false]);
check('newer than the extension knows: left alone', kind({daemon: true, version: '0.3.0'}), ['newer', null, false]);
check('a VERSION that makes no sense: Replace, after asking', kind({daemon: true, version: 'garbage'}), ['unknown', 'replace', true]);
check('rollback offered when engine.old is there', classify({daemon: true, version: '0.2.0', previous: true}, release).canRollback, true);
check('the Shell: not installed', shellEngineState(null, false, release), {installed: false, outdated: false});
check('the Shell: by hand is not "outdated"', shellEngineState(null, true, release), {installed: true, outdated: false});
check('the Shell: older is outdated', shellEngineState('0.1.9', true, release), {installed: true, outdated: true});
check('the Shell: current', shellEngineState('0.2.0', true, release), {installed: true, outdated: false});

// --- sums
check('sum: case and blanks do not count', sameSum(`${A.toUpperCase()}\n`, A), true);
check('sum: another one', sameSum(A, B), false);
check('sum: empty never matches', sameSum('', ''), false);
check('sum: not hex never matches', sameSum('z'.repeat(64), 'z'.repeat(64)), false);
check('the pinned release: well formed', [ENGINE_RELEASE.url.endsWith(`landline-engine-${ENGINE_RELEASE.version}-x86_64.tar.zst`),
    /^[0-9a-f]{64}$/.test(ENGINE_RELEASE.sha256), /^[0-9a-f]{64}$/.test(ENGINE_RELEASE.setupSha256),
    /^[0-9a-f]{64}$/.test(ENGINE_RELEASE.leParamsSha256)], [true, true, true, true]);
check('placeholder sums are not a pinned release', releaseIsPinned({...release, sha256: '0'.repeat(64)}), false);
check('real sums are', releaseIsPinned(release), true);

// --- the steps
const run = (events, from = newFlow()) => events.reduce((f, e) => advance(f, e), from);
const states = f => STEPS.map(id => f.steps[id].state);
let f = run([{type: 'begin'}]);
check('begin: download on', [states(f), f.running, activeStep(f)],
    [['active', 'pending', 'pending', 'pending', 'pending', 'pending'], true, 'download']);
f = run([{type: 'progress', done: 50, total: 200}], f);
check('progress: a quarter', f.fraction, 0.25);
check('no remedy while running', remedies(f), []);
f = run([{type: 'downloaded'}, {type: 'verified'}, {type: 'installed', previous: true}, {type: 'answered'}], f);
check('through to the system step', [states(f), f.canRollback],
    [['done', 'done', 'done', 'done', 'active', 'pending'], true]);
const ready = run([{type: 'system-ready'}], f);
check('settings already right: straight to the iPhone', [states(ready), ready.finished, ready.steps.system.detail],
    [['done', 'done', 'done', 'done', 'done', 'waiting'], true, 'already']);
const needed = run([{type: 'system-needed', manual: false}], f);
check('settings to change: waits for the password', [states(needed), needed.running, remedies(needed)],
    [['done', 'done', 'done', 'done', 'waiting', 'pending'], false, ['password', 'skip']]);
const byHand = run([{type: 'system-needed', manual: true}], f);
check('engine by hand: the command, not the password', remedies(byHand), ['manual', 'skip']);
const refused = run([{type: 'system-asking'}, {type: 'system-result', result: 'refused'}], needed);
check('password window dismissed: nothing broken, asked again or skipped',
    [refused.steps.system.state, refused.error, remedies(refused), retryFrom(refused)],
    ['failed', 'password', ['password', 'skip', 'manual'], 'system']);
const partial = run([{type: 'system-asking'}, {type: 'system-result', result: 'partial'}], needed);
check('partial still goes on', [partial.steps.system.state, partial.finished], ['done', true]);
const skipped = run([{type: 'system-skip'}], refused);
check('skip: the iPhone next, no error left', [skipped.steps.system.state, skipped.steps.phone.state, skipped.error],
    ['skipped', 'waiting', null]);
check('linked: last step done', run([{type: 'linked'}], skipped).steps.phone.state, 'done');
const resumed = run([{type: 'system-needed', manual: false, resume: true}]);
check('engine installed earlier: only the system step left', states(resumed),
    ['done', 'done', 'done', 'done', 'waiting', 'pending']);

// failures, and where each one goes on
const failAt = (events, error) => run([{type: 'begin'}, ...events, {type: 'failed', error}]);
let x = failAt([], 'network');
check('download failed: retry or by hand', [x.failedAt, remedies(x), retryFrom(x)], ['download', ['retry', 'manual'], 'download']);
x = failAt([{type: 'downloaded'}], 'mismatch');
check('wrong sum: at the check, downloaded afresh', [x.failedAt, x.error, retryFrom(x)], ['verify', 'mismatch', 'download']);
x = failAt([{type: 'downloaded'}, {type: 'verified'}], 'unpack');
check('unpack failed: checked again, then installed', [x.failedAt, retryFrom(x)], ['install', 'verify']);
x = failAt([{type: 'downloaded'}, {type: 'verified'}, {type: 'installed', previous: true}], 'silent');
check('silent new engine: going back first', [x.failedAt, remedies(x), retryFrom(x)], ['start', ['rollback', 'retry', 'manual'], 'start']);
x = failAt([{type: 'downloaded'}, {type: 'verified'}, {type: 'installed', previous: false}], 'silent');
check('silent first engine: nothing to go back to', remedies(x), ['retry', 'manual']);
const again = advance(x, {type: 'begin', from: 'start'});
check('retry from start keeps the steps before', states(again), ['done', 'done', 'done', 'active', 'pending', 'pending']);
const back = advance(x, {type: 'rolled-back'});
check('rolled back: said, not offered again', [back.rolledBack, back.canRollback], [true, false]);

// --- the unit: the same service as system/landline-engine.service
check('unit: same directives as the one install-local.sh copies',
    unitDirectives(unitText()), unitDirectives(read('system/landline-engine.service')));
check('obex drop-in: same directives as system/obex-landline.conf',
    unitDirectives(obexDropinText()), unitDirectives(read('system/obex-landline.conf')));
check('unit: says who wrote it', unitText().startsWith('# Written by the Landline extension'), true);

// --- the script run as root
const script = bootstrapScript(release);
check('script: the two sums are in its text', [script.includes(`setup_sum=${B}`), script.includes(`helper_sum=${C}`)], [true, true]);
check('script: the same each time (a constant of this version)', bootstrapScript(release), script);
check('script: no sum of the archive, no URL, no home folder',
    [script.includes(A), /https?:/.test(script), /home|\$HOME|~/.test(script)], [false, false, false]);
check('script: checks the copy, not the file the user can change',
    script.indexOf('cat -- "$1" > "$work/landline-setup"') < script.indexOf('sha256sum -c'), true);
check('script: the copy of root runs apply, nothing else',
    script.match(/landline-setup" apply/g)?.length, 1);
check('script: a sum that is not SHA-256 is refused', (() => {
    try {
        bootstrapScript({...release, setupSha256: 'x; rm -rf /'});
        return 'built';
    } catch (e) {
        return 'refused';
    }
})(), 'refused');
check('argv: pkexec, /bin/sh -c, the script, then only the two paths',
    bootstrapArgv(script, '/h/s', '/h/l'), ['pkexec', '/bin/sh', '-c', script, 'landline-bootstrap', '/h/s', '/h/l']);
check('results', [0, 2, 126, 127, 66, 1, 64].map(bootstrapResult),
    ['done', 'partial', 'refused', 'refused', 'mismatch', 'failed', 'failed']);

// --- by hand
const lines = manualCommands(release, {base: '/home/a b/.local/lib/landline', units: '/home/a b/.config/systemd/user'});
check('by hand: the sum is checked before anything is unpacked',
    lines.findIndex(l => l.includes('sha256sum -c')) < lines.findIndex(l => l.startsWith('tar ')), true);
check('by hand: ends with the system step, both files checked before sudo, paths quoted', lines.at(-1),
    `printf '%s  %s\\n' ${B} '/home/a b/.local/lib/landline/engine/system/landline-setup' ` +
    `${C} '/home/a b/.local/lib/landline/engine/landline-le-params' | sha256sum -c - && ` +
    "sudo '/home/a b/.local/lib/landline/engine/system/landline-setup' apply");
check('by hand: no path left unquoted', lines.filter(l => / \/home\/a b/.test(l)), []);
check('by hand, system step of an engine installed some other way: said, then sudo',
    manualSystemCommands(release, {kind: 'local', setup: "/home/o'k/.local/lib/landline/landline-setup"}).at(-1),
    "sudo '/home/o'\\''k/.local/lib/landline/landline-setup' apply");
check('by hand, going back: quoted', manualRollbackCommands({base: '/home/a b/.local/lib/landline'})[1],
    "rm -rf '/home/a b/.local/lib/landline/engine' && mv '/home/a b/.local/lib/landline/engine.old' '/home/a b/.local/lib/landline/engine'");

print(failed ? `\n${failed}/${count} checks FAILED` : `\n${count}/${count} checks passed`);
if (failed)
    imports.system.exit(1);
