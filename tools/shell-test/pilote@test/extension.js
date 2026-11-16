// SPDX-License-Identifier: GPL-2.0-or-later
// Pilote d'essai pour tools/shell-test/run.sh. Il lit $PILOTE_DIR/cmd, exécute
// la commande qui s'y trouve, puis supprime le fichier :
//   click                 vrai clic de souris au centre de l'indicateur
//   press <nom> [n]       vrai clic sur un contrôle de la carte ouverte
//                         (answer, decline, hangUp, toPhone, toComputer, send,
//                         close, differ, match, action, settings, calls,
//                         chips n, rows n)
//   type <texte>          écrit dans le champ de réponse
//   key <Return|Escape>   vraie touche du clavier
//   state <nom>           écrit ce que montre l'extension dans <nom>.json
//   shot <nom>            capture de tout l'écran dans <nom>.png
//   accent <on|off>       couleur d'accent du système
//   tray | closetray      ouvre / ferme la liste des notifications
//   dpress <n>            vrai clic sur le n-ième bouton du parcours d'accueil
//   tpress <nom> [n]      vrai clic sur un contrôle nommé du parcours (cancel,
//                         continue, match, differ, repaired, next, back, done,
//                         retry, candidates n, help, fix, forget, confirm, again)
//   langue <fr|en|auto>   langue de l'iPhone (réglage phone-language)
//   forgetrequest         ce que fait le bouton « Forget This iPhone… » de
//                         Phone Settings
//   fakeauth open|close   ouvre / ferme une fenêtre modale du Shell au-dessus du
//                         parcours, comme le fait la fenêtre de mot de passe
//   expand <n>            vrai clic sur la n-ième bulle de la carte de réponse
//   nohelper | helper [user]  l'assistant système manque / est installé chez root /
//                         n'existe que sans droits (helper user) ; puis le relit
//   clearall              vide la liste des notifications
//   dnd <on|off>          le « Ne pas déranger » de GNOME (show-banners)
//   sharing <on|off|auto> fait comme si l'écran était partagé (aucun partage réel
//                         n'est possible dans ce Shell sans écran)
//   reglage <clé> <on|off>  un réglage booléen de l'extension
//   hotspot <profil|sans-profil|refuse|accepte>
//                         doublure de NetworkManager pour « Use iPhone's Internet
//                         Connection » : un profil existe ou non pour l'iPhone, la
//                         connexion aboutit ou retombe
//   hoverbanner           amène le pointeur sur la bulle de notification, qui se
//                         déplie et montre ses boutons
//   steady <on|off>       indicateur stable (délai de 3 s, verrou « Link again ») ;
//                         coupé pour les parties écrites avant lui
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';
import * as Dialog from 'resource:///org/gnome/shell/ui/dialog.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as ModalDialog from 'resource:///org/gnome/shell/ui/modalDialog.js';

import Pango from 'gi://Pango';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

const UUID = 'landline@d101c.github.io';

// L'extension livrée ne porte rien pour les essais : le pilote retrouve
// lui-même les contrôles, par leur libellé ou leur classe de style.
const all = (root, test) => {
    const out = [];
    const walk = a => {
        if (test(a))
            out.push(a);
        a.get_children().forEach(walk);
    };
    if (root)
        walk(root);
    return out.filter(a => a.mapped);
};
const hasClass = (a, name) => a.has_style_class_name?.(name) === true;
const textOf = a => a instanceof St.Label ? a.text : a instanceof St.Button ? a.label : null;
// L'acteur cliquable (bouton ou ligne de menu) qui porte un de ces libellés.
const labelled = (...labels) => root => {
    for (const actor of all(root, a => labels.includes(textOf(a)))) {
        for (let a = actor; a && a !== root.get_parent(); a = a.get_parent()) {
            if (a instanceof St.Button || a instanceof PopupMenu.PopupBaseMenuItem)
                return a;
        }
    }
    return null;
};
const styled = name => root => all(root, a => hasClass(a, name))[0] ?? null;
const everyStyled = name => root => {
    const found = all(root, a => hasClass(a, name));
    return found.length ? found : null;
};

// Les contrôles d'une carte, dans l'ordre où les essais les attendent.
const CARD_CONTROLS = {
    close: root => all(root, a => a instanceof St.Button && hasClass(a, 'landline-round'))[0] ?? null,
    entry: root => all(root, a => a instanceof St.Entry)[0] ?? null,
    send: styled('landline-send'),
    chips: everyStyled('landline-chip'),
    bubbles: styled('landline-bubbles'),
    results: everyStyled('landline-result'),
    decline: labelled('Decline'),
    answer: labelled('Answer', 'Answer here'),
    answerHere: labelled('Answer here'),
    answerPhone: labelled('Answer on iPhone'),
    toPhone: root => all(root, a => a instanceof St.Button && a.label === 'iPhone')[0] ?? null,
    toComputer: labelled('This computer'),
    hangUp: labelled('Hang Up'),
    differ: labelled('They Differ'),
    match: labelled('They Match'),
    action: labelled('Copy Commands', 'Try Again'),
    top: root => all(root, a => a.constructor.name === 'PhoneTopRow')[0] ?? null,
    permission: labelled('Finish Setting Up This Computer…'),
    setup: labelled('Continue Setup…'),
    fix: labelled('Fix It'),
    hotspot: labelled('Use iPhone’s Internet Connection'),
    hotspotLine: root => {
        const item = all(root, a => hasClass(a, 'landline-under-switch'))[0];
        return item ? all(item, a => a instanceof St.Label)[0] ?? null : null;
    },
    phoneAudio: labelled('Play iPhone audio on this computer'),
    mute: labelled('Mute iPhone on this computer'),
    compose: labelled('New Message…'),
    forget: labelled('Forget This iPhone…'),
    rows: root => {
        const rows = all(root, a => a instanceof PopupMenu.PopupBaseMenuItem && hasClass(a, 'landline-row') &&
            all(a, c => hasClass(c, 'landline-avatar')).length > 0);
        return rows.length ? rows : null;
    },
    calls: labelled('Recent calls'),
    settings: labelled('Phone Settings'),
};

// Ceux du parcours : les boutons du bas par leur libellé, le reste par sa classe.
const TOUR_CONTROLS = {
    cancel: labelled('Back', 'Cancel', 'Close', 'Finish Later', 'Not Now'),
    back: labelled('Back'),
    continue: labelled('Continue', 'Try Again'),
    confirm: labelled('Forget', 'Try Again'),
    retry: labelled('Try Again'),
    next: labelled('Continue', 'Skip'),
    differ: labelled('They Differ'),
    match: labelled('They Match'),
    repaired: labelled('Done on My iPhone'),
    done: labelled('Done'),
    again: labelled('Link Again'),
    fix: labelled('Fix It'),
    copy: labelled('Copy Commands'),
    forget: labelled('Forget This iPhone…'),
    help: root => all(root, a => a instanceof St.Button && a.accessible_name === 'Don’t see these switches?')[0] ?? null,
    candidates: everyStyled('landline-action'),
};

const controlsOf = (root, table) => {
    const found = {};
    for (const [name, find] of Object.entries(table)) {
        const actor = find(root);
        if (actor)
            found[name] = actor;
    }
    return found;
};

export default class Pilote {
    enable() {
        this._dir = GLib.getenv('PILOTE_DIR');
        this._doubles().catch(e => {
            logError(e, 'pilote');
            GLib.file_set_contents(`${this._dir}/error`, `doublures : ${e.message}\n`);
        });
        const seat = global.stage.context.get_backend().get_default_seat();
        this._pointer = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
        this._keyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this._id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => this._tick());
    }

    disable() {
        GLib.source_remove(this._id);
        this._auth?.destroy();
        this._auth = null;
        this._pointer = null;
        this._keyboard = null;
    }

    // Avant que l'extension ne soit activée : ses modules sont chargés ici (le
    // Shell reprendra les mêmes), et ce qui toucherait au vrai système reçoit
    // une doublure. Rien de cela n'existe dans l'extension livrée.
    async _doubles() {
        const base = GLib.filename_to_uri(
            GLib.build_filenamev([GLib.get_user_data_dir(), 'gnome-shell', 'extensions', UUID, 'lib']), null);
        const fake = GLib.getenv('PILOTE_SETUP');
        this._helperMode = 'root';     // 'root' : copie de root ; 'user' : copie sans droits ; 'none'
        const {SetupHelper} = await import(`${base}/setupHelper.js`);
        SetupHelper.prototype._rootCopy = () => this._helperMode === 'root' ? fake : null;
        SetupHelper.prototype._statusPath = () => this._helperMode === 'none' ? null : fake;
        // NetworkManager vit sur le bus système, qui est le vrai : jamais joint.
        const {NetworkManagerBackend} = await import(`${base}/hotspot.js`);
        NetworkManagerBackend.prototype._start = async function () {};
        // Aucun écran à partager dans ce Shell : le partage se décrète.
        this._sharing = null;
        const pilote = this;
        const {QuietWatch} = await import(`${base}/quiet.js`);
        const real = Object.getOwnPropertyDescriptor(QuietWatch.prototype, 'sharing').get;
        Object.defineProperty(QuietWatch.prototype, 'sharing', {
            get() {
                return pilote._sharing ?? real.call(this);
            },
        });
        GLib.file_set_contents(`${this._dir}/doublures`, 'ok\n');
    }

    // La carte ouverte (menu, ou carte montrée sans prendre le clavier).
    _card(ind) {
        const menu = ind.menu.isOpen ? ind.menu : ind._passive?.isOpen ? ind._passive : null;
        return menu ? controlsOf(menu.box, CARD_CONTROLS) : {};
    }

    _tourControls() {
        const tour = this._ext()?._tourDialog;
        return tour ? controlsOf(tour, TOUR_CONTROLS) : {};
    }

    _geometry(row) {
        if (!row)
            return null;
        const span = actor => {
            if (!actor)
                return null;
            const [x, y] = actor.get_transformed_position();
            const [w, h] = actor.get_transformed_size();
            return {x: Math.round(x), width: Math.round(w), y: Math.round(y), height: Math.round(h)};
        };
        const baseline = label => label
            ? Math.round(label.get_transformed_position()[1] +
                label.clutter_text.get_layout().get_baseline() / Pango.SCALE)
            : null;
        return {
            row: span(row),
            glyph: span(row.glyph),
            name: span(row.nameLabel),
            state: span(row.stateLabel),
            battery: span(row.batteryLabel),
            nameText: row.nameLabel.text,
            nameCut: row.nameLabel.clutter_text.get_layout().is_ellipsized(),
            baselines: [baseline(row.nameLabel), baseline(row.stateLabel), baseline(row.batteryLabel)],
        };
    }

    _ext() {
        return Main.extensionManager.lookup(UUID)?.stateObj ?? null;
    }

    _tick() {
        const path = `${this._dir}/cmd`;
        if (!GLib.file_test(path, GLib.FileTest.EXISTS))
            return GLib.SOURCE_CONTINUE;
        const [, bytes] = GLib.file_get_contents(path);
        const text = new TextDecoder().decode(bytes).trim();
        if (text === '')
            return GLib.SOURCE_CONTINUE;
        const [cmd, ...args] = text.split(/\s+/);
        try {
            this._run(cmd, args, text.slice(cmd.length).trim());
        } catch (e) {
            logError(e, 'pilote');
            GLib.file_set_contents(`${this._dir}/error`, `${cmd}: ${e.message}\n`);
        }
        GLib.unlink(path);
        return GLib.SOURCE_CONTINUE;
    }

    _clickActor(actor) {
        if (!actor || !actor.mapped)
            throw new Error('nothing to click');
        const [x, y] = actor.get_transformed_position();
        const [w, h] = actor.get_transformed_size();
        const t = GLib.get_monotonic_time();
        this._pointer.notify_absolute_motion(t, x + w / 2, y + h / 2);
        this._pointer.notify_button(t + 1000, Clutter.BUTTON_PRIMARY, Clutter.ButtonState.PRESSED);
        this._pointer.notify_button(t + 2000, Clutter.BUTTON_PRIMARY, Clutter.ButtonState.RELEASED);
    }

    _state() {
        const ext = this._ext();
        const ind = ext?._indicator;
        if (!ind)
            return {enabled: false, inPanel: Main.panel.statusArea[UUID] !== undefined};
        const right = Main.panel._rightBox.get_children();
        const focus = global.stage.get_key_focus();
        const tour = ext._tourDialog;
        const controls = this._card(ind);
        const bridge = ext._bridge;
        const right0 = right.filter(a => a !== ind.container).map(a => Math.round(a.get_transformed_position()[0]));
        return {
            // Le parcours d'accueil : ouvert, au premier plan, clavier dessus.
            tourDialog: !!tour,
            tourButtons: tour?.buttonLayout.get_children().map(b => b.label) ?? [],
            tourControls: Object.keys(this._tourControls()),
            tourStepLabel: tour?._stepLabel.text ?? null,
            tourWhere: tour?._whereLabel.text ?? null,
            tourHeight: tour ? Math.round(tour.dialogLayout._dialog.height) : 0,
            tourTexts: tour ? this._texts(tour._body) : [],
            phoneLanguage: ext._illustrations?.language ?? null,
            focusInTour: !!tour && !!focus && tour.contains(focus),
            focusLabel: focus?.label ?? null,
            grabIsTour: !!tour && global.stage.get_grab_actor() === tour,
            authOpen: !!this._auth,
            neighbours: right0,
            pillWidth: Math.round(ind._pill.width),
            slot: ind._slot.visible,
            menuWidth: Math.round(ind.menu.box.width),
            // La première ligne du menu au repos : où sont le nom, l'état, la batterie.
            header: ind.menu.isOpen ? this._geometry(controls.top) : null,
            bubbles: controls.bubbles?.get_children().map(b => [b.reactive, Math.round(b.height)]) ?? [],
            enabled: true,
            view: ext._model.view(),
            box: 'right',
            indexInRightBox: right.indexOf(ind.container),
            rightBoxCount: right.length,
            width: Math.round(ind.width),
            height: Math.round(ind.height),
            panelHeight: Math.round(Main.panel.height),
            strong: ind._strong.visible ? ind._strong.text : '',
            text: ind._text.visible ? ind._text.text : '',
            count: ind._count.visible ? ind._count.text : '',
            dot: ind._dot.visible,
            classes: ind.get_style_class_name(),
            menuOpen: ind.menu.isOpen,
            passiveOpen: ind._passive.isOpen,
            controls: Object.keys(controls),
            menuTexts: ind.menu.isOpen ? this._texts(ind.menu.box) : [],
            focusInEntry: !!controls.entry && controls.entry.clutter_text === focus,
            entryText: controls.entry?.text ?? null,
            notifications: bridge._notifications.size,
            replyLinks: Object.fromEntries([...bridge._notifications]
                .filter(([, entry]) => entry.replyUri).map(([uid, entry]) => [uid, entry.replyUri])),
            hotspotLine: controls.hotspotLine?.text ?? null,
            bannerTexts: Main.messageTray._banner ? this._texts(Main.messageTray._banner) : [],
            sources: [...ext._bridge._sources.values()].map(s => s.title),
            trayTitles: Main.messageTray.getSources().flatMap(s => s.notifications.map(n => `${s.title}: ${n.title}`)),
            timer: ext._timerId !== 0,
            engine: ext._client.state,
            windows: global.get_window_actors().length,
            banner: Main.messageTray._notification?.title ?? null,
        };
    }

    _texts(actor) {
        const out = [];
        const walk = a => {
            if (a instanceof St.Label && a.text)
                out.push(a.text);
            else if (a instanceof St.Button && a.label)
                out.push(a.label);
            a.get_children().forEach(walk);
        };
        walk(actor);
        return out;
    }

    _run(cmd, args, rest) {
        const ind = this._ext()?._indicator;
        switch (cmd) {
        case 'click':
            this._clickActor(ind);
            break;
        case 'press': {
            let target = this._card(ind)[args[0]];
            if (Array.isArray(target))
                target = target[Number(args[1] ?? 0)];
            this._clickActor(target);
            break;
        }
        case 'dpress':
            this._clickActor(this._ext()._tourDialog.buttonLayout.get_children()[Number(args[0])]);
            break;
        case 'tpress': {
            let target = this._tourControls()[args[0]];
            if (Array.isArray(target))
                target = target[Number(args[1] ?? 0)];
            this._clickActor(target);
            break;
        }
        case 'expand':
            this._clickActor(this._card(ind).bubbles.get_children()[Number(args[0] ?? 0)]);
            break;
        case 'fakeauth':
            if (args[0] === 'open') {
                // La fenêtre de mot de passe du Shell est une ModalDialog
                // ouverte par-dessus ce qui est déjà là : celle-ci fait pareil.
                this._auth = new ModalDialog.ModalDialog({destroyOnClose: true});
                this._auth.contentLayout.add_child(new Dialog.MessageDialogContent({
                    title: 'Authentication Required',
                    description: 'Stand-in for the password window of the system (test only).',
                }));
                const entry = new St.PasswordEntry({style_class: 'prompt-dialog-password-entry', can_focus: true});
                this._auth.contentLayout.add_child(entry);
                this._auth.setInitialKeyFocus(entry);
                this._auth.setButtons([{label: 'Cancel', action: () => {}}, {label: 'Authenticate', action: () => {}, default: true}]);
                this._auth.open();
            } else {
                this._auth?.close();
                this._auth = null;
            }
            break;
        case 'steady': {
            const model = this._ext()._model;
            model.steady = args[0] === 'on';
            model._steadyReset();
            model._candidate = null;
            model._shown = null;
            model._changed();
            break;
        }
        case 'hotspot': {
            const ext = this._ext();
            if (args[0] === 'sans-profil') {
                this._net = null;
                ext._hotspot.setBackend(null);
            } else if (args[0] === 'profil') {
                const net = {state: 'off', refuse: false, onChange: null, timer: 0};
                this._net = net;
                ext._hotspot.setBackend(onChange => ({
                    state: () => net.state,
                    connect: async () => {
                        net.state = 'connecting';
                        onChange();
                        net.timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 2500, () => {
                            net.timer = 0;
                            net.state = net.refuse ? 'off' : 'on';
                            onChange();
                            return GLib.SOURCE_REMOVE;
                        });
                    },
                    disconnect: async () => {
                        net.state = 'off';
                        onChange();
                    },
                    destroy: () => {
                        if (net.timer)
                            GLib.source_remove(net.timer);
                        net.timer = 0;
                    },
                }));
            } else if (this._net) {
                this._net.refuse = args[0] === 'refuse';
            }
            break;
        }
        case 'hoverbanner': {
            const banner = Main.messageTray._banner;
            if (!banner?.mapped)
                throw new Error('no banner');
            const [x, y] = banner.get_transformed_position();
            const [w, h] = banner.get_transformed_size();
            this._pointer.notify_absolute_motion(GLib.get_monotonic_time(), x + w / 2, y + h / 2);
            break;
        }
        case 'dnd':
            new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'})
                .set_boolean('show-banners', args[0] !== 'on');
            break;
        case 'sharing':
            this._sharing = args[0] === 'auto' ? null : args[0] === 'on';
            this._ext()._quiet._report();
            break;
        case 'reglage':
            this._ext()._settings.set_boolean(args[0], args[1] === 'on');
            break;
        case 'nohelper':
            this._helperMode = 'none';
            this._ext()._refreshSystemSetup();
            break;
        case 'helper':
            // helper : copie de root (mot de passe possible) ; helper user :
            // copie sans droits seulement (commandes à lancer soi-même).
            this._helperMode = args[0] === 'user' ? 'user' : 'root';
            this._ext()._refreshSystemSetup();
            break;
        case 'clearall':
            // Sans relayer quarante effacements au faux moteur.
            this._ext()._bridge._keepOnly(new Set());
            this._ext()._bridge._notice?.destroy();
            break;
        case 'type':
            this._card(ind).entry.text = rest;
            break;
        case 'key': {
            const keyval = {Return: Clutter.KEY_Return, Escape: Clutter.KEY_Escape, Tab: Clutter.KEY_Tab}[args[0]];
            const t = GLib.get_monotonic_time();
            this._keyboard.notify_keyval(t, keyval, Clutter.KeyState.PRESSED);
            this._keyboard.notify_keyval(t + 1000, keyval, Clutter.KeyState.RELEASED);
            break;
        }
        case 'scroll': {
            // La molette, au milieu de l'écran : fait défiler la fenêtre qui s'y trouve.
            const t = GLib.get_monotonic_time();
            const [w, h] = global.display.get_size();
            this._pointer.notify_absolute_motion(t, w / 2, h / 2);
            for (let i = 0; i < Number(args[0] ?? 10); i++) {
                this._pointer.notify_discrete_scroll(t + 1000 * (i + 1), Clutter.ScrollDirection.DOWN,
                    Clutter.ScrollSource.WHEEL);
            }
            break;
        }
        case 'state':
            GLib.file_set_contents(`${this._dir}/${args[0]}.json`, `${JSON.stringify(this._state())}\n`);
            break;
        case 'langue':
            this._ext()._settings.set_string('phone-language', args[0] === 'auto' ? '' : args[0]);
            break;
        case 'forgetrequest':
            this._ext()._settings.set_boolean('forget-request', true);
            break;
        case 'accent':
            this._ext()._settings.set_boolean('use-accent-color', args[0] === 'on');
            break;
        case 'tray':
            Main.panel.statusArea.dateMenu.menu.open();
            break;
        case 'closetray':
            Main.panel.statusArea.dateMenu.menu.close();
            break;
        case 'dismiss': {
            const source = Main.messageTray.getSources().find(s => s.title === rest);
            source.notifications[0].destroy();
            break;
        }
        case 'shot': {
            const file = Gio.File.new_for_path(`${this._dir}/${args[0]}.png`);
            const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
            new Shell.Screenshot().screenshot(false, stream, (o, res) => {
                try {
                    o.screenshot_finish(res);
                } catch (e) {
                    logError(e, 'pilote');
                }
                stream.close(null);
            });
            break;
        }
        default:
            throw new Error(`unknown command ${cmd}`);
        }
    }
}
