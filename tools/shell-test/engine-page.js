// SPDX-License-Identifier: GPL-2.0-or-later
// Phone Settings › Engine, drawn in each of its states, without installing
// anything: gjs -m tools/shell-test/engine-page.js <dossier des captures>
// Lancé par tools/shell-test/engine-page.sh (HOME temporaire, GTK sur
// broadway). systemd et le moteur sont des doublures ; les états sont
// amenés en passant à l'installateur les événements qu'il recevrait.
import Adw from 'gi://Adw?version=1';
import Gdk from 'gi://Gdk?version=4.0';
import Gio from 'gi://Gio';
import Graphene from 'gi://Graphene';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';

import {buildEnginePage} from '../../extension/lib/enginePage.js';
import {EngineInstaller, enginePaths} from '../../extension/lib/engineInstaller.js';

const [shots] = ARGV;
const here = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const extension = Gio.File.new_for_path(`${here}/../../extension`);
const home = GLib.get_home_dir();
const write = (path, text, mode = 0o644) => {
    GLib.mkdir_with_parents(GLib.path_get_dirname(path), 0o755);
    GLib.file_set_contents(path, text);
    GLib.chmod(path, mode);
};
const sleep = ms => new Promise(r => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    r();
    return GLib.SOURCE_REMOVE;
}));

const release = {version: '0.2.0', arch: 'x86_64', url: 'http://127.0.0.1:9/none.tar.zst',
    sha256: 'a'.repeat(64), setupSha256: 'b'.repeat(64), leParamsSha256: 'c'.repeat(64)};
const systemd = {
    reload: async () => {}, enable: async () => {}, disable: async () => {}, restart: async () => {},
    stop: async () => {}, resetFailed: async () => {}, activeState: async () => 'active',
    unitState: async () => ({active: 'active', pid: 0, started: 0}),
};
const settings = {set_boolean: () => {}};

const app = new Adw.Application({application_id: 'io.github.d101c.landline.EnginePage'});
let n = 0;

async function shot(window, name) {
    let w = 0, h = 0;
    for (let i = 0; i < 40 && !(w > 0 && h > 0); i++) {
        await sleep(100);
        [w, h] = [window.get_width(), window.get_height()];
    }
    await sleep(300);
    const paintable = new Gtk.WidgetPaintable({widget: window.get_content()});
    const snapshot = new Gtk.Snapshot();
    // The window's own background is not part of its content.
    const background = new Gdk.RGBA();
    background.parse(Adw.StyleManager.get_default().dark ? '#222226' : '#fafafb');
    snapshot.append_color(background, new Graphene.Rect({origin: new Graphene.Point({x: 0, y: 0}), size: new Graphene.Size({width: w, height: h})}));
    paintable.snapshot(snapshot, w, h);
    const node = snapshot.to_node();
    if (!node)
        throw new Error(`nothing drawn (${w}×${h})`);
    const texture = window.get_native().get_renderer().render_texture(node, null);
    texture.save_to_png(`${shots}/engine-${String(++n).padStart(2, '0')}-${name}.png`);
}

// A few checks besides the pictures: the exit status says whether they held.
let failures = 0;
function expect(label, got, want) {
    if (got === want) {
        print(`ok   ${label}`);
    } else {
        failures++;
        printerr(`ÉCHEC ${label} : ${got} au lieu de ${want}`);
    }
}

function findWidget(widget, type) {
    if (widget instanceof type)
        return widget;
    for (let c = widget.get_first_child(); c; c = c.get_next_sibling()) {
        const found = findWidget(c, type);
        if (found)
            return found;
    }
    return null;
}

// "Uninstall the Engine…", shown and clickable.
function uninstallUsable(window) {
    const row = findWidget(window, Adw.ButtonRow);
    return row !== null && row.is_visible() && row.is_sensitive();
}

// A window with the page, in a fresh home folder state.
function open(prepare, scheme = Adw.ColorScheme.FORCE_DARK) {
    GLib.spawn_command_line_sync(`rm -rf ${home}/.local/lib/landline`);
    prepare?.();
    Adw.StyleManager.get_default().color_scheme = scheme;
    const window = new Adw.PreferencesWindow({application: app, default_width: 720, default_height: 900});
    const paths = enginePaths();
    const installer = new EngineInstaller({release, paths, systemd, probe: async () => false});
    const page = buildEnginePage({window, settings, dir: extension, gettext: s => s, release, paths, systemd, installer});
    window.add(page);
    window.present();
    return {window, installer};
}

const engine = version => () => {
    const base = `${home}/.local/lib/landline/engine`;
    write(`${base}/landlined`, '#!/bin/sh\n', 0o755);
    if (version)
        write(`${base}/VERSION`, `${version}\n`);
    write(`${base}/system/landline-setup`, '#!/bin/sh\nexit 1\n', 0o755);
    write(`${base}/landline-le-params`, 'x', 0o755);
};

async function main() {
    let {window, installer} = open(null);
    await shot(window, 'absent');
    installer._emit({type: 'begin'});
    installer._emit({type: 'progress', done: 3, total: 8});
    await shot(window, 'download');
    installer._emit({type: 'downloaded'});
    installer._emit({type: 'verified'});
    installer._emit({type: 'installed', previous: true});
    await shot(window, 'start');
    installer._emit({type: 'failed', error: 'silent'});
    await shot(window, 'silent');
    window.destroy();

    ({window, installer} = open(engine('0.2.0')));
    for (const e of [{type: 'begin'}, {type: 'downloaded'}, {type: 'verified'}, {type: 'installed', previous: true},
        {type: 'answered'}, {type: 'system-needed', manual: false}])
        installer._emit(e);
    await shot(window, 'password');
    expect('password asked for: Uninstall usable', uninstallUsable(window), true);
    installer._emit({type: 'system-asking'});
    expect('password window up: Uninstall greyed out', uninstallUsable(window), false);
    installer._emit({type: 'system-result', result: 'refused'});
    expect('password refused: Uninstall usable again', uninstallUsable(window), true);
    await shot(window, 'refused');
    installer._emit({type: 'system-skip'});
    await shot(window, 'done');
    window.destroy();

    ({window, installer} = open(engine('0.1.0')));
    await shot(window, 'update');
    expect('engine installed, nothing running: Uninstall usable', uninstallUsable(window), true);
    installer._emit({type: 'begin'});
    expect('update running: Uninstall greyed out', uninstallUsable(window), false);
    installer._emit({type: 'downloaded'});
    installer._emit({type: 'failed', error: 'mismatch'});
    expect('update failed: Uninstall usable again', uninstallUsable(window), true);
    await shot(window, 'mismatch');
    window.destroy();

    ({window, installer} = open(engine(null), Adw.ColorScheme.FORCE_LIGHT));
    await shot(window, 'manual-light');
    installer._emit({type: 'begin'});
    installer._emit({type: 'failed', error: 'network'});
    await shot(window, 'network-light');
    window.destroy();
    app.quit();
}

app.connect('activate', () => {
    app.hold();
    main().catch(e => {
        failures++;
        printerr(`${e}\n${e.stack}`);
        app.quit();
    });
});
app.run([]);
if (failures > 0)
    imports.system.exit(1);
