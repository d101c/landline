// SPDX-License-Identifier: GPL-2.0-or-later
// "Use Landline for phone links" (#50), in Phone Settings: the desktop file
// that receives tel: and callto: links, and the defaults of the user.
// Used by the preferences window, and by the Shell for the one offer of the
// first run (only when no app opens these links: nothing is replaced).
//
// Turned on: the file is written in the user's applications folder and
// made the default for both schemes; whatever was the default before (the
// user's own choice, or the system's) is remembered in the key
// `phone-links-previous`. Turned off: the user's associations of the two
// schemes are reset, the remembered default is put back if it is still
// installed, and the file is removed. Phone Settings asks before it
// replaces a default that is not Landline's.

import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';

import {DESKTOP_ID, SCHEMES, desktopEntry} from './telLinks.js';

Gio._promisify(Gio.File.prototype, 'replace_contents_async');
Gio._promisify(Gio.File.prototype, 'delete_async');
Gio._promisify(Gio.File.prototype, 'make_directory_async');
Gio._promisify(Gio.AppInfo, 'get_default_for_uri_scheme_async', 'get_default_for_uri_scheme_finish');

const type = scheme => `x-scheme-handler/${scheme}`;

export function handlerFile() {
    return Gio.File.new_for_path(GLib.build_filenamev([GLib.get_user_data_dir(), 'applications', DESKTOP_ID]));
}

async function defaultFor(scheme) {
    try {
        return await Gio.AppInfo.get_default_for_uri_scheme_async(scheme, null);
    } catch (e) {
        return null;
    }
}

// {ours: Landline opens tel: links now, others: [{scheme, id, name}] the
// defaults that are another app's}.
export async function phoneLinksState() {
    const others = [];
    let ours = false;
    for (const scheme of SCHEMES) {
        const info = await defaultFor(scheme);
        if (!info)
            continue;
        if (info.get_id() === DESKTOP_ID) {
            if (scheme === 'tel')
                ours = true;
        } else {
            others.push({scheme, id: info.get_id(), name: info.get_display_name?.() ?? info.get_name()});
        }
    }
    return {ours, others};
}

// The desktop file as the system sees it once it is written: the list of
// applications is reread a moment after a file appears.
async function ourAppInfo() {
    for (let i = 0; i < 30; i++) {
        const info = GioUnix.DesktopAppInfo.new(DESKTOP_ID);
        if (info)
            return info;
        await new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, 100, () => {
            resolve();
            return GLib.SOURCE_REMOVE;
        }));
    }
    return null;
}

// Throws when the file cannot be written or made the default; then what
// was done is undone (#56): no file, the defaults as they were, the option
// off. setDefault(info, type) is for the tests.
export async function usePhoneLinks(settings, {name, comment},
    setDefault = (info, contentType) => info.set_as_default_for_type(contentType)) {
    if (settings.get_boolean('phone-links')) {
        await writeHandler(settings, {name, comment}, setDefault);
        return;
    }
    const taken = [];
    try {
        await writeHandler(settings, {name, comment}, setDefault, taken);
    } catch (e) {
        await undo(taken).catch(() => {});
        settings.set_strv('phone-links-previous', []);
        settings.set_boolean('phone-links', false);
        throw e;
    }
}

// Back to before a failed usePhoneLinks(): the file goes, the schemes it
// took get their associations reset and the user's own default back.
async function undo(taken) {
    try {
        await handlerFile().delete_async(GLib.PRIORITY_DEFAULT, null);
    } catch (e) {
        if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
            throw e;
    }
    for (const {scheme, previous} of taken) {
        Gio.AppInfo.reset_type_associations(type(scheme));
        const back = previous ? GioUnix.DesktopAppInfo.new(previous) : null;
        if (back && Gio.AppInfo.get_default_for_type(type(scheme), false)?.get_id() !== previous)
            back.set_as_default_for_type(type(scheme));
    }
}

// taken: filled with {scheme, previous} as each scheme is made Landline's.
async function writeHandler(settings, {name, comment}, setDefault, taken = []) {
    const previous = [];
    const before = new Map();
    for (const scheme of SCHEMES) {
        const info = await defaultFor(scheme);
        if (info && info.get_id() !== DESKTOP_ID) {
            previous.push(`${scheme}=${info.get_id()}`);
            before.set(scheme, info.get_id());
        }
    }
    const file = handlerFile();
    try {
        await file.get_parent().make_directory_async(GLib.PRIORITY_DEFAULT, null);
    } catch (e) {
        if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.EXISTS))
            throw e;
    }
    await file.replace_contents_async(new TextEncoder().encode(desktopEntry(name, comment)), null, false,
        Gio.FileCreateFlags.REPLACE_DESTINATION, null);
    const info = await ourAppInfo();
    if (!info)
        throw new Error('the phone links file was written but is not seen as an application');
    for (const scheme of SCHEMES) {
        taken.push({scheme, previous: before.get(scheme) ?? null});
        setDefault(info, type(scheme));
    }
    // Remembered once: turned on twice, the first default is the one to
    // give back.
    if (settings.get_strv('phone-links-previous').length === 0 || !settings.get_boolean('phone-links'))
        settings.set_strv('phone-links-previous', previous);
    settings.set_boolean('phone-links', true);
}

export async function stopPhoneLinks(settings) {
    const previous = new Map(settings.get_strv('phone-links-previous')
        .map(entry => entry.split('=')).filter(([scheme, id]) => scheme && id));
    // Already given to another app by the user since: theirs, left alone.
    const ours = [];
    for (const scheme of SCHEMES) {
        const info = await defaultFor(scheme);
        if (!info || info.get_id() === DESKTOP_ID)
            ours.push(scheme);
    }
    // The file goes first: while it is there, it could still be picked as
    // the default of a scheme left without one.
    try {
        await handlerFile().delete_async(GLib.PRIORITY_DEFAULT, null);
    } catch (e) {
        if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
            throw e;
    }
    for (const scheme of ours) {
        Gio.AppInfo.reset_type_associations(type(scheme));
        const wanted = previous.get(scheme);
        const back = wanted ? GioUnix.DesktopAppInfo.new(wanted) : null;
        // A default of the system comes back by itself with the reset; only
        // the user's own choice is written again.
        if (back && Gio.AppInfo.get_default_for_type(type(scheme), false)?.get_id() !== wanted)
            back.set_as_default_for_type(type(scheme));
    }
    settings.set_strv('phone-links-previous', []);
    settings.set_boolean('phone-links', false);
}
