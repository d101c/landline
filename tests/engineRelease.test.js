// SPDX-License-Identifier: GPL-2.0-or-later
// La vraie archive du moteur, épinglée dans extension/lib/engineRelease.js,
// installée dans un HOME temporaire :
//   LANDLINE_DIST=<dossier de l'archive> gjs -m tests/engineRelease.test.js
// (défaut : dist/ du dépôt ; sans archive, l'essai est sauté).
//
// L'archive est servie par « python3 -m http.server » sur 127.0.0.1 à la
// place de GitHub ; systemd et le moteur qui répond sont des doublures :
// rien n'est démarré, le vrai dossier de l'utilisateur n'est pas touché.
// Vérifie que les sommes épinglées sont celles de l'archive et de ses deux
// fichiers lancés en root, et que l'installation la reconnaît.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {archiveName, bootstrapScript, classify} from '../extension/lib/engineInstall.js';
import {EngineInstaller, enginePaths, readFacts, sha256File} from '../extension/lib/engineInstaller.js';
import {ENGINE_RELEASE} from '../extension/lib/engineRelease.js';

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
const dist = GLib.getenv('LANDLINE_DIST') ?? `${GLib.path_get_dirname(here)}/dist`;
const archive = `${dist}/${archiveName(ENGINE_RELEASE)}`;
const dir = GLib.dir_make_tmp('ll-release-XXXXXX');
const exists = path => GLib.file_test(path, GLib.FileTest.EXISTS);

const systemd = {calls: []};
for (const name of ['reload', 'enable', 'disable', 'restart', 'stop', 'resetFailed'])
    systemd[name] = async unit => systemd.calls.push(unit ? `${name} ${unit}` : name);
// The unit's main process: a new one after each restart (see #23).
let mainPid = 0;
systemd.unitState = async () => ({active: mainPid ? 'active' : 'inactive', pid: mainPid, started: mainPid * 1000});
const restart = systemd.restart;
systemd.restart = async unit => {
    await restart(unit);
    mainPid += 1;
};

async function main() {
    if (!exists(archive)) {
        print(`archive absente (${archive}) : essai sauté`);
        return;
    }
    check('the pinned sum is the archive\'s', await sha256File(archive), ENGINE_RELEASE.sha256);
    const port = 20000 + Math.floor(Math.random() * 20000);
    const server = Gio.Subprocess.new(['python3', '-I', '-m', 'http.server', '--bind', '127.0.0.1', '--directory', dist, String(port)],
        Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
    try {
        await sleep(600);
        const release = {...ENGINE_RELEASE, url: `http://127.0.0.1:${port}/${archiveName(ENGINE_RELEASE)}`};
        const home = `${dir}/home`;
        const paths = enginePaths({home, configDir: `${home}/.config`, cacheDir: `${home}/.cache`, runtimeDir: `${dir}/run`});
        const inst = new EngineInstaller({release, paths, systemd, probe: async () => true});
        const flow = await inst.run();
        check('the real archive installs', [flow.failedAt, flow.error, flow.steps.start.state], [null, null, 'done']);
        check('its engine is the pinned version', classify(readFacts(paths), release).state, 'current');
        check('daemon, CLI and helper executable', ['landlined', 'landlinectl', 'landline-le-params'].map(
            n => GLib.file_test(`${paths.engine}/${n}`, GLib.FileTest.IS_EXECUTABLE)), [true, true, true]);
        check('the two files run as root have the pinned sums',
            [await sha256File(paths.setup), await sha256File(paths.leParams)],
            [ENGINE_RELEASE.setupSha256, ENGINE_RELEASE.leParamsSha256]);
        check('the setup helper answers "version" without privilege', (() => {
            const proc = Gio.Subprocess.new([paths.setup, 'version'], Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
            const [, out] = proc.communicate_utf8(null, null);
            return proc.get_exit_status() === 0 && /^\d+$/.test(out.trim());
        })(), true);
        // The root script, played without root: refuses nothing it should
        // accept. Only the checks run: the copy of landline-setup is run with
        // "version" instead of "apply".
        const work = `${dir}/bootstrap`;
        GLib.mkdir_with_parents(work, 0o755);
        const script = bootstrapScript(ENGINE_RELEASE)
            .replace('/run/landline-setup.XXXXXX', `${work}/landline-setup.XXXXXX`)
            .replace('landline-setup" apply', 'landline-setup" version');
        const proc = Gio.Subprocess.new(['/bin/sh', '-c', script, 'landline-bootstrap', paths.setup, paths.leParams],
            Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
        proc.wait(null);
        check('root script: the installed files pass its checks', proc.get_exit_status(), 0);
        check('nothing under the real home folder', paths.engine.startsWith(dir), true);
    } finally {
        server.force_exit();
    }
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
