// SPDX-License-Identifier: GPL-2.0-or-later
// The rules of "Install the engine", with no import at all: what is
// installed and what to offer, the steps and how they move, and the texts
// written to disk (the user unit, the obex drop-in, the script that runs as
// root). lib/engineInstaller.js does the work; lib/enginePage.js shows it.
// Unit-tested in tests/engineInstall.test.js.

// Where things go, below the home folder (the unit names them with %h).
export const LAYOUT = Object.freeze({
    base: ['.local', 'lib', 'landline'],
    engine: 'engine',
    // The engine before the last update, kept for "Use Previous Engine".
    previous: 'engine.old',
    // The new engine while it is put together, before the switch.
    incoming: 'engine.new',
    // Where the archive is unpacked, then taken apart.
    staging: '.staging',
    daemon: 'landlined',
    cli: 'landlinectl',
    leParams: 'landline-le-params',
    versionFile: 'VERSION',
    // The files of system/ in the archive, kept inside the engine.
    system: 'system',
    setup: 'landline-setup',
    unit: 'landline-engine.service',
    obexDropinDir: 'obex.service.d',
    obexDropin: 'landline.conf',
});

// The fixed directory of root where the system helper goes.
export const ROOT_LIBEXEC = '/usr/local/libexec/landline';

// The steps shown, in order.
export const STEPS = ['download', 'verify', 'install', 'start', 'system', 'phone'];

// -- versions ---------------------------------------------------------------------

// '0.2.0' < '0.10.0'. Anything that is not digits and dots is null.
export function parseVersion(text) {
    const clean = String(text ?? '').trim();
    if (!/^\d+(\.\d+)*$/.test(clean))
        return null;
    return clean.split('.').map(Number);
}

export function compareVersions(a, b) {
    const x = parseVersion(a), y = parseVersion(b);
    if (!x || !y)
        return null;
    for (let i = 0; i < Math.max(x.length, y.length); i++) {
        const d = (x[i] ?? 0) - (y[i] ?? 0);
        if (d !== 0)
            return d < 0 ? -1 : 1;
    }
    return 0;
}

// What is installed, from a few facts read on disk:
//   daemon     the engine's daemon is there
//   version    the text of engine/VERSION, or null when there is none
//   previous   an engine.old is there
//   incoming   an engine.new is there (an install stopped half-way)
//
// state:
//   absent       nothing: offer "Install"
//   interrupted  no engine but a previous one: offer to put it back
//   manual       an engine with no VERSION, put there by
//                scripts/install-local.sh: never replaced without asking
//   outdated     older than the pinned one: offer "Update"
//   current      the pinned one
//   newer        newer than this extension knows: left alone
//   unknown      a VERSION that cannot be read: offer to replace it
export function classify(facts, release) {
    const f = facts ?? {};
    let state;
    if (!f.daemon)
        state = f.previous ? 'interrupted' : 'absent';
    else if (f.version === null || f.version === undefined)
        state = 'manual';
    else {
        const c = compareVersions(f.version, release.version);
        state = c === null ? 'unknown' : c < 0 ? 'outdated' : c === 0 ? 'current' : 'newer';
    }
    const action = {
        absent: 'install',
        interrupted: 'restore',
        manual: 'replace',
        outdated: 'update',
        current: 'reinstall',
        newer: null,
        unknown: 'replace',
    }[state];
    return {
        state,
        action,
        installed: f.daemon === true,
        version: typeof f.version === 'string' ? f.version.trim() : null,
        canRollback: f.previous === true,
        // An install of this window, or one the user has to confirm first.
        needsConfirm: state === 'manual' || state === 'unknown',
    };
}

// The same, said from what the Shell can know cheaply: the text of
// engine/VERSION (null when the file is not there). Used for the menu.
export function shellEngineState(versionText, daemonPresent, release) {
    if (!daemonPresent)
        return {installed: false, outdated: false};
    const c = versionText === null ? null : compareVersions(versionText, release.version);
    return {installed: true, outdated: c !== null && c < 0};
}

// -- checksums ----------------------------------------------------------------------

export function isSha256(text) {
    return /^[0-9a-f]{64}$/.test(String(text ?? ''));
}

// Case and blanks do not count; anything that is not a SHA-256 never matches.
export function sameSum(a, b) {
    const x = String(a ?? '').trim().toLowerCase();
    const y = String(b ?? '').trim().toLowerCase();
    return isSha256(x) && x === y;
}

// The values of this build are placeholders (all zeros): no download can pass.
export function releaseIsPinned(release) {
    return [release.sha256, release.setupSha256, release.leParamsSha256]
        .every(s => isSha256(s) && !/^0+$/.test(s));
}

export function archiveName(release) {
    return `landline-engine-${release.version}-${release.arch}.tar.zst`;
}

export function archiveRoot(release) {
    return `landline-engine-${release.version}`;
}

// -- the steps ----------------------------------------------------------------------

// A step is {state, detail}: state 'pending', 'active', 'done', 'failed',
// 'skipped' or 'waiting' (waits for a click: the password, the iPhone).
// The flow holds the steps, which step failed and why, and the fraction of
// the download.
//
// Failures (flow.error):
//   network     the archive could not be downloaded
//   mismatch    it is not the archive this extension expects
//   contents    it does not hold what an engine release holds
//   unpack      it could not be unpacked (no libgnome-autoar, disk full…)
//   write       a file could not be written in the home folder
//   systemd     systemd did not take the service
//   silent      the new engine did not answer in time
//   cancelled   stopped from the window
//   password    the system step was not allowed (window dismissed)
//   system      the system step failed
export function newFlow() {
    return {
        running: false,
        finished: false,
        steps: Object.fromEntries(STEPS.map(id => [id, {state: 'pending', detail: ''}])),
        failedAt: null,
        error: null,
        detail: '',
        fraction: 0,
        // An engine.old exists to go back to.
        canRollback: false,
        rolledBack: false,
    };
}

function clone(flow) {
    return {...flow, steps: Object.fromEntries(Object.entries(flow.steps).map(([k, v]) => [k, {...v}]))};
}

function set(flow, id, state, detail = '') {
    flow.steps[id] = {state, detail};
}

// The step that is on now, or null.
export function activeStep(flow) {
    return STEPS.find(id => ['active', 'waiting'].includes(flow.steps[id].state)) ?? null;
}

// One event in, the next flow out. Unknown events change nothing.
export function advance(flow, event) {
    const f = clone(flow);
    switch (event.type) {
    case 'begin': {
        const fresh = newFlow();
        fresh.running = true;
        fresh.canRollback = flow.canRollback;
        // A retry from a step keeps what was done before it.
        const from = event.from && STEPS.includes(event.from) ? event.from : 'download';
        for (const id of STEPS.slice(0, STEPS.indexOf(from)))
            fresh.steps[id] = {...flow.steps[id]};
        set(fresh, from, 'active');
        return fresh;
    }
    case 'progress':
        if (f.steps.download.state === 'active')
            f.fraction = event.total > 0 ? Math.min(1, event.done / event.total) : 0;
        return f;
    case 'downloaded':
        set(f, 'download', 'done', event.reused ? 'reused' : '');
        f.fraction = 1;
        set(f, 'verify', 'active');
        return f;
    case 'verified':
        set(f, 'verify', 'done');
        set(f, 'install', 'active');
        return f;
    case 'installed':
        set(f, 'install', 'done');
        f.canRollback = event.previous === true;
        set(f, 'start', 'active');
        return f;
    case 'answered':
        set(f, 'start', 'done');
        set(f, 'system', 'active');
        return f;
    // The system settings: already right, or waiting for the password.
    case 'system-ready':
        set(f, 'system', 'done', 'already');
        set(f, 'phone', 'waiting');
        f.running = false;
        f.finished = true;
        return f;
    case 'system-needed':
        if (event.resume) {
            for (const id of STEPS.slice(0, STEPS.indexOf('system')))
                set(f, id, 'done');
        }
        set(f, 'system', 'waiting', event.manual ? 'manual' : 'password');
        f.running = false;
        return f;
    case 'system-asking':
        set(f, 'system', 'active', 'asking');
        f.error = null;
        f.failedAt = null;
        return f;
    case 'system-result':
        if (event.result === 'done' || event.result === 'partial') {
            set(f, 'system', 'done', event.result);
            set(f, 'phone', 'waiting');
            f.finished = true;
            f.error = null;
            f.failedAt = null;
        } else {
            set(f, 'system', 'failed', event.result);
            f.failedAt = 'system';
            f.error = event.result === 'refused' ? 'password' : 'system';
            f.detail = event.detail ?? '';
        }
        f.running = false;
        return f;
    case 'system-skip':
        set(f, 'system', 'skipped');
        set(f, 'phone', 'waiting');
        f.error = null;
        f.failedAt = null;
        f.running = false;
        f.finished = true;
        return f;
    case 'linked':
        set(f, 'phone', 'done');
        return f;
    case 'failed': {
        const at = activeStep(f) ?? event.step ?? 'download';
        set(f, at, 'failed', event.error);
        f.failedAt = at;
        f.error = event.error;
        f.detail = event.detail ?? '';
        f.running = false;
        return f;
    }
    case 'rolled-back':
        f.rolledBack = true;
        f.canRollback = false;
        f.running = false;
        return f;
    default:
        return f;
    }
}

// What the window offers after a failure, first the one to try first:
//   retry       the same step again
//   rollback    go back to the previous engine
//   manual      show the commands to do it by hand
//   password    ask for the password (the system step)
//   skip        go on without the system settings
export function remedies(flow) {
    if (flow.running)
        return [];
    const offers = [];
    switch (flow.failedAt) {
    case 'download':
        offers.push('retry', 'manual');
        break;
    case 'verify':
        // Another download will not change the sum of the published file
        // when the window pinned another one; it may if it was cut short.
        offers.push('retry', 'manual');
        break;
    case 'install':
        offers.push('retry', 'manual');
        break;
    case 'start':
        if (flow.canRollback)
            offers.push('rollback');
        offers.push('retry', 'manual');
        break;
    case 'system':
        offers.push('password', 'skip', 'manual');
        break;
    default:
        if (flow.steps.system.state === 'waiting')
            offers.push(flow.steps.system.detail === 'manual' ? 'manual' : 'password', 'skip');
        break;
    }
    return offers;
}

// Where "Retry" starts again.
export function retryFrom(flow) {
    switch (flow.failedAt) {
    case 'install':
        // The archive downloaded is checked again before use.
        return 'verify';
    case 'start':
        return 'start';
    case 'system':
        return 'system';
    default:
        return 'download';
    }
}

// -- texts written to disk -------------------------------------------------------

export const WRITTEN_BY = 'Written by the Landline extension (Phone Settings › Engine).';

// The user unit. Same service as system/landline-engine.service (which
// scripts/install-local.sh copies): only the comments differ.
export function unitText() {
    return `# ${WRITTEN_BY}
# Removed by "Uninstall" there. The engine lives in ~/.local/lib/landline/engine.
[Unit]
Description=Landline engine (Bluetooth link to an iPhone)
Documentation=https://github.com/d101c/landline
PartOf=graphical-session.target dbus.service
After=graphical-session.target dbus.socket dbus.service
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
Environment=PATH=%h/.local/lib/landline/engine:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
Environment=LD_LIBRARY_PATH=%h/.local/lib/landline/engine/lib
Environment=LANDLINE_LOCALEDIR=%h/.local/lib/landline/engine/locale
ExecStartPre=-/usr/bin/pkill -u %U -f ^landlined
ExecStart=%h/.local/lib/landline/engine/landlined
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
`;
}

// The drop-in of obex.service: obexd started again when it stops in error
// (same as system/obex-landline.conf).
export function obexDropinText() {
    return `# ${WRITTEN_BY}
# obexd stops in error when it starts before the session is active; it is
# started again, ten times at most in five minutes.
[Unit]
StartLimitIntervalSec=300
StartLimitBurst=10

[Service]
Restart=on-failure
RestartSec=10
TimeoutStopSec=5
`;
}

// Only the lines that count for systemd: no comments, no blank lines.
export function unitDirectives(text) {
    return String(text).split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'));
}

// -- the system step, as root ---------------------------------------------------------

// Exit statuses of the script, besides the helper's own (0, 1, 2) and
// pkexec's (126, 127).
export const BOOTSTRAP_STATUS = Object.freeze({usage: 64, missing: 65, mismatch: 66});

// The script given to `pkexec /bin/sh -c`. It is a constant of this
// version of the extension: the two sums are part of its text, not
// arguments. Its only arguments are the paths of the two files of the
// installed engine; whatever they point to, nothing runs unless the copy
// root made of it has the pinned sum.
//
// 1. copies both files into a new directory of root (mktemp, mode 0700):
//    the user can no longer change what is checked and run;
// 2. checks the SHA-256 of the copies;
// 3. runs the copy of landline-setup with bash (the directory may be on a
//    filesystem mounted noexec): "apply" installs itself and the helper
//    from that directory into /usr/local/libexec/landline, root:root 0755,
//    then changes the Bluetooth settings;
// 4. removes the directory, whatever happened.
export function bootstrapScript(release) {
    for (const sum of [release.setupSha256, release.leParamsSha256]) {
        if (!isSha256(sum))
            throw new Error('the pinned sums are not SHA-256 values');
    }
    return `set -eu
umask 022
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
setup_sum=${release.setupSha256}
helper_sum=${release.leParamsSha256}
[ "$#" -eq 2 ] || exit ${BOOTSTRAP_STATUS.usage}
for f in "$1" "$2"; do
    case "$f" in /*) ;; *) exit ${BOOTSTRAP_STATUS.usage} ;; esac
    [ -f "$f" ] && [ ! -L "$f" ] || { echo "missing: $f" >&2; exit ${BOOTSTRAP_STATUS.missing}; }
done
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
work=$(mktemp -d /run/landline-setup.XXXXXX)
chmod 0700 "$work"
cat -- "$1" > "$work/landline-setup"
cat -- "$2" > "$work/landline-le-params"
echo "$setup_sum  $work/landline-setup" | sha256sum -c --status - ||
    { echo "landline-setup is not the expected file" >&2; exit ${BOOTSTRAP_STATUS.mismatch}; }
echo "$helper_sum  $work/landline-le-params" | sha256sum -c --status - ||
    { echo "landline-le-params is not the expected file" >&2; exit ${BOOTSTRAP_STATUS.mismatch}; }
chmod 0755 "$work/landline-setup" "$work/landline-le-params"
status=0
bash "$work/landline-setup" apply || status=$?
exit "$status"
`;
}

// The arguments after the script: $0 (a name for error messages), $1, $2.
export function bootstrapArgv(script, setupPath, leParamsPath) {
    return ['pkexec', '/bin/sh', '-c', script, 'landline-bootstrap', setupPath, leParamsPath];
}

// pkexec: 126 = dismissed, 127 = not authorized; the helper: 2 = partial.
export function bootstrapResult(status) {
    if (status === 0)
        return 'done';
    if (status === 2)
        return 'partial';
    if (status === 126 || status === 127)
        return 'refused';
    if (status === BOOTSTRAP_STATUS.mismatch)
        return 'mismatch';
    return 'failed';
}

// -- by hand ----------------------------------------------------------------------------
//
// The commands of "Show Steps by Hand", built from the real paths (see
// enginePaths() in lib/engineInstaller.js), each one quoted with `quote`
// (GLib.shell_quote in the window; a home folder may hold a space).

function q(text) {
    return /^[\w@%+=:,./~-]+$/.test(text) ? text : `'${String(text).replace(/'/g, `'\\''`)}'`;
}

const join = (...parts) => parts.join('/');

// One line, so that nothing runs as root when a sum is wrong: both files
// checked against the pinned sums, then the helper run with sudo.
function checkedApply(release, setup, leParams, quote) {
    return `printf '%s  %s\\n' ${release.setupSha256} ${quote(setup)} ${release.leParamsSha256} ${quote(leParams)}` +
        ` | sha256sum -c - && sudo ${quote(setup)} apply`;
}

// The system step by hand. where:
//   {kind: 'pinned', setup, leParams}  the engine of the pinned release,
//       installed by this window: its two files are checked first
//   {kind: 'local', setup}             installed some other way
//       (scripts/install-local.sh, another version): there is no pinned
//       sum to check it against
export function manualSystemCommands(release, where, quote = q) {
    if (where.kind === 'pinned')
        return [checkedApply(release, where.setup, where.leParams, quote)];
    return [
        '# This landline-setup was not installed by this window: no pinned sum to check it against.',
        `sudo ${quote(where.setup)} apply`,
    ];
}

// The same install in a terminal: what "Show Steps by Hand" copies.
// paths: {base, units} of enginePaths().
export function manualCommands(release, paths, quote = q) {
    const file = archiveName(release);
    const root = archiveRoot(release);
    const engine = join(paths.base, LAYOUT.engine);
    const previous = join(paths.base, LAYOUT.previous);
    const incoming = join(paths.base, LAYOUT.incoming);
    const dropinDir = join(paths.units, LAYOUT.obexDropinDir);
    const E = quote(engine), P = quote(previous), N = quote(incoming);
    return [
        'cd "$(mktemp -d)"',
        `curl -fLO ${quote(release.url)}`,
        `echo '${release.sha256}  ${file}' | sha256sum -c -`,
        `tar --zstd -xf ${quote(file)}`,
        `mkdir -p ${quote(paths.base)} ${quote(dropinDir)}`,
        `rm -rf ${N} && cp -r ${root}/engine ${N} && cp -r ${root}/system ${N}/system`,
        // The engine that ran until now is kept, unless it is this very
        // version: then engine.old, the real previous one, stays as it is.
        `if [ -e ${E} ] && [ "$(cat ${quote(join(engine, LAYOUT.versionFile))} 2>/dev/null)" != ${quote(release.version)} ]; ` +
            `then rm -rf ${P} && mv ${E} ${P}; else rm -rf ${E}; fi`,
        `mv ${N} ${E}`,
        `cp ${root}/system/landline-engine.service ${quote(paths.units)}/`,
        `cp ${root}/system/obex-landline.conf ${quote(join(dropinDir, LAYOUT.obexDropin))}`,
        'systemctl --user daemon-reload',
        'systemctl --user enable landline-engine.service',
        'systemctl --user restart landline-engine.service',
        checkedApply(release, join(engine, LAYOUT.system, LAYOUT.setup), join(engine, LAYOUT.leParams), quote),
    ];
}

export function manualRollbackCommands(paths, quote = q) {
    const engine = quote(join(paths.base, LAYOUT.engine));
    const previous = quote(join(paths.base, LAYOUT.previous));
    return [
        'systemctl --user stop landline-engine.service',
        `rm -rf ${engine} && mv ${previous} ${engine}`,
        'systemctl --user restart landline-engine.service',
    ];
}
