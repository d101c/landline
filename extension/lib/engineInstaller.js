// SPDX-License-Identifier: GPL-2.0-or-later
// Installs, updates, rolls back and removes the engine, from the
// preferences window only (never in the Shell process):
//
//   download   libsoup, the archive pinned in lib/engineRelease.js, into
//              ~/.cache/landline/, hashed on the way
//   verify     its SHA-256 against the pinned one
//   install    unpacked by libgnome-autoar next to the engine
//              (~/.local/lib/landline/.staging), put together as engine.new,
//              then switched in: engine -> engine.old, engine.new -> engine;
//              the user unit and the obex drop-in are written
//   start      systemd's D-Bus API on the session bus (Reload,
//              EnableUnitFiles, RestartUnit), then the engine's socket is
//              asked for protocol_info until it answers
//
// No process is started here and no terminal is needed. The system step
// (the password) is in lib/engineInstall.js (bootstrapScript) and
// lib/enginePage.js.
//
// systemd is behind a small interface (SystemdUser below, or a stand-in in
// the tests): reload(), enable(unit), disable(unit), restart(unit),
// stop(unit), resetFailed(unit), activeState(unit), unitState(unit).
//
// "Start" is passed only by the process RestartUnit started: RestartUnit
// returns once the job is queued, and during an update the old engine still
// answers on the socket. The unit's MainPID and
// ExecMainStartTimestampMonotonic are read before RestartUnit; the probe
// counts only once both have changed and the unit is active, and that same
// process must still be the unit's after the probe answered.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {
    LAYOUT, advance, archiveName, archiveRoot, classify, newFlow, obexDropinText,
    releaseIsPinned, sameSum, unitText,
} from './engineInstall.js';
import {LOG_PREFIX, SOCKET_PATH, SYSTEM_DAEMON, SYSTEM_ENGINE_DIRS, SYSTEM_VERSION_FILE} from './names.js';

Gio._promisify(Gio.File.prototype, 'replace_contents_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.InputStream.prototype, 'read_bytes_async');
Gio._promisify(Gio.OutputStream.prototype, 'write_bytes_async');
Gio._promisify(Gio.OutputStream.prototype, 'close_async');
Gio._promisify(Gio.File.prototype, 'replace_async');
Gio._promisify(Gio.File.prototype, 'read_async');
Gio._promisify(Gio.SocketClient.prototype, 'connect_async');
Gio._promisify(Gio.DataInputStream.prototype, 'read_line_async');
Gio._promisify(Gio.DBusConnection.prototype, 'call');

const CHUNK = 256 * 1024;
const USER_AGENT = 'landline-extension';

// How long the new engine has to answer, and how often it is asked.
export const START_TIMEOUT_MS = 20000;
const PROBE_EVERY_MS = 1000;
const PROBE_TIMEOUT_MS = 2000;

// -- where ----------------------------------------------------------------------

export function enginePaths({
    home = GLib.get_home_dir(),
    configDir = GLib.get_user_config_dir(),
    cacheDir = GLib.get_user_cache_dir(),
    runtimeDir = GLib.get_user_runtime_dir(),
} = {}) {
    const base = GLib.build_filenamev([home, ...LAYOUT.base]);
    const join = (...parts) => GLib.build_filenamev(parts);
    const units = join(configDir, 'systemd', 'user');
    return {
        base,
        engine: join(base, LAYOUT.engine),
        previous: join(base, LAYOUT.previous),
        incoming: join(base, LAYOUT.incoming),
        staging: join(base, LAYOUT.staging),
        daemon: join(base, LAYOUT.engine, LAYOUT.daemon),
        versionFile: join(base, LAYOUT.engine, LAYOUT.versionFile),
        setup: join(base, LAYOUT.engine, LAYOUT.system, LAYOUT.setup),
        leParams: join(base, LAYOUT.engine, LAYOUT.leParams),
        // What scripts/install-local.sh leaves next to the engine.
        localSetup: join(base, LAYOUT.setup),
        units,
        unit: join(units, LAYOUT.unit),
        obexDropinDir: join(units, LAYOUT.obexDropinDir),
        obexDropin: join(units, LAYOUT.obexDropinDir, LAYOUT.obexDropin),
        downloads: join(cacheDir, 'landline'),
        socket: join(runtimeDir, ...SOCKET_PATH),
    };
}

// -- small file helpers (the preferences process, not the Shell) -------------------

function exists(path) {
    return GLib.file_test(path, GLib.FileTest.EXISTS) || GLib.file_test(path, GLib.FileTest.IS_SYMLINK);
}

function readText(path) {
    try {
        const [ok, bytes] = GLib.file_get_contents(path);
        return ok ? new TextDecoder().decode(bytes) : null;
    } catch (e) {
        return null;
    }
}

// rm -rf, without following links.
export function removeTree(path) {
    const file = Gio.File.new_for_path(path);
    let info;
    try {
        info = file.query_info('standard::type', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
    } catch (e) {
        return;
    }
    if (info.get_file_type() === Gio.FileType.DIRECTORY) {
        const children = file.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
        let child;
        while ((child = children.next_file(null)) !== null)
            removeTree(GLib.build_filenamev([path, child.get_name()]));
        children.close(null);
    }
    file.delete(null);
}

function rename(from, to) {
    Gio.File.new_for_path(from).move(Gio.File.new_for_path(to), Gio.FileCopyFlags.NOFOLLOW_SYMLINKS, null, null);
}

function mkdirs(path) {
    GLib.mkdir_with_parents(path, 0o755);
}

async function writeFile(path, text) {
    mkdirs(GLib.path_get_dirname(path));
    await Gio.File.new_for_path(path).replace_contents_async(
        new TextEncoder().encode(text), null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, null);
}

function isExecutable(path) {
    return GLib.file_test(path, GLib.FileTest.IS_EXECUTABLE) && GLib.file_test(path, GLib.FileTest.IS_REGULAR);
}

// What is installed (see classify() in lib/engineInstall.js).
export function readFacts(p) {
    return {
        daemon: isExecutable(p.daemon),
        version: exists(p.versionFile) ? (readText(p.versionFile) ?? '').trim() : null,
        previous: exists(GLib.build_filenamev([p.previous, LAYOUT.daemon])),
        incoming: exists(p.incoming),
        unit: exists(p.unit),
        unitOurs: (readText(p.unit) ?? '').includes('Landline extension'),
        previousVersion: (readText(GLib.build_filenamev([p.previous, LAYOUT.versionFile])) ?? '').trim() || null,
    };
}

// The engine of a distribution package, when there is one: {dir, version}.
export function systemEngine(dirs = SYSTEM_ENGINE_DIRS) {
    for (const dir of dirs) {
        if (isExecutable(GLib.build_filenamev([dir, SYSTEM_DAEMON]))) {
            const version = (readText(GLib.build_filenamev([dir, SYSTEM_VERSION_FILE])) ?? '').trim() || null;
            return {dir, version};
        }
    }
    return null;
}

// An engine in the home folder comes first: its user unit hides the one of
// the package. With none there, the engine of the package is state 'system':
// nothing to install, update or remove from here.
export function detect(p, release, systemDirs = SYSTEM_ENGINE_DIRS) {
    const facts = readFacts(p);
    const system = facts.daemon || facts.previous || facts.incoming ? null : systemEngine(systemDirs);
    if (system) {
        return {state: 'system', action: null, installed: true, version: system.version,
            canRollback: false, needsConfirm: false, facts, system};
    }
    return {...classify(facts, release), facts};
}

const isRegular = path => GLib.file_test(path, GLib.FileTest.IS_REGULAR) && !GLib.file_test(path, GLib.FileTest.IS_SYMLINK);

// Which landline-setup the system step by hand runs, for
// manualSystemCommands() in lib/engineInstall.js: the one of the pinned
// engine, checked against its sums, when that engine is installed; else
// the copy scripts/install-local.sh leaves, or the one of another version.
// null when there is none.
export function manualSystemWhere(p, release) {
    const facts = readFacts(p);
    if (releaseIsPinned(release) && facts.version === release.version && isRegular(p.setup) && isRegular(p.leParams))
        return {kind: 'pinned', setup: p.setup, leParams: p.leParams};
    const order = facts.version === null ? [p.localSetup, p.setup] : [p.setup, p.localSetup];
    const setup = order.find(isRegular);
    return setup ? {kind: 'local', setup} : null;
}

// -- checksums and downloads ----------------------------------------------------------

export async function sha256File(path, cancellable = null) {
    const stream = await Gio.File.new_for_path(path).read_async(GLib.PRIORITY_DEFAULT, cancellable);
    const sum = new GLib.Checksum(GLib.ChecksumType.SHA256);
    try {
        for (;;) {
            const bytes = await stream.read_bytes_async(CHUNK, GLib.PRIORITY_DEFAULT, cancellable);
            if (bytes.get_size() === 0)
                break;
            sum.update(bytes.toArray());
        }
    } finally {
        stream.close(null);
    }
    return sum.get_string();
}

let soupSession = null;
async function httpStream(url, cancellable) {
    const {default: Soup} = await import('gi://Soup?version=3.0');
    if (!soupSession)
        soupSession = new Soup.Session({user_agent: USER_AGENT, timeout: 60});
    const msg = Soup.Message.new('GET', url);
    const stream = await new Promise((resolve, reject) => {
        soupSession.send_async(msg, GLib.PRIORITY_DEFAULT, cancellable, (s, res) => {
            try {
                resolve(s.send_finish(res));
            } catch (e) {
                reject(e);
            }
        });
    });
    if (msg.get_status() !== Soup.Status.OK) {
        stream.close(null);
        throw new Error(`HTTP ${msg.get_status()}`);
    }
    return {stream, total: msg.get_response_headers().get_content_length()};
}

// Ends the downloads still running and lets the session go (the
// preferences window is closing). The next download makes a new one.
export function closeSession() {
    soupSession?.abort();
    soupSession = null;
}

// For the tests: whether a session is held.
export function hasSession() {
    return soupSession !== null;
}

// Downloads url to dest (through dest.part), hashing on the way;
// progress(done, total). Resolves to the hex SHA-256.
export async function download(url, dest, progress = null, cancellable = null) {
    mkdirs(GLib.path_get_dirname(dest));
    const part = `${dest}.part`;
    const {stream, total} = await httpStream(url, cancellable);
    const out = await Gio.File.new_for_path(part).replace_async(
        null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, GLib.PRIORITY_DEFAULT, cancellable);
    const sum = new GLib.Checksum(GLib.ChecksumType.SHA256);
    let done = 0;
    try {
        for (;;) {
            const bytes = await stream.read_bytes_async(CHUNK, GLib.PRIORITY_DEFAULT, cancellable);
            if (bytes.get_size() === 0)
                break;
            sum.update(bytes.toArray());
            await out.write_bytes_async(bytes, GLib.PRIORITY_DEFAULT, cancellable);
            done += bytes.get_size();
            progress?.(done, total > 0 ? total : 0);
        }
    } finally {
        stream.close(null);
        await out.close_async(GLib.PRIORITY_DEFAULT, null).catch(() => {});
    }
    Gio.File.new_for_path(part).move(Gio.File.new_for_path(dest), Gio.FileCopyFlags.OVERWRITE, cancellable, null);
    return sum.get_string();
}

// Unpacks archive into dir with libgnome-autoar (tar.zst included); dir is
// the destination itself.
export async function extract(archive, dir, cancellable = null) {
    const {default: Autoar} = await import('gi://GnomeAutoar?version=0.1');
    mkdirs(dir);
    await new Promise((resolve, reject) => {
        const ex = new Autoar.Extractor({
            source_file: Gio.File.new_for_path(archive),
            output_file: Gio.File.new_for_path(dir),
            output_is_dest: true,
            delete_after_extraction: false,
        });
        // 2: AUTOAR_CONFLICT_OVERWRITE (dir is new anyway).
        ex.connect('conflict', () => 2);
        ex.connect('error', (_e, err) => reject(new Error(err.message)));
        ex.connect('cancelled', () => reject(new Gio.IOErrorEnum({code: Gio.IOErrorEnum.CANCELLED, message: 'Cancelled'})));
        ex.connect('completed', () => resolve());
        ex.start_async(cancellable);
    });
}

// -- the engine's socket --------------------------------------------------------------

// Resolves to true when the engine answers protocol_info within timeoutMs.
export async function probeEngine(socketPath, timeoutMs = PROBE_TIMEOUT_MS) {
    return await askEngine(socketPath, 'protocol_info', timeoutMs) !== null;
}

// Resolves to true when the engine says an iPhone is paired and its bond is
// whole: the last step has nothing left to do (18 November: Replace offered
// "Link My iPhone…" to an iPhone linked all along).
export async function phoneLinked(socketPath, timeoutMs = PROBE_TIMEOUT_MS) {
    const status = await askEngine(socketPath, 'bt_status', timeoutMs);
    return Boolean(status?.device_address) && status?.bond_state === 'ok';
}

// One command on the engine's socket; resolves to its answer, or null.
export async function askEngine(socketPath, command, timeoutMs = PROBE_TIMEOUT_MS) {
    const cancellable = new Gio.Cancellable();
    const timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
        cancellable.cancel();
        return GLib.SOURCE_REMOVE;
    });
    let connection = null;
    try {
        const client = new Gio.SocketClient();
        connection = await client.connect_async(Gio.UnixSocketAddress.new(socketPath), cancellable);
        connection.get_output_stream().write_bytes(
            new GLib.Bytes(new TextEncoder().encode(`${JSON.stringify({command})}\n`)), cancellable);
        const input = new Gio.DataInputStream({base_stream: connection.get_input_stream()});
        for (let i = 0; i < 50; i++) {
            const [line] = await input.read_line_async(GLib.PRIORITY_DEFAULT, cancellable);
            if (line === null)
                return null;
            try {
                const answer = JSON.parse(new TextDecoder().decode(line));
                if (answer?.command === command)
                    return answer;
            } catch (e) {
                // A bare OK or another line: read on.
            }
        }
        return null;
    } catch (e) {
        return null;
    } finally {
        if (!cancellable.is_cancelled())
            GLib.source_remove(timer);
        connection?.close(null);
    }
}

// -- systemd, user manager, on the session bus -------------------------------------------

const SD = ['org.freedesktop.systemd1', '/org/freedesktop/systemd1', 'org.freedesktop.systemd1.Manager'];

export class SystemdUser {
    constructor(bus = Gio.DBus.session) {
        this._bus = bus;
    }

    async _call(method, params = null, type = null) {
        const reply = await this._bus.call(...SD, method, params,
            type ? new GLib.VariantType(type) : null, Gio.DBusCallFlags.NONE, 30000, null);
        return reply?.deepUnpack();
    }

    reload() {
        return this._call('Reload');
    }

    enable(unit) {
        return this._call('EnableUnitFiles', new GLib.Variant('(asbb)', [[unit], false, true]), '(ba(sss))');
    }

    disable(unit) {
        return this._call('DisableUnitFiles', new GLib.Variant('(asb)', [[unit], false]), '(a(sss))');
    }

    restart(unit) {
        return this._call('RestartUnit', new GLib.Variant('(ss)', [unit, 'replace']), '(o)');
    }

    stop(unit) {
        return this._call('StopUnit', new GLib.Variant('(ss)', [unit, 'replace']), '(o)');
    }

    async resetFailed(unit) {
        try {
            await this._call('ResetFailedUnit', new GLib.Variant('(s)', [unit]));
        } catch (e) {
            // Not failed, or not loaded: nothing to reset.
        }
    }

    async _property(path, iface, name) {
        const reply = await this._bus.call('org.freedesktop.systemd1', path,
            'org.freedesktop.DBus.Properties', 'Get', new GLib.Variant('(ss)', [iface, name]),
            new GLib.VariantType('(v)'), Gio.DBusCallFlags.NONE, 5000, null);
        return reply.deepUnpack()[0].deepUnpack();
    }

    // {active, pid, started}: ActiveState ('active', 'activating',
    // 'failed'…), MainPID (0 when no process runs) and
    // ExecMainStartTimestampMonotonic (µs, when the main process was
    // started; another value means another process), or null.
    async unitState(unit) {
        try {
            const [path] = await this._call('LoadUnit', new GLib.Variant('(s)', [unit]), '(o)');
            const active = await this._property(path, 'org.freedesktop.systemd1.Unit', 'ActiveState');
            const pid = await this._property(path, 'org.freedesktop.systemd1.Service', 'MainPID');
            const started = await this._property(path, 'org.freedesktop.systemd1.Service', 'ExecMainStartTimestampMonotonic');
            return {active, pid: Number(pid), started: Number(started)};
        } catch (e) {
            return null;
        }
    }

    // 'active', 'activating', 'failed', 'inactive'…, or null.
    async activeState(unit) {
        return (await this.unitState(unit))?.active ?? null;
    }
}

// -- the installer -------------------------------------------------------------------------

const sleep = (ms, cancellable) => new Promise(resolve => {
    const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        cancellable?.disconnect(handler);
        resolve();
        return GLib.SOURCE_REMOVE;
    });
    const handler = cancellable ? cancellable.connect(() => {
        GLib.source_remove(id);
        resolve();
    }) : 0;
});

// One install at a time. onChange(flow) after every step.
export class EngineInstaller {
    constructor({release, paths = enginePaths(), systemd = new SystemdUser(), probe = probeEngine,
        linkedProbe = phoneLinked, onChange = () => {}, startTimeoutMs = START_TIMEOUT_MS,
        probeEveryMs = PROBE_EVERY_MS} = {}) {
        this.release = release;
        this.paths = paths;
        this.systemd = systemd;
        this.probe = probe;
        this.linkedProbe = linkedProbe;
        this.onChange = onChange;
        this.startTimeoutMs = startTimeoutMs;
        this.probeEveryMs = probeEveryMs;
        this.flow = newFlow();
        this.flow.canRollback = readFacts(paths).previous;
        this._cancellable = null;
        // A rollback or an uninstall is under way.
        this._working = false;
        // The unit as it was before RestartUnit (see _start()).
        this._before = null;
    }

    // Whether the engine's files must be left alone now: an install, the
    // password step, a rollback or an uninstall is running.
    get busy() {
        return this.flow.running || this._working || this.flow.steps.system.state === 'active';
    }

    get archive() {
        return GLib.build_filenamev([this.paths.downloads, archiveName(this.release)]);
    }

    _emit(event) {
        this.flow = advance(this.flow, event);
        this.onChange(this.flow);
    }

    cancel() {
        this._cancellable?.cancel();
    }

    destroy() {
        this.cancel();
        this.onChange = () => {};
        closeSession();
    }

    // Runs the steps from `from` ('download', 'verify', 'install' or
    // 'start') to the system step, which waits for the window. Resolves to
    // the flow.
    async run(from = 'download') {
        if (this.busy)
            return this.flow;
        this._cancellable = new Gio.Cancellable();
        const cancellable = this._cancellable;
        this._emit({type: 'begin', from});
        const fail = (error, e = null) => {
            if (e && !(e instanceof Error && cancellable.is_cancelled()))
                console.warn(`${LOG_PREFIX}: engine install, ${error}: ${e.message ?? e}`);
            this._emit({type: 'failed', error: cancellable.is_cancelled() ? 'cancelled' : error,
                detail: e?.message ?? ''});
            return this.flow;
        };

        if (from === 'download') {
            try {
                await this._download(cancellable);
            } catch (e) {
                return fail('network', e);
            }
        }
        if (from === 'download' || from === 'verify') {
            try {
                if (!sameSum(await sha256File(this.archive, cancellable), this.release.sha256)) {
                    // Never used again: the next try downloads it afresh.
                    GLib.unlink(this.archive);
                    return fail('mismatch');
                }
            } catch (e) {
                return fail(cancellable.is_cancelled() ? 'cancelled' : 'network', e);
            }
            this._emit({type: 'verified'});
        }
        if (from !== 'start') {
            let previous;
            try {
                previous = await this._install(cancellable);
            } catch (e) {
                return fail(e.landline ?? 'write', e);
            }
            this._emit({type: 'installed', previous});
        }
        try {
            await this._start();
        } catch (e) {
            return fail('systemd', e);
        }
        if (!await this._waitForAnswer(cancellable))
            return fail(cancellable.is_cancelled() ? 'cancelled' : 'silent');
        // The archive is no longer needed.
        try {
            GLib.unlink(this.archive);
        } catch (e) {
            // Already gone.
        }
        this._emit({type: 'answered'});
        return this.flow;
    }

    async _download(cancellable) {
        // A complete copy from an earlier try is used again.
        if (exists(this.archive) && sameSum(await sha256File(this.archive, cancellable), this.release.sha256)) {
            this._emit({type: 'downloaded', reused: true});
            return;
        }
        await download(this.release.url, this.archive,
            (done, total) => this._emit({type: 'progress', done, total}), cancellable);
        this._emit({type: 'downloaded'});
    }

    // Puts the new engine together as engine.new, checks it, then switches.
    // Resolves to whether an engine.old is there to go back to.
    async _install(cancellable) {
        const p = this.paths;
        const fault = (kind, message) => Object.assign(new Error(message), {landline: kind});
        removeTree(p.staging);
        removeTree(p.incoming);
        try {
            await extract(this.archive, p.staging, cancellable);
        } catch (e) {
            removeTree(p.staging);
            throw fault(cancellable.is_cancelled() ? 'cancelled' : 'unpack', e.message);
        }
        const root = GLib.build_filenamev([p.staging, archiveRoot(this.release)]);
        const engine = GLib.build_filenamev([root, 'engine']);
        const system = GLib.build_filenamev([root, LAYOUT.system]);
        const version = (readText(GLib.build_filenamev([engine, LAYOUT.versionFile])) ?? '').trim();
        const setup = GLib.build_filenamev([system, LAYOUT.setup]);
        const le = GLib.build_filenamev([engine, LAYOUT.leParams]);
        const complete = isExecutable(GLib.build_filenamev([engine, LAYOUT.daemon])) &&
            exists(setup) && exists(le) && version === this.release.version;
        if (!complete) {
            removeTree(p.staging);
            throw fault('contents', 'the archive does not hold the expected engine');
        }
        // The two files the system step runs as root must be the pinned ones.
        if (!sameSum(await sha256File(setup, cancellable), this.release.setupSha256) ||
            !sameSum(await sha256File(le, cancellable), this.release.leParamsSha256)) {
            removeTree(p.staging);
            throw fault('mismatch', 'landline-setup or landline-le-params is not the pinned file');
        }
        mkdirs(p.base);
        rename(engine, p.incoming);
        rename(system, GLib.build_filenamev([p.incoming, LAYOUT.system]));
        removeTree(p.staging);
        // Asked for its status without privilege: it must be executable,
        // whatever mode the archive gave it.
        Gio.File.new_for_path(GLib.build_filenamev([p.incoming, LAYOUT.system, LAYOUT.setup]))
            .set_attribute_uint32('unix::mode', 0o755, Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);

        // The switch. engine.old becomes the engine that ran until now,
        // unless that one is already this very version (Reinstall, or a
        // retry after a failure that came after the switch): then it is
        // replaced, and engine.old, the real previous engine, stays.
        const facts = readFacts(p);
        let previous;
        if (exists(p.engine) && facts.version !== this.release.version) {
            removeTree(p.previous);
            rename(p.engine, p.previous);
            previous = true;
        } else {
            if (exists(p.engine)) {
                // Out of the way in one rename, then removed.
                rename(p.engine, p.staging);
                removeTree(p.staging);
            }
            previous = facts.previous;
        }
        rename(p.incoming, p.engine);

        await writeFile(p.unit, unitText());
        await writeFile(p.obexDropin, obexDropinText());
        return previous;
    }

    async _start() {
        const unit = LAYOUT.unit;
        await this.systemd.reload();
        await this.systemd.enable(unit);
        await this.systemd.resetFailed(unit);
        this._before = await this.systemd.unitState(unit);
        await this.systemd.restart(unit);
    }

    // Whether the unit now runs a process RestartUnit started: active, with
    // a main process that is neither the one read before nor started when
    // that one was.
    _newProcess(state) {
        if (!state || state.active !== 'active' || !(state.pid > 0) || !(state.started > 0))
            return false;
        const b = this._before;
        return !b || (state.pid !== b.pid && state.started !== b.started);
    }

    async _waitForAnswer(cancellable) {
        const unit = LAYOUT.unit;
        const end = GLib.get_monotonic_time() + this.startTimeoutMs * 1000;
        while (!cancellable.is_cancelled()) {
            const state = await this.systemd.unitState(unit);
            if (this._newProcess(state) && await this.probe(this.paths.socket)) {
                // The process that answered is still the unit's.
                const after = await this.systemd.unitState(unit);
                if (after?.active === 'active' && after.pid === state.pid && after.started === state.started)
                    return true;
            }
            if (GLib.get_monotonic_time() >= end)
                return false;
            await sleep(this.probeEveryMs, cancellable);
        }
        return false;
    }

    // Back to engine.old: the new one is removed, the old one restarted.
    // Resolves to false, having done nothing, when there is nothing to go
    // back to or something else runs.
    async rollback() {
        if (this.busy || !readFacts(this.paths).previous)
            return false;
        this._working = true;
        try {
            return await this._rollback();
        } finally {
            this._working = false;
        }
    }

    async _rollback() {
        const p = this.paths;
        try {
            await this.systemd.stop(LAYOUT.unit);
        } catch (e) {
            // Not running: nothing to stop.
        }
        removeTree(p.incoming);
        if (exists(p.engine))
            rename(p.engine, p.incoming);
        rename(p.previous, p.engine);
        removeTree(p.incoming);
        if (!exists(p.unit))
            await writeFile(p.unit, unitText());
        try {
            await this.systemd.reload();
            await this.systemd.resetFailed(LAYOUT.unit);
            await this.systemd.restart(LAYOUT.unit);
        } catch (e) {
            console.warn(`${LOG_PREFIX}: the previous engine could not be started: ${e.message}`);
        }
        this._emit({type: 'rolled-back'});
        return true;
    }

    // Stops and disables the service, then removes the engine and what was
    // written for it. The engine's own data (the pairing, settings, kept
    // messages in ~/.config, ~/.local/share and ~/.local/state) stays.
    // Resolves to false, having done nothing, while something else runs.
    async uninstall() {
        if (this.busy)
            return false;
        this._working = true;
        try {
            await this._uninstall();
            return true;
        } finally {
            this._working = false;
        }
    }

    async _uninstall() {
        const p = this.paths;
        const unit = LAYOUT.unit;
        for (const step of [() => this.systemd.stop(unit), () => this.systemd.disable(unit)]) {
            try {
                await step();
            } catch (e) {
                // Not loaded or not enabled.
            }
        }
        for (const path of [p.engine, p.previous, p.incoming, p.staging, p.localSetup, p.unit, p.obexDropin])
            removeTree(path);
        for (const dir of [p.obexDropinDir, p.base]) {
            try {
                Gio.File.new_for_path(dir).delete(null);
            } catch (e) {
                // Not empty, or not there.
            }
        }
        removeTree(this.archive);
        try {
            await this.systemd.reload();
        } catch (e) {
            console.warn(`${LOG_PREFIX}: systemd could not reload: ${e.message}`);
        }
        this.flow = newFlow();
        this.onChange(this.flow);
    }

    // The system step, from the window.
    systemAsking() {
        this._emit({type: 'system-asking'});
    }

    // resume: the engine was installed earlier, only this step is left.
    systemNeeded(manual, resume = false) {
        this._emit({type: 'system-needed', manual, resume});
    }

    systemReady() {
        this._emit({type: 'system-ready'});
        this._checkLinked();
    }

    systemResult(result, detail = '') {
        this._emit({type: 'system-result', result, detail});
        this._checkLinked();
    }

    systemSkip() {
        this._emit({type: 'system-skip'});
        this._checkLinked();
    }

    // The last step is done already when the iPhone is linked.
    async _checkLinked() {
        if (!this.flow.finished || this.flow.steps.phone.state !== 'waiting')
            return false;
        let linked = false;
        try {
            linked = await this.linkedProbe(this.paths.socket);
        } catch (e) {
            linked = false;
        }
        if (linked && this.flow.steps.phone.state === 'waiting')
            this.linked();
        return linked;
    }

    linked() {
        this._emit({type: 'linked'});
    }
}
