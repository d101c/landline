// SPDX-License-Identifier: GPL-2.0-or-later
// Phone Settings › Engine: install, update, go back or remove the engine
// in one click, then a few guided steps, each a line with its state and a
// drawing of what is happening. When a step fails the page says what
// happened in a sentence and offers a way on: try again, go back to the
// previous engine, or the commands to do it by hand.
//
// Only in the preferences window: the Shell process never downloads,
// unpacks or writes any of this.

import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';

import {
    STEPS, activeStep, bootstrapArgv, bootstrapResult, bootstrapScript,
    manualCommands, manualRollbackCommands, manualSystemCommands, releaseIsPinned, remedies, retryFrom,
} from './engineInstall.js';
import {EngineInstaller, detect, enginePaths, manualSystemWhere} from './engineInstaller.js';
import {ENGINE_RELEASE} from './engineRelease.js';
import {LOG_PREFIX} from './names.js';
import {SetupHelper, rootHelper, runAsRoot} from './setupHelper.js';
import {run} from './subprocess.js';

const ICONS = {
    pending: 'content-loading-symbolic',
    done: 'object-select-symbolic',
    failed: 'dialog-warning-symbolic',
    skipped: 'action-unavailable-symbolic',
    waiting: 'dialog-password-symbolic',
};

// The drawing shown for each step (illustrations/engine-<name>.svg).
const DRAWINGS = {
    download: 'download', verify: 'verify', install: 'install', start: 'start',
    system: 'system', phone: 'phone',
};

// The one place this page writes to the journal.
function warn(e, what = 'engine') {
    console.warn(`${LOG_PREFIX}: ${what}: ${e?.message ?? e}`);
}

// Whether the page should be the one shown when the window opens.
export function engineNeedsAttention(paths = enginePaths(), release = ENGINE_RELEASE) {
    return ['absent', 'interrupted', 'outdated'].includes(detect(paths, release).state);
}

// systemd and installer: stand-ins, for tools/shell-test/engine-page.js.
export function buildEnginePage({window, settings, dir, gettext: _, release = ENGINE_RELEASE, paths = enginePaths(),
    systemd = undefined, installer: given = null}) {
    const page = new Adw.PreferencesPage({
        title: _('Engine'),
        icon_name: 'system-software-install-symbolic',
        name: 'engine',
    });
    const cancellable = new Gio.Cancellable();
    const helper = new SetupHelper();
    const installer = given ?? new EngineInstaller({release, paths, ...systemd ? {systemd} : {}});
    let installed = detect(paths, release);
    let manualShown = false;
    let manualLines = [];
    let busy = false;

    // -- the top: a drawing, a sentence, the buttons ---------------------------------
    const hero = new Adw.PreferencesGroup();
    const top = new Gtk.Box({spacing: 18, margin_top: 6, margin_bottom: 6});
    const picture = new Gtk.Picture({
        can_shrink: true,
        content_fit: Gtk.ContentFit.CONTAIN,
        width_request: 168,
        height_request: 112,
        valign: Gtk.Align.CENTER,
    });
    picture.add_css_class('card');
    top.append(picture);
    const words = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 6, valign: Gtk.Align.CENTER, hexpand: true});
    const title = new Gtk.Label({xalign: 0, wrap: true});
    title.add_css_class('title-2');
    const body = new Gtk.Label({xalign: 0, wrap: true});
    body.add_css_class('body');
    words.append(title);
    words.append(body);
    const bar = new Gtk.ProgressBar({visible: false, margin_top: 6});
    words.append(bar);
    // The buttons wrap to a second line rather than run off the window.
    const buttons = new Adw.WrapBox({child_spacing: 8, line_spacing: 8, margin_top: 8});
    words.append(buttons);
    top.append(words);
    hero.add(top);
    page.add(hero);

    // -- the steps ----------------------------------------------------------------------
    const stepsGroup = new Adw.PreferencesGroup({title: _('Steps')});
    const labels = {
        download: [_('Download'),
            // Translators: %s is a version number, like 0.2.0.
            _('The engine, version %s, from GitHub').replace('%s', release.version)],
        verify: [_('Check'), _('Making sure it is the genuine file')],
        install: [_('Install'), _('Into your home folder; the engine you had stays beside it')],
        start: [_('Start'), _('The engine starts now, and with every session')],
        system: [_('Bluetooth settings'), _('Your password, once, for two settings iPhones need')],
        phone: [_('Link your iPhone'), _('Pictures in the top bar show where to tap')],
    };
    const rows = {};
    for (const id of STEPS) {
        const row = new Adw.ActionRow({title: labels[id][0], subtitle: labels[id][1]});
        const stack = new Gtk.Stack({valign: Gtk.Align.CENTER});
        const icon = new Gtk.Image({icon_name: ICONS.pending, pixel_size: 16});
        const spinner = new Adw.Spinner();
        stack.add_named(icon, 'icon');
        stack.add_named(spinner, 'spinner');
        row.add_prefix(stack);
        stepsGroup.add(row);
        rows[id] = {row, stack, icon};
    }
    page.add(stepsGroup);

    // -- by hand --------------------------------------------------------------------------
    const manualGroup = new Adw.PreferencesGroup({
        title: _('By hand'),
        description: _('The same steps, to paste in a terminal. They do exactly what the Install button does.'),
        visible: false,
    });
    const text = new Gtk.TextView({
        editable: false, monospace: true, wrap_mode: Gtk.WrapMode.CHAR,
        top_margin: 10, bottom_margin: 10, left_margin: 10, right_margin: 10,
    });
    text.add_css_class('card');
    manualGroup.add(text);
    const copy = new Gtk.Button({label: _('Copy Commands'), halign: Gtk.Align.START, margin_top: 8});
    copy.connect('clicked', () => {
        Gdk.Display.get_default().get_clipboard().set_content(Gdk.ContentProvider.new_for_bytes(
            'text/plain;charset=utf-8', new GLib.Bytes(new TextEncoder().encode(manualLines.join('\n')))));
        copy.label = _('Copied');
    });
    manualGroup.add(copy);
    page.add(manualGroup);

    // -- remove ---------------------------------------------------------------------------
    const removeGroup = new Adw.PreferencesGroup({
        description: _('Stops the engine and removes it from your home folder. Your pairing and settings stay, in case you install it again.'),
    });
    const remove = new Adw.ButtonRow({title: _('Uninstall the Engine…')});
    remove.add_css_class('destructive-action');
    removeGroup.add(remove);
    page.add(removeGroup);

    // -- drawing the state ------------------------------------------------------------------
    const drawing = name => {
        const file = dir.get_child('illustrations').get_child(`engine-${DRAWINGS[name] ?? 'download'}.svg`);
        picture.set_file(file);
    };

    const button = (label, style, callback) => {
        const b = new Gtk.Button({label, valign: Gtk.Align.CENTER});
        if (style)
            b.add_css_class(style);
        b.add_css_class('pill');
        b.connect('clicked', callback);
        buttons.append(b);
        return b;
    };

    const clearButtons = () => {
        buttons.remove_all();
    };

    const setManual = lines => {
        manualLines = lines;
        text.buffer.text = lines.join('\n');
        copy.label = _('Copy Commands');
    };

    // The commands by hand, with the real paths, each quoted.
    const installLines = () => manualCommands(release, paths, GLib.shell_quote);
    const rollbackLines = () => manualRollbackCommands(paths, GLib.shell_quote);
    // The system step alone: the right landline-setup, its pinned sums
    // checked before sudo when there are any; the whole install when no
    // engine is there.
    const systemLines = () => {
        const where = manualSystemWhere(paths, release);
        return where ? manualSystemCommands(release, where, GLib.shell_quote) : installLines();
    };

    const showManual = (lines = installLines()) => {
        setManual(lines);
        manualShown = true;
        manualGroup.visible = true;
    };

    const errorWords = (flow) => {
        const back = flow.canRollback;
        return {
            network: [_('The download did not work'),
                _('Check that this computer is online, then try again. Nothing was changed.')],
            mismatch: [_('This download can’t be trusted'),
                releaseIsPinned(release)
                    ? _('It is not the file this version of Landline expects, so it was thrown away. Nothing was changed.')
                    : _('This build of Landline has no engine release pinned yet, so no download can pass the check. Nothing was changed.')],
            contents: [_('The download is not an engine'), _('Nothing was changed.')],
            unpack: [_('The engine could not be unpacked'),
                _('Nothing was changed. Try again; if it happens again, install it by hand.')],
            write: [_('The engine could not be copied'),
                _('Is the disk full? Try again, or install it by hand.')],
            systemd: [_('The engine could not be started'),
                _('The service manager of your session did not take it.') + (back ? ` ${_('You can go back to the engine you had.')}` : '')],
            silent: [_('The new engine does not answer'),
                back ? _('It is installed but stayed silent. Go back to the engine you had: it worked.')
                    : _('It is installed but stayed silent. Try again, or look at the steps by hand.')],
            cancelled: [_('Stopped'), _('Nothing is broken. Start again whenever you like.')],
            password: [_('No password, nothing changed'),
                _('Landline runs without these settings, but some iPhones may not link. Come back here any time.')],
            system: [_('The Bluetooth settings could not be changed'),
                _('Landline runs without them, but some iPhones may not link. Try again, or change them by hand.')],
        }[flow.error] ?? [_('Something went wrong'), _('Try again, or install it by hand.')];
    };

    const restWords = () => {
        const v = installed.version;
        switch (installed.state) {
        case 'absent':
            return [_('Install Landline’s engine'),
                _('A small background service that talks to your iPhone. It goes in your home folder in a few seconds, and asks for your password once, at the end.')];
        case 'interrupted':
            return [_('The engine is missing'),
                _('An update stopped half-way. Put back the engine you had, or install it again.')];
        case 'manual':
        case 'unknown':
            return [_('An engine is already here'),
                // Translators: %s is a version number.
                _('It was installed by hand. Replace it with version %s? The one you have now is kept, so you can go back.').replace('%s', release.version)];
        case 'outdated':
            return [_('An update is ready'),
                // Translators: the first %s is the new version, the second the one installed.
                _('Version %s replaces version %s. The one you have now is kept, so you can go back.').replace('%s', release.version).replace('%s', v)];
        case 'system':
            return [_('The engine is installed'),
                v ? // Translators: %s is a version number.
                    _('Version %s, installed with your system’s packages. It is updated with them.').replace('%s', v)
                    : _('Installed with your system’s packages. It is updated with them.')];
        case 'newer':
            return [_('The engine is up to date'),
                // Translators: %s is a version number.
                _('Version %s, newer than this extension knows. Nothing to do.').replace('%s', v)];
        default:
            return [_('The engine is installed'),
                // Translators: %s is a version number.
                _('Version %s, up to date.').replace('%s', v)];
        }
    };

    const render = () => {
        const flow = installer.flow;
        const step = activeStep(flow);
        // An install, the password step, a rollback or an uninstall is
        // running: nothing else may move the engine's files meanwhile.
        const working = busy || installer.busy;
        clearButtons();
        bar.visible = false;
        buttons.sensitive = true;
        remove.sensitive = !working;

        // The rows.
        for (const id of STEPS) {
            const {row, stack, icon} = rows[id];
            const s = flow.steps[id];
            const on = s.state === 'active';
            stack.visible_child_name = on ? 'spinner' : 'icon';
            icon.icon_name = id === 'phone' && s.state === 'waiting' ? 'phone-symbolic' : ICONS[s.state] ?? ICONS.pending;
            for (const c of ['success', 'error', 'dim-label', 'accent'])
                icon.remove_css_class(c);
            icon.add_css_class({done: 'success', failed: 'error', pending: 'dim-label', skipped: 'dim-label', waiting: 'accent'}[s.state] ?? 'dim-label');
            row.subtitle = s.state === 'failed' ? errorWords(flow)[0]
                : id === 'system' && s.state === 'done' && s.detail === 'already' ? _('Already right on this computer')
                    : id === 'system' && s.state === 'skipped' ? _('Skipped for now')
                        : id === 'download' && s.state === 'done' && s.detail === 'reused' ? _('Already downloaded')
                            : id === 'phone' && s.state === 'done' ? _('Your iPhone is linked')
                            : labels[id][1];
        }

        // The top.
        if ((flow.running || installer.busy) && step) {
            drawing(step);
            title.label = {
                download: _('Downloading…'),
                verify: _('Checking the download…'),
                install: _('Installing…'),
                start: _('Starting the engine…'),
                system: _('Changing the Bluetooth settings…'),
            }[step] ?? '';
            body.label = {
                download: _('From GitHub, the version this extension was made for.'),
                verify: _('Its fingerprint must match the one written in Landline.'),
                install: _('Into your home folder. The engine you had stays beside it.'),
                start: _('Waiting for it to answer. This takes a few seconds.'),
                system: _('Waiting for your password.'),
            }[step] ?? '';
            if (step === 'download') {
                bar.visible = true;
                bar.fraction = flow.fraction;
            }
            if (step !== 'system' && flow.running)
                button(_('Cancel'), null, () => installer.cancel());
        } else if (flow.failedAt) {
            drawing(flow.failedAt);
            [title.label, body.label] = errorWords(flow);
            for (const offer of remedies(flow))
                offerButton(offer, flow);
        } else if (flow.rolledBack) {
            drawing('start');
            title.label = _('Back to the engine you had');
            body.label = _('It is running again. The new one was removed; you can try the update again later.');
        } else if (flow.steps.system.state === 'waiting') {
            drawing('system');
            const manual = flow.steps.system.detail === 'manual';
            title.label = _('One last thing: your password');
            body.label = manual
                ? _('Two Bluetooth settings of this computer must change for iPhones. This engine was installed by hand, so they are changed by hand too: copy the command and run it in a terminal.')
                : _('Two Bluetooth settings of this computer must change for iPhones. The system asks for your administrator password once.');
            for (const offer of remedies(flow))
                offerButton(offer, flow);
        } else if (flow.finished && flow.steps.phone.state === 'done') {
            drawing('phone');
            title.label = _('Your iPhone is linked');
            body.label = _('Landline is ready. You can close this window.');
            button(_('Close'), 'suggested-action', () => window.close());
        } else if (flow.finished) {
            drawing('phone');
            title.label = _('The engine is running');
            body.label = _('Now link your iPhone: the pictures in the top bar show you where to tap.');
            button(_('Link My iPhone…'), 'suggested-action', () => {
                settings.set_boolean('tour-request', true);
                window.close();
            });
        } else {
            const [t, b] = restWords();
            title.label = t;
            body.label = b;
            drawing(['current', 'newer', 'system'].includes(installed.state) ? 'start' : 'download');
            const action = installed.action;
            if (action === 'restore')
                button(_('Put Back'), 'suggested-action', () => rollback());
            const label = {
                install: _('Install'), restore: _('Install Again'), replace: _('Replace'),
                update: _('Update'), reinstall: _('Reinstall'),
            }[action];
            if (label) {
                button(label, action === 'reinstall' || action === 'restore' ? null : 'suggested-action',
                    () => start(action));
            }
            if (installed.canRollback && ['current', 'outdated', 'newer'].includes(installed.state))
                button(_('Use Previous Engine'), 'flat', () => rollback());
        }
        // While something runs, only Cancel of an install stays usable.
        if (working && !flow.running)
            buttons.sensitive = false;
        removeGroup.visible = installed.state !== 'system' && (installed.installed || installed.canRollback);
        manualGroup.visible = manualShown;
        stepsGroup.visible = flow.running || flow.failedAt !== null || flow.finished ||
            flow.steps.system.state === 'waiting' || flow.rolledBack;
    };

    function offerButton(offer, flow) {
        switch (offer) {
        case 'retry':
            button(_('Try Again'), 'suggested-action', () => go(retryFrom(flow)));
            break;
        case 'rollback':
            button(_('Use Previous Engine'), 'suggested-action', () => rollback());
            break;
        case 'manual':
            button(_('Show Steps by Hand'), 'flat', () => {
                showManual(flow.failedAt === 'start' && flow.canRollback
                    ? rollbackLines()
                    : flow.steps.system.detail === 'manual' || flow.failedAt === 'system'
                        ? systemLines()
                        : installLines());
                render();
            });
            break;
        case 'password':
            button(_('Continue…'), 'suggested-action', () => askPassword());
            break;
        case 'skip':
            button(_('Skip for Now'), 'flat', () => installer.systemSkip());
            break;
        default:
            break;
        }
    }

    // -- actions --------------------------------------------------------------------------

    const refresh = () => {
        installed = detect(paths, release);
    };

    installer.onChange = () => {
        if (!cancellable.is_cancelled())
            render();
    };

    async function go(from) {
        if (busy || installer.busy)
            return;
        busy = true;
        manualShown = false;
        if (from === 'system') {
            busy = false;
            await askPassword();
            return;
        }
        const flow = await installer.run(from);
        busy = false;
        if (cancellable.is_cancelled())
            return;
        refresh();
        if (flow.steps.start.state === 'done')
            await checkSystem(true);
        render();
    }

    function start(action) {
        if (action !== 'replace') {
            go('download').catch(warn);
            return;
        }
        const dialog = new Adw.AlertDialog({
            heading: _('Replace the engine installed by hand?'),
            body: _('It is kept beside the new one, and “Use Previous Engine” brings it back.'),
        });
        dialog.add_response('cancel', _('Cancel'));
        dialog.add_response('replace', _('Replace'));
        dialog.set_response_appearance('replace', Adw.ResponseAppearance.SUGGESTED);
        dialog.connect('response', (_d, response) => {
            if (response === 'replace')
                go('download').catch(warn);
        });
        dialog.present(window);
    }

    async function rollback() {
        if (busy || installer.busy)
            return;
        busy = true;
        render();
        try {
            await installer.rollback();
        } catch (e) {
            warn(e, 'going back');
            showManual(rollbackLines());
        }
        busy = false;
        refresh();
        render();
    }

    // Can the system step be done from here: the engine of the pinned
    // release, with the two files the script checks.
    // The engine of a distribution package: its setup helper is already a
    // file of root, run as it is.
    const packaged = () => installed.state === 'system';
    const canBootstrap = () => packaged() || releaseIsPinned(release) && installed.version === release.version &&
        GLib.file_test(paths.setup, GLib.FileTest.IS_REGULAR) &&
        GLib.file_test(paths.leParams, GLib.FileTest.IS_REGULAR);

    // After a start, or when the page shows an installed engine: are the
    // Bluetooth settings right? inFlow: move the steps on.
    async function checkSystem(inFlow = false) {
        const status = await helper.status();
        if (cancellable.is_cancelled())
            return;
        const ready = status?.ready === true && status?.le_params === true;
        if (inFlow) {
            if (ready)
                installer.systemReady();
            else
                installer.systemNeeded(!canBootstrap());
        } else if (!ready && !installer.flow.running && installer.flow.steps.system.state === 'pending' &&
            ['current', 'system'].includes(installed.state) && status !== null) {
            // Installed earlier, settings still to do: offer them.
            installer.systemNeeded(!canBootstrap(), true);
        }
    }

    async function askPassword() {
        if (busy)
            return;
        if (!canBootstrap()) {
            showManual(systemLines());
            render();
            return;
        }
        busy = true;
        installer.systemAsking();
        let result = 'failed';
        try {
            if (packaged()) {
                result = await runAsRoot(await rootHelper(cancellable), 'apply', cancellable);
                if (result === 'unavailable')
                    result = 'failed';
            } else {
                const argv = bootstrapArgv(bootstrapScript(release), paths.setup, paths.leParams);
                const {exited, status} = await run(argv, cancellable);
                result = exited ? bootstrapResult(status) : 'failed';
            }
        } catch (e) {
            if (!cancellable.is_cancelled())
                warn(e, 'system step');
        }
        busy = false;
        if (cancellable.is_cancelled())
            return;
        installer.systemResult(result === 'mismatch' ? 'failed' : result);
    }

    remove.connect('activated', () => {
        if (busy || installer.busy)
            return;
        const dialog = new Adw.AlertDialog({
            heading: _('Uninstall the engine?'),
            body: _('Landline stops working until it is installed again. Your pairing and settings are kept.'),
        });
        dialog.add_response('cancel', _('Cancel'));
        dialog.add_response('remove', _('Uninstall'));
        dialog.set_response_appearance('remove', Adw.ResponseAppearance.DESTRUCTIVE);
        let root = null;
        rootHelper(cancellable).then(path => {
            root = path;
            if (path && !cancellable.is_cancelled()) {
                dialog.add_response('revert', _('Uninstall and Undo Bluetooth Changes…'));
                dialog.set_response_appearance('revert', Adw.ResponseAppearance.DESTRUCTIVE);
            }
        });
        dialog.connect('response', (_d, response) => {
            if (response !== 'remove' && response !== 'revert')
                return;
            if (busy || installer.busy)
                return;
            (async () => {
                busy = true;
                render();
                // The system part first, while its helper is still known.
                try {
                    if (response === 'revert' && root)
                        await runAsRoot(root, 'revert', cancellable);
                    await installer.uninstall();
                } finally {
                    busy = false;
                }
                manualShown = false;
                refresh();
                render();
            })().catch(e => warn(e, 'uninstall'));
        });
        dialog.present(window);
    });

    window.connect('close-request', () => {
        cancellable.cancel();
        installer.destroy();
        helper.destroy();
        return false;
    });

    render();
    if (['current', 'system'].includes(installed.state))
        checkSystem().catch(warn);
    return page;
}
