// SPDX-License-Identifier: GPL-2.0-or-later
// L'installation du moteur pour de vrai, mais dans un dossier temporaire :
//   gjs -m tests/engineInstaller.test.js
//
// Une archive factice (tar --zstd) est servie par « python3 -m http.server »
// sur 127.0.0.1 ; HOME, ~/.config, ~/.cache et le dossier d'exécution sont
// temporaires ; systemd est une doublure qui note les appels (le vrai n'est
// jamais appelé) ; le moteur qui « répond » est soit une doublure, soit
// tools/faux-moteur.py sur un socket temporaire. Le script racine est joué
// avec /bin/sh, sans pkexec, son dossier de travail déplacé de /run vers le
// dossier temporaire et son assistant remplacé par un faux.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {
    LAYOUT, bootstrapScript, classify, manualCommands, manualSystemCommands, obexDropinText, remedies, unitText,
} from '../extension/lib/engineInstall.js';
import {
    EngineInstaller, SystemdUser, enginePaths, hasSession, manualSystemWhere, probeEngine, readFacts, sha256File,
} from '../extension/lib/engineInstaller.js';

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
const repo = GLib.path_get_dirname(here);
const dir = GLib.dir_make_tmp('ll-inst-XXXXXX');
const home = `${dir}/home`;
const serve = `${dir}/serve`;
const build = `${dir}/build`;
const exists = path => GLib.file_test(path, GLib.FileTest.EXISTS);
const text = path => {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]);
    } catch (e) {
        return null;
    }
};
const write = (path, content, mode = 0o644) => {
    GLib.mkdir_with_parents(GLib.path_get_dirname(path), 0o755);
    GLib.file_set_contents(path, content);
    GLib.chmod(path, mode);
};
function sh(argv, env = {}) {
    const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE});
    for (const [k, v] of Object.entries(env))
        launcher.setenv(k, v, true);
    const proc = launcher.spawnv(argv);
    const [, out, err] = proc.communicate_utf8(null, null);
    return {status: proc.get_exit_status(), out, err};
}

// A release laid out like the real one: engine/ and system/.
async function makeArchive(name, version, {engineVersion = version, setupBody = null} = {}) {
    const root = `${build}/${name}/landline-engine-${version}`;
    const e = `${root}/engine`, s = `${root}/system`;
    write(`${e}/landlined`, '#!/bin/sh\nexit 0\n', 0o755);
    write(`${e}/landlinectl`, '#!/bin/sh\nexit 0\n', 0o755);
    write(`${e}/landline-le-params`, '\x7fELF fake helper\n', 0o755);
    write(`${e}/lib/libfake.so.1`, 'lib\n');
    write(`${e}/locale/fr/LC_MESSAGES/landline.mo`, 'mo\n');
    write(`${e}/LICENSE`, 'MIT\n');
    write(`${e}/VERSION`, `${engineVersion}\n`);
    // The fake setup helper: "status" says all is right when $FAKE_READY
    // exists; "apply" writes its arguments and where it ran from.
    write(`${s}/landline-setup`, setupBody ?? `#!/bin/bash
case "$1" in
status) if [ -e "\${FAKE_READY:-/nonexistent}" ]; then r=true; else r=false; fi
    echo "{\\"ready\\":$r,\\"le_params\\":$r}" ;;
apply) echo "apply from $(dirname "\${BASH_SOURCE[0]}") helper=$(cat "$(dirname "\${BASH_SOURCE[0]}")/landline-le-params")" >> "$FAKE_APPLIED"; exit 0 ;;
esac
`, 0o644);
    write(`${s}/io.github.d101c.landline.policy`, '<policyconfig/>\n');
    write(`${s}/landline-engine.service`, unitText());
    write(`${s}/obex-landline.conf`, obexDropinText());
    GLib.mkdir_with_parents(serve, 0o755);
    const file = `${serve}/landline-engine-${version}-x86_64.tar.zst`;
    const r = sh(['tar', '--zstd', '-C', `${build}/${name}`, '-cf', file, `landline-engine-${version}`]);
    if (r.status !== 0)
        throw new Error(`tar: ${r.err}`);
    return {
        file,
        sha256: await sha256File(file),
        setupSha256: await sha256File(`${s}/landline-setup`),
        leParamsSha256: await sha256File(`${e}/landline-le-params`),
    };
}

// The unit's main process: RestartUnit starts a new one (another MainPID,
// another start time) unless `starts` is false, where the old one stays
// the unit's (the job did not get through).
class FakeSystemd {
    constructor() {
        this.calls = [];
        this.fail = null;
        this.starts = true;
        this.state = {active: 'inactive', pid: 0, started: 0};
        this._nextPid = 1000;
    }

    async unitState() {
        return {...this.state};
    }

    _note(name, unit = '') {
        this.calls.push(unit ? `${name} ${unit}` : name);
        if (this.fail === name)
            throw new Error(`${name} refused`);
    }

    async reload() {
        this._note('reload');
    }

    async enable(u) {
        this._note('enable', u);
    }

    async disable(u) {
        this._note('disable', u);
    }

    async restart(u) {
        this._note('restart', u);
        if (this.starts)
            this.state = {active: 'active', pid: this._nextPid++, started: this.state.started + 1000};
    }

    async stop(u) {
        this._note('stop', u);
    }

    async resetFailed(u) {
        this._note('reset-failed', u);
    }

    async activeState() {
        return 'active';
    }
}

async function main() {
    const port = 20000 + Math.floor(Math.random() * 20000);
    GLib.mkdir_with_parents(serve, 0o755);
    const server = Gio.Subprocess.new(['python3', '-I', '-m', 'http.server', '--bind', '127.0.0.1', '--directory', serve, String(port)],
        Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
    try {
        await sleep(600);
        await scenarios(`http://127.0.0.1:${port}`);
    } finally {
        server.force_exit();
    }
}

async function scenarios(origin) {
    const good = await makeArchive('good', '0.2.0');
    const release = {version: '0.2.0', arch: 'x86_64', url: `${origin}/landline-engine-0.2.0-x86_64.tar.zst`,
        sha256: good.sha256, setupSha256: good.setupSha256, leParamsSha256: good.leParamsSha256};
    const runtime = `${dir}/run`;
    GLib.mkdir_with_parents(`${runtime}/landline`, 0o700);
    const paths = enginePaths({home, configDir: `${home}/.config`, cacheDir: `${home}/.cache`, runtimeDir: runtime});
    let answers = true;
    const probe = async () => answers;
    const systemd = new FakeSystemd();
    const make = (r = release, extra = {}) => new EngineInstaller({release: r, paths, systemd, probe,
        startTimeoutMs: 300, probeEveryMs: 50, ...extra});

    // --- the sum does not match: nothing is installed, the file is not kept
    let inst = make({...release, sha256: 'f'.repeat(64)});
    let progress = 0;
    inst.onChange = f => (progress = Math.max(progress, f.fraction));
    let flow = await inst.run();
    check('wrong sum: stops at the check', [flow.failedAt, flow.error], ['verify', 'mismatch']);
    check('wrong sum: the download progressed to the end first', progress, 1);
    check('wrong sum: nothing installed, download thrown away',
        [exists(paths.engine), exists(paths.staging), exists(`${paths.downloads}/landline-engine-0.2.0-x86_64.tar.zst`)],
        [false, false, false]);
    check('wrong sum: systemd not asked', systemd.calls, []);

    // --- the network is down
    inst = make({...release, url: 'http://127.0.0.1:9/nothing.tar.zst'});
    flow = await inst.run();
    check('no network: said at the download', [flow.failedAt, flow.error], ['download', 'network']);

    // --- a first install
    inst = make();
    flow = await inst.run();
    check('first install: through to the system step', [flow.failedAt, flow.steps.start.state, flow.steps.system.state],
        [null, 'done', 'active']);
    check('first install: engine in place', [text(paths.versionFile), exists(paths.daemon),
        GLib.file_test(paths.daemon, GLib.FileTest.IS_EXECUTABLE), exists(`${paths.engine}/lib/libfake.so.1`)],
    ['0.2.0\n', true, true, true]);
    check('first install: system files kept inside the engine, helper executable',
        [exists(`${paths.engine}/system/io.github.d101c.landline.policy`),
            GLib.file_test(paths.setup, GLib.FileTest.IS_EXECUTABLE)], [true, true]);
    check('first install: unit and obex drop-in written', [text(paths.unit) === unitText(), text(paths.obexDropin) === obexDropinText()],
        [true, true]);
    check('first install: unit where systemd looks', paths.unit, `${home}/.config/systemd/user/landline-engine.service`);
    check('first install: systemd asked through its interface only', systemd.calls,
        ['reload', 'enable landline-engine.service', 'reset-failed landline-engine.service', 'restart landline-engine.service']);
    check('first install: nothing left over', [exists(paths.staging), exists(paths.incoming), exists(paths.previous),
        exists(`${paths.downloads}/landline-engine-0.2.0-x86_64.tar.zst`)], [false, false, false, false]);
    check('first install: nothing to go back to', flow.canRollback, false);
    check('first install: now current', classify(readFacts(paths), release).state, 'current');
    check('soup session: held after a download, let go by destroy()', [hasSession(), (inst.destroy(), hasSession())],
        [true, false]);

    // --- the script run as root, played without root
    const work = `${dir}/bootstrap`;
    GLib.mkdir_with_parents(work, 0o755);
    const script = bootstrapScript(release).replace('/run/landline-setup.XXXXXX', `${work}/landline-setup.XXXXXX`);
    const applied = `${dir}/applied`;
    let r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', paths.setup, paths.leParams], {FAKE_APPLIED: applied});
    check('root script: right files, apply runs from the copy', [r.status, (text(applied) ?? '').includes(`apply from ${work}/landline-setup.`),
        (text(applied) ?? '').includes('helper=\x7fELF fake helper')], [0, true, true]);
    check('root script: its working copy is removed', sh(['ls', '-A', work]).out.trim(), '');
    GLib.unlink(applied);
    const tampered = `${dir}/tampered-setup`;
    write(tampered, `${text(paths.setup)}\necho owned\n`, 0o755);
    r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', tampered, paths.leParams], {FAKE_APPLIED: applied});
    check('root script: a changed landline-setup is refused, nothing runs', [r.status, exists(applied)], [66, false]);
    const otherHelper = `${dir}/other-helper`;
    write(otherHelper, 'other\n', 0o755);
    r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', paths.setup, otherHelper], {FAKE_APPLIED: applied});
    check('root script: a changed helper is refused, nothing runs', [r.status, exists(applied)], [66, false]);
    r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', paths.setup], {FAKE_APPLIED: applied});
    check('root script: two paths or nothing', r.status, 64);
    r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', 'relative/setup', paths.leParams], {FAKE_APPLIED: applied});
    check('root script: only absolute paths', r.status, 64);
    const link = `${dir}/link-setup`;
    Gio.File.new_for_path(link).make_symbolic_link(paths.setup, null);
    r = sh(['/bin/sh', '-c', script, 'landline-bootstrap', link, paths.leParams], {FAKE_APPLIED: applied});
    check('root script: a symbolic link is not followed', [r.status, exists(applied)], [65, false]);
    check('root script: no working copy left after a refusal', sh(['ls', '-A', work]).out.trim(), '');

    // --- an update from an older engine
    write(paths.versionFile, '0.1.0\n');
    write(`${paths.engine}/old-marker`, 'old\n');
    check('older engine: Update offered', classify(readFacts(paths), release).action, 'update');
    systemd.calls = [];
    inst = make();
    flow = await inst.run();
    check('update: done, the old one kept beside it', [flow.steps.start.state, text(paths.versionFile),
        text(`${paths.previous}/VERSION`), exists(`${paths.previous}/old-marker`), flow.canRollback],
    ['done', '0.2.0\n', '0.1.0\n', true, true]);

    // --- Reinstall of the version already there: engine.old, the real
    // previous engine, is kept as it is (#26)
    inst = make();
    flow = await inst.run();
    check('reinstall: done, the previous engine untouched, still offered', [flow.steps.start.state, text(paths.versionFile),
        text(`${paths.previous}/VERSION`), exists(`${paths.previous}/old-marker`), flow.canRollback],
    ['done', '0.2.0\n', '0.1.0\n', true, true]);
    check('reinstall: nothing left beside', [exists(paths.staging), exists(paths.incoming)], [false, false]);
    // ...and a retry from the archive after a failure that came after the switch.
    systemd.fail = 'restart';
    flow = await make().run();
    systemd.fail = null;
    flow = await make().run('verify');
    check('retry after the switch: the previous engine untouched', [flow.steps.start.state,
        text(`${paths.previous}/VERSION`), exists(`${paths.previous}/old-marker`)], ['done', '0.1.0\n', true]);

    // --- while something runs, no uninstall and no rollback (#24)
    inst = make();
    const running = inst.run();
    check('during an install: busy', [inst.flow.running, inst.busy], [true, true]);
    check('during an install: uninstall refused', await inst.uninstall(), false);
    check('during an install: rollback refused', await inst.rollback(), false);
    check('during an install: a second run refused', (await inst.run()).running, true);
    flow = await running;
    check('during an install: the install went through, engine and previous in place',
        [flow.steps.start.state, text(paths.versionFile), exists(`${paths.previous}/old-marker`)], ['done', '0.2.0\n', true]);
    inst = make();
    inst.systemAsking();
    check('during the password step: uninstall and rollback refused',
        [inst.busy, await inst.uninstall(), await inst.rollback(), exists(paths.daemon)], [true, false, false, true]);
    inst = make();
    write(paths.versionFile, '0.1.0\n');
    const back = inst.rollback();
    check('during a rollback: uninstall and an install refused', [inst.busy, await inst.uninstall(), (await inst.run()).running],
        [true, false, false]);
    await back;
    check('during a rollback: it went through alone', [exists(paths.daemon), exists(paths.previous)], [true, false]);
    // Back to: 0.2.0 installed, 0.1.0 (with its marker) beside it.
    GLib.spawn_command_line_sync(`mv ${paths.engine} ${paths.previous}`);
    write(`${paths.previous}/old-marker`, 'old\n');
    flow = await make().run();
    check('set up again: 0.2.0 over 0.1.0', [text(paths.versionFile), text(`${paths.previous}/VERSION`)], ['0.2.0\n', '0.1.0\n']);

    // --- the old engine still answers, the new one never starts: "Start"
    // fails and going back is offered (#23)
    write(paths.versionFile, '0.1.0\n');   // the current one plays the old one
    systemd.starts = false;
    systemd.state = {active: 'active', pid: 42, started: 5000};
    answers = true;
    flow = await make().run();
    check('old engine answering, new one not started: Start fails, going back offered',
        [flow.failedAt, flow.error, flow.canRollback, remedies(flow)[0]], ['start', 'silent', true, 'rollback']);
    check('old engine answering: the archive is kept for a retry',
        exists(`${paths.downloads}/landline-engine-0.2.0-x86_64.tar.zst`), true);
    systemd.starts = true;
    // A new process that is active but then replaced at once (it crashed and
    // systemd restarted it) does not pass either while it keeps changing.
    const flaky = new FakeSystemd();
    flaky.state = {active: 'active', pid: 42, started: 5000};
    let reads = 0;
    flaky.unitState = async () => ({active: 'active', pid: 100 + reads, started: 6000 + reads++});
    flow = await make(release, {systemd: flaky}).run('start');
    check('a new process that never stays: Start fails', [flow.failedAt, flow.error], ['start', 'silent']);
    flow = await make().run('start');
    check('the new one starts: Start passes', [flow.failedAt, flow.steps.start.state], [null, 'done']);

    // --- the new engine stays silent: back to the previous one
    write(paths.versionFile, '0.1.0\n');   // the current one plays the old one
    answers = false;
    systemd.calls = [];
    inst = make();
    flow = await inst.run();
    check('silent new engine: start failed, going back offered', [flow.failedAt, flow.error, flow.canRollback],
        ['start', 'silent', true]);
    check('silent new engine: the archive is kept for a retry', exists(`${paths.downloads}/landline-engine-0.2.0-x86_64.tar.zst`), true);
    answers = true;
    systemd.calls = [];
    check('rollback: done', await inst.rollback(), true);
    check('rollback: the previous engine is back, the new one gone',
        [text(paths.versionFile), exists(paths.previous), exists(paths.incoming), inst.flow.rolledBack],
        ['0.1.0\n', false, false, true]);
    check('rollback: stopped, then started again', systemd.calls,
        ['stop landline-engine.service', 'reload', 'reset-failed landline-engine.service', 'restart landline-engine.service']);
    check('rollback: nothing more to go back to', await inst.rollback(), false);

    // --- retry from "start" after systemd refused
    systemd.fail = 'restart';
    inst = make();
    flow = await inst.run('verify');
    check('systemd refuses: said at the start step', [flow.failedAt, flow.error], ['start', 'systemd']);
    systemd.fail = null;
    flow = await inst.run('start');
    check('retry from start: goes on without installing again', [flow.failedAt, flow.steps.install.state, flow.steps.start.state],
        [null, 'done', 'done']);

    // --- an engine installed by hand (scripts/install-local.sh: no VERSION)
    GLib.unlink(paths.versionFile);
    check('installed by hand: recognized, replaced only after asking', [classify(readFacts(paths), release).state,
        classify(readFacts(paths), release).needsConfirm], ['manual', true]);

    // --- an update stopped half-way: no engine, the previous one there
    GLib.spawn_command_line_sync(`rm -rf ${paths.previous}`);
    Gio.File.new_for_path(paths.engine).move(Gio.File.new_for_path(paths.previous), Gio.FileCopyFlags.NONE, null, null);
    write(`${paths.previous}/VERSION`, '0.1.0\n');
    check('interrupted: put back offered', classify(readFacts(paths), release).action, 'restore');
    await make().rollback();
    check('interrupted: put back', [text(paths.versionFile), exists(paths.previous)], ['0.1.0\n', false]);

    // --- an archive that is not an engine (its sum pinned all the same)
    const bad = await makeArchive('bad', '0.2.0', {engineVersion: '9.9.9'});
    inst = make({...release, sha256: bad.sha256});
    flow = await inst.run();
    check('archive with the wrong engine: refused at install, nothing switched',
        [flow.failedAt, flow.error, text(paths.versionFile), exists(paths.staging)], ['install', 'contents', '0.1.0\n', false]);
    // ...and one whose setup helper is not the pinned one.
    const odd = await makeArchive('odd', '0.2.0', {setupBody: '#!/bin/sh\necho other\n'});
    inst = make({...release, sha256: odd.sha256});
    flow = await inst.run();
    check('archive with another landline-setup: refused before the switch',
        [flow.failedAt, flow.error, text(paths.versionFile)], ['install', 'mismatch', '0.1.0\n']);

    // --- the real probe, against the fake engine
    check('probe: nobody there', await probeEngine(`${runtime}/landline/landlined.sock`, 500), false);
    const sock = `${runtime}/landline/landlined.sock`;
    const fake = Gio.Subprocess.new(['python3', `${repo}/tools/faux-moteur.py`, '--socket', sock],
        Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
    for (let i = 0; i < 50 && !exists(sock); i++)
        await sleep(100);
    check('probe: the engine answers protocol_info', await probeEngine(sock, 2000), true);
    fake.force_exit();

    // --- the commands by hand, run for real in bash against the local
    // server, in a home folder with a space; sudo and systemctl are
    // stand-ins that note their arguments (#27)
    const again = await makeArchive('good-again', '0.2.0');
    await commandsByHand({...release, ...again}, runtime);

    // --- uninstall: the engine and what was written for it, not the data
    write(`${home}/.config/landline/bluetooth.json`, '{}\n');
    write(paths.localSetup, '#!/bin/sh\n', 0o755);
    write(paths.unit, unitText());
    write(paths.obexDropin, obexDropinText());
    systemd.calls = [];
    inst = make();
    await inst.uninstall();
    check('uninstall: stopped, disabled, reloaded', systemd.calls,
        ['stop landline-engine.service', 'disable landline-engine.service', 'reload']);
    check('uninstall: engine, unit, drop-in, helper copy removed',
        [exists(paths.engine), exists(paths.previous), exists(paths.unit), exists(paths.obexDropin),
            exists(paths.obexDropinDir), exists(paths.localSetup), exists(paths.base)],
        [false, false, false, false, false, false, false]);
    check('uninstall: the pairing and settings stay', exists(`${home}/.config/landline/bluetooth.json`), true);
    check('uninstall: then Install is offered', classify(readFacts(paths), release).state, 'absent');
    check('layout: names as the unit and install-local.sh use them', [LAYOUT.unit, LAYOUT.daemon], ['landline-engine.service', 'landlined']);
}

async function commandsByHand(release, runtime) {
    const spaced = `${dir}/my home`;
    const p = enginePaths({home: spaced, configDir: `${spaced}/.config`, cacheDir: `${spaced}/.cache`, runtimeDir: runtime});
    const bin = `${dir}/fakebin`;
    const sudoLog = `${dir}/sudo.log`;
    write(`${bin}/sudo`, '#!/bin/sh\nprintf \'%s\\n\' "$@" > "$SUDO_LOG"\n', 0o755);
    write(`${bin}/systemctl`, '#!/bin/sh\nexit 0\n', 0o755);
    const env = {PATH: `${bin}:${GLib.getenv('PATH')}`, SUDO_LOG: sudoLog, TMPDIR: dir};
    const bash = lines => sh(['bash', '-e', '-c', lines.join('\n')], env);
    const sudoArgs = () => {
        const t = text(sudoLog);
        try {
            GLib.unlink(sudoLog);
        } catch (e) {
            // Not called.
        }
        return t === null ? null : t.trimEnd().split('\n');
    };

    let lines = manualCommands(release, p, GLib.shell_quote);
    let r = bash(lines);
    check('by hand: the whole install runs, its sum checked', [r.status, r.out.includes(': OK')], [0, true]);
    check('by hand: engine, unit and drop-in in place, in a home with a space',
        [text(p.versionFile), text(p.unit) !== null, text(p.obexDropin) !== null, exists(p.previous)],
        ['0.2.0\n', true, true, false]);
    check('by hand: sudo runs the one-click landline-setup, after the check', sudoArgs(), [p.setup, 'apply']);
    // An older engine is kept as engine.old; a second run of the same
    // version leaves that one alone.
    write(p.versionFile, '0.1.0\n');
    write(`${p.engine}/old-marker`, 'old\n');
    r = bash(lines);
    check('by hand, update: the older engine kept', [r.status, text(p.versionFile), exists(`${p.previous}/old-marker`)],
        [0, '0.2.0\n', true]);
    r = bash(lines);
    check('by hand, same version again: engine.old untouched', [r.status, exists(`${p.previous}/old-marker`),
        text(`${p.previous}/VERSION`)], [0, true, '0.1.0\n']);
    sudoArgs();

    // The system step alone.
    let where = manualSystemWhere(p, release);
    check('system by hand: the pinned engine, both files', where, {kind: 'pinned', setup: p.setup, leParams: p.leParams});
    lines = manualSystemCommands(release, where, GLib.shell_quote);
    check('system by hand: one line, the check before sudo, paths quoted', [lines.length,
        lines[0].indexOf('sha256sum -c') < lines[0].indexOf('sudo'), lines[0].includes(GLib.shell_quote(p.setup))],
    [1, true, true]);
    r = bash(lines);
    check('system by hand: right files, sudo called', [r.status, sudoArgs()], [0, [p.setup, 'apply']]);
    const helper = text(p.leParams);
    write(p.leParams, `${helper}x`, 0o755);
    r = bash(lines);
    check('system by hand: a changed helper, sudo never called', [r.status !== 0, sudoArgs()], [true, null]);
    write(p.leParams, helper, 0o755);
    write(p.setup, `${text(p.setup)}\necho owned\n`, 0o755);
    r = bash(lines);
    check('system by hand: a changed landline-setup, sudo never called', [r.status !== 0, sudoArgs()], [true, null]);

    // An engine of scripts/install-local.sh: its landline-setup is beside
    // the engine, with no pinned sum.
    GLib.unlink(p.versionFile);
    write(p.localSetup, '#!/bin/sh\n', 0o755);
    where = manualSystemWhere(p, release);
    check('install-local: its own landline-setup', where, {kind: 'local', setup: p.localSetup});
    lines = manualSystemCommands(release, where, GLib.shell_quote);
    r = bash(lines);
    check('install-local: sudo runs it, path quoted', [r.status, lines.at(-1), sudoArgs()],
        [0, `sudo ${GLib.shell_quote(p.localSetup)} apply`, [p.localSetup, 'apply']]);
    GLib.spawn_command_line_sync(`rm -rf ${GLib.shell_quote(spaced)}`);
    check('no landline-setup at all: the whole install instead', manualSystemWhere(p, release), null);
}

// SystemdUser against a stand-in of systemd's Manager, on a private
// peer-to-peer D-Bus connection: the calls and their signatures, as the
// real one would get them. The unit is an object of its own with the
// properties of org.freedesktop.systemd1.Unit and .Service that the
// installer reads, answered through org.freedesktop.DBus.Properties.
const MANAGER_XML = `<node><interface name="org.freedesktop.systemd1.Manager">
<method name="Reload"/>
<method name="EnableUnitFiles"><arg type="as" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="out"/><arg type="a(sss)" direction="out"/></method>
<method name="DisableUnitFiles"><arg type="as" direction="in"/><arg type="b" direction="in"/><arg type="a(sss)" direction="out"/></method>
<method name="RestartUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="StopUnit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="ResetFailedUnit"><arg type="s" direction="in"/></method>
<method name="LoadUnit"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
</interface></node>`;
const UNIT_XML = `<node><interface name="org.freedesktop.systemd1.Unit">
<property name="ActiveState" type="s" access="read"/>
</interface><interface name="org.freedesktop.systemd1.Service">
<property name="MainPID" type="u" access="read"/>
<property name="ExecMainStartTimestampMonotonic" type="t" access="read"/>
</interface></node>`;
const UNIT_PATH = '/org/freedesktop/systemd1/unit/landline_2dengine_2eservice';

async function systemdOverDBus() {
    const guid = Gio.dbus_generate_guid();
    const server = Gio.DBusServer.new_sync(`unix:dir=${dir}`, Gio.DBusServerFlags.NONE, guid, null, null);
    const calls = [];
    // The old engine runs; RestartUnit starts a new one only when
    // `restartStarts` (otherwise the job is queued and nothing changes yet).
    const unit = {active: 'active', pid: 4242, started: 5000000, restartStarts: true};
    server.connect('new-connection', (_s, connection) => {
        const info = Gio.DBusNodeInfo.new_for_xml(MANAGER_XML).interfaces[0];
        connection.register_object('/org/freedesktop/systemd1', info, (conn, sender, path, iface, method, params, invocation) => {
            calls.push([method, params.deepUnpack()]);
            if (method === 'RestartUnit' && unit.restartStarts)
                Object.assign(unit, {active: 'active', pid: unit.pid + 1, started: unit.started + 1000000});
            const replies = {
                EnableUnitFiles: new GLib.Variant('(ba(sss))', [true, []]),
                DisableUnitFiles: new GLib.Variant('(a(sss))', [[]]),
                RestartUnit: new GLib.Variant('(o)', ['/org/freedesktop/systemd1/job/1']),
                StopUnit: new GLib.Variant('(o)', ['/org/freedesktop/systemd1/job/2']),
                LoadUnit: new GLib.Variant('(o)', [UNIT_PATH]),
            };
            invocation.return_value(replies[method] ?? null);
        }, null, null);
        for (const iface of Gio.DBusNodeInfo.new_for_xml(UNIT_XML).interfaces) {
            connection.register_object(UNIT_PATH, iface, null, (conn, sender, path, ifaceName, name) => ({
                ActiveState: () => new GLib.Variant('s', unit.active),
                MainPID: () => new GLib.Variant('u', unit.pid),
                ExecMainStartTimestampMonotonic: () => new GLib.Variant('t', unit.started),
            })[name](), null);
        }
        return true;
    });
    server.start();
    const bus = await new Promise((resolve, reject) => Gio.DBusConnection.new_for_address(server.get_client_address(),
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, null, null, (_o, res) => {
            try {
                resolve(Gio.DBusConnection.new_for_address_finish(res));
            } catch (e) {
                reject(e);
            }
        }));
    const sd = new SystemdUser(bus);
    unit.restartStarts = false;
    await sd.reload();
    await sd.enable('landline-engine.service');
    await sd.resetFailed('landline-engine.service');
    await sd.restart('landline-engine.service');
    await sd.stop('landline-engine.service');
    await sd.disable('landline-engine.service');
    check('systemd over D-Bus: the calls and their arguments', calls, [
        ['Reload', []],
        ['EnableUnitFiles', [['landline-engine.service'], false, true]],
        ['ResetFailedUnit', ['landline-engine.service']],
        ['RestartUnit', ['landline-engine.service', 'replace']],
        ['StopUnit', ['landline-engine.service', 'replace']],
        ['DisableUnitFiles', [['landline-engine.service'], false]],
    ]);
    check('systemd over D-Bus: the unit read through its properties',
        [await sd.unitState('landline-engine.service'), await sd.activeState('landline-engine.service')],
        [{active: 'active', pid: 4242, started: 5000000}, 'active']);

    // The installer against it, from "Start", with a previous engine beside
    // and an engine that always answers on the socket: the old one.
    const home2 = `${dir}/home-dbus`;
    const p = enginePaths({home: home2, configDir: `${home2}/.config`, cacheDir: `${home2}/.cache`, runtimeDir: `${dir}/run`});
    write(`${p.previous}/landlined`, '#!/bin/sh\n', 0o755);
    write(`${p.daemon}`, '#!/bin/sh\n', 0o755);
    const release = {version: '0.2.0', arch: 'x86_64', url: 'http://127.0.0.1:9/none', sha256: 'a'.repeat(64),
        setupSha256: 'b'.repeat(64), leParamsSha256: 'c'.repeat(64)};
    const installer = () => new EngineInstaller({release, paths: p, systemd: sd, probe: async () => true,
        startTimeoutMs: 400, probeEveryMs: 50});
    calls.length = 0;
    let flow = await installer().run('start');
    check('over D-Bus, the old engine answers and the new one does not start: Start fails, going back first',
        [flow.failedAt, flow.error, flow.canRollback, remedies(flow)], ['start', 'silent', true, ['rollback', 'retry', 'manual']]);
    check('over D-Bus: the unit was read before RestartUnit', calls.map(c => c[0]).slice(0, 6),
        ['Reload', 'EnableUnitFiles', 'ResetFailedUnit', 'LoadUnit', 'RestartUnit', 'LoadUnit']);
    unit.restartStarts = true;
    flow = await installer().run('start');
    check('over D-Bus, a new process started: Start passes', [flow.failedAt, flow.steps.start.state], [null, 'done']);
    bus.close_sync(null);
    server.stop();
}

// 18 November: Replace on a computer whose iPhone was linked all along
// offered "Link My iPhone…". The last step is ticked when the engine says so.
async function alreadyLinked() {
    const make = linked => new EngineInstaller({
        release: {version: '9.9.9', arch: 'x86_64', url: 'http://127.0.0.1:1/x', sha256: '0'.repeat(64)},
        paths: enginePaths({home: `${dir}/lh`, configDir: `${dir}/lh/c`, cacheDir: `${dir}/lh/k`, runtimeDir: `${dir}/lh/r`}),
        systemd: {}, probe: async () => true, linkedProbe: async () => linked,
    });
    for (const [linked, want] of [[true, 'done'], [false, 'waiting']]) {
        const inst = make(linked);
        inst.systemReady();
        await inst._checkLinked();
        check(`system ready, iPhone ${linked ? 'already linked' : 'not linked'}: last step ${want}`,
            inst.flow.steps.phone.state, want);
    }
    const skipped = make(true);
    skipped.systemSkip();
    await skipped._checkLinked();
    check('password skipped, iPhone linked: last step done', skipped.flow.steps.phone.state, 'done');
}

main().then(() => systemdOverDBus()).then(() => alreadyLinked()).catch(e => {
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

