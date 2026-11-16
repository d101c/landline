# Guide de relecture avant l'envoi

Pour l'auteur, qui relit chaque ligne avant de soumettre. État décrit : branche de travail du renommage, nom
« Landline », 16 novembre 2026. Les numéros de ligne sont ceux de cette branche ; s'ils ont bougé,
chercher le nom de la fonction.

Temps de lecture : estimé à environ 250 lignes à l'heure pour du code lu attentivement, deux fois plus
vite pour les textes d'interface et les tables. Total pour `extension/` : **6 600 lignes de JavaScript,
environ 24 heures** ; 820 lignes de CSS et 71 de schéma, environ 2 heures.

## 1. Ordre de lecture conseillé

1. `docs/ego/description.txt` et `docs/ego/note-de-revue.txt` : ce que l'on affirme au relecteur. Tout
   ce qui suit sert à vérifier que c'est vrai.
2. `extension/metadata.json`, `extension/lib/names.js` : l'identité.
3. `extension/extension.js` : l'assemblage, `enable()` / `disable()`, les sous-processus.
4. `extension/lib/setupHelper.js` puis `extension/prefs.js` : toute l'élévation de droits est là.
5. `extension/lib/engineClient.js` : le socket.
6. Les petits modules sans surprise : `quiet.js`, `replyLinks.js`, `notifications.js`, `hotspot.js`,
   `illustrations.js`, `phoneLabels.js`.
7. Le modèle : `model.js`, puis `model/constants.js`, `format.js`, `core.js`, `quiet.js`, `link.js`,
   `messages.js`, `notifications.js`, `calls.js`, `daily.js`, puis `tour.js` (le plus long), `obex.js`,
   `forget.js`, et `view.js` en dernier (il se lit comme un catalogue de ce qui s'affiche).
   `tests/model.test.js` est le meilleur compagnon : chaque règle y a son cas.
8. Les vues : `indicator.js`, `cards.js`, `setup.js`.
9. `stylesheet.css`, `stylesheet-light.css`, le schéma.
10. Hors archive : `system/landline-setup`, les scripts, puis le moteur (section 4).

## 2. Les fichiers de `extension/`

Ce qu'un relecteur d'extensions.gnome.org vérifie, et qui est rappelé fichier par fichier : rien de créé
avant `enable()` ; tout ce qui est créé est détruit dans `disable()` ; chaque source GLib retirée, et
retirée avant d'être recréée ; chaque signal déconnecté ; les sous-processus (lesquels, pourquoi,
attendus et annulés) ; l'élévation ; le presse-papiers ; les importations (pas de Gtk/Adw dans le Shell,
pas de St/Clutter/Meta dans les préférences) ; pas de journal bavard.

### `metadata.json` — 10 lignes, 10 min

Rôle : l'identité de l'extension et le texte de la fiche.

- `uuid` `landline@d101c.github.io` : **irréversible** une fois publié.
- `description` : doit être identique à `docs/ego/description.txt` (c'est ce texte qu'affiche la fiche).
- `shell-version` : 51 seulement ; pas de `version` (réservé au site), pas de `session-modes`.
- `url` : le dépôt doit exister et être public au moment de l'envoi.

### `lib/names.js` — 18 lignes, 5 min

Rôle : les noms partagés avec le moteur (unité systemd, socket, chemins de l'assistant).

- `ROOT_HELPER` (l. 17) est le **seul** chemin jamais donné à `pkexec`. Il doit être sous `/usr/local/libexec`.
- Ces valeurs doivent correspondre à `scripts/noms.sh`, `system/landline-setup` et
  `engine/src/core/include/tether/identity.hpp`.

### `extension.js` — 489 lignes, 2 h

Rôle : crée le modèle, le client, les vues ; fait le lien entre réglages, modèle et moteur.

- **Avant `enable()`** : une seule ligne de module, `Gio._promisify` (l. 24). Pas de constructeur.
- **`enable()` (l. 29) / `disable()` (l. 144)** : faire la liste de tout ce que `enable()` crée (modèle,
  dessins, pont de notifications, indicateur, partage de connexion, guetteur de silence, assistant,
  signaux des réglages l. 102, client) et cocher chaque élément dans `disable()`. Trois minuteries :
  `_timerId`, `_startTimerId`, `_setupPollId`.
- **Minuteries retirées avant d'être recréées** (le reproche fait deux fois à Accent Hold) :
  `_schedule()` l. 258 ; `_startEngine()` l. 298 ; `_pollSystemSetup()` l. 369 (ne recrée pas si elle
  existe, et ne redemande pas une relecture qui n'est pas revenue).
- **Sous-processus** : `systemctl --user start` (l. 302) et `restart` (l. 339), par `run()` de
  `lib/subprocess.js` : attendus, et **terminés** (`force_exit`) quand `this._cancellable` est annulé ;
  le code vérifie après l'attente que l'extension est toujours active (l. 307, l. 344).
- **Presse-papiers** : `_copyCommands()` l. 395, seule ligne qui y touche, en écriture. C'est le
  signalement EGO-A-005.
- Passage délicat : `_applyPermission()` l. 402. Deux `await` ; après chacun, on vérifie que
  l'assistant est toujours le même (extension désactivée entre-temps). `disable()` remet `_applying` à
  faux.
- Passage délicat : `_syncTourDialog()` l. 421. La boîte est créée, ouverte, mise à jour et fermée
  d'après `view().tour`. Toute boîte créée reste dans `_dialogs` jusqu'à son signal `destroy` (l. 442,
  par `connectObject`) : `disable()` (l. 172) détruit aussi celle qui finit son fondu de fermeture.
- `_onAlert()` l. 210 : `String.prototype.format` est fourni par le Shell.

### `lib/setupHelper.js` — 135 lignes, 40 min — **le fichier à lire le plus lentement**

Rôle : lit l'état des réglages Bluetooth auprès de l'assistant système et, s'il est installé chez root,
le lance avec `pkexec`. Aucun appel de fichier synchrone : tout passe par `query_info_async`.

- `ownedByRoot()` l. 35 et `rootHelper()` l. 50 : ne rendent le chemin que si le fichier appartient à
  l'uid 0, n'est inscriptible ni par le groupe ni par les autres, et est exécutable. Vérifier le masque
  `0o022`.
- `runAsRoot()` l. 61 : `pkexec <chemin> <verbe>`, vecteur d'arguments, attendu ; codes 126 et 127 de
  pkexec (refus), code 2 de l'assistant (partiel). Annulé pendant la fenêtre de mot de passe, pkexec est
  tué et rien n'est changé ; une fois l'assistant lancé en root, le signal ne l'atteint plus et il finit.
- `_statusPath()` l. 91 : la copie de root, sinon celle de `~/.local/lib/landline/` — celle-ci n'est
  **jamais** élevée, seulement interrogée (`status`, l. 104).
- `status.command` (l. 117) : la commande `sudo … apply` montrée à l'utilisateur ; le chemin passe par
  `GLib.shell_quote`.

### `lib/subprocess.js` — 36 lignes, 10 min

`run(argv, cancellable, capture)` : le seul endroit où un programme est lancé. Vecteur d'arguments,
attente asynchrone, et `force_exit()` (SIGKILL) à l'annulation ; rien n'est lancé si l'annulation a déjà
eu lieu.

### `prefs.js` — 210 lignes, 50 min

Rôle : la fenêtre « Phone Settings » (Adw), liée au schéma.

- Importations : `Adw`, `Gtk`, `Gio` et `lib/setupHelper.js` (qui n'importe que Gio, GLib, `subprocess.js` et `names.js`). Rien du Shell.
- Signal sur les réglages l. 101, déconnecté à la fermeture l. 106-107.
- « Undo » l. 160-193 : le groupe reste caché tant que `rootHelper()` (asynchrone, l. 173) n'a pas rendu
  un chemin ; `pkexec … revert` attendu, annulé à la fermeture de la fenêtre.
- « Forget This iPhone… » l. 193-201 : ne fait que lever le réglage `forget-request` ; c'est le Shell qui
  pose la question.
- Lire les textes : ce sont eux qui disent à l'utilisateur ce qu'un mot de passe va changer.

### `lib/engineClient.js` — 304 lignes, 1 h 15

Rôle : le socket Unix du moteur, une ligne JSON par message, une commande à la fois.

- Aucune importation du Shell ; chemin du socket fixe (`defaultSocketPath()`), aucune variable d'environnement.
- Deux minuteries : nouvel essai (l. 167, retirée par `_clearRetry()` juste avant) et délai d'accusé
  (l. 295, `_clearAck()` juste avant). `stop()` l. 91 annule les opérations, ferme le socket et retire les deux.
- Lecture asynchrone ligne à ligne (l. 209) : vérifier que chaque rappel regarde `cancellable.is_cancelled()`.
- Journal : deux `console.warn` (ligne illisible, commande sans réponse) et un `console.error`.
- Passage délicat : la file des commandes et les trois façons de finir une attente (`OK`, réponse
  directe, erreur nommée), tables `DIRECT_REPLIES` et `OPTIONAL_REPLIES` en tête.

### `lib/quiet.js` — 65 lignes, 15 min

Rôle : dit au modèle si le Ne pas déranger est actif ou si l'écran est partagé.

- `Gio.Settings` sur le schéma de GNOME (l. 17), lecture seule, `connectObject` / `disconnectObject`.
- Signal `stopped` posé à la main sur chaque poignée de partage (l. 40), déconnecté dans `_drop()` et
  pour toutes dans `destroy()`.
- `global.backend.get_remote_access_controller?.()` : API interne du Shell, appelée avec `?.`.

### `lib/replyLinks.js` — 78 lignes, 20 min

Rôle : fabrique le lien « Reply in app » (JavaScript pur).

- Les trois formes de lien l. 11-14 : c'est ce que la description déclare.
- Les règles qui écartent un groupe ou un expéditeur incertain (`replySender`, `contactNumber`).

### `lib/notifications.js` — 151 lignes, 40 min

Rôle : transforme les notifications des autres applis de l'iPhone en notifications GNOME.

- `Gio.AppInfo.launch_default_for_uri` l. 50 : seule ouverture de lien, sur clic.
- Signaux `destroy` et `activated` posés sur nos propres sources et notifications (l. 66, 95, 98, 119) :
  ils disparaissent avec elles ; `destroy()` l. 143 détruit les sources sans rien relayer à l'iPhone.
- Vérifier qu'aucun texte de notification ne part dans le journal.

### `lib/hotspot.js` — 238 lignes, 1 h

Rôle : allume ou coupe le profil Bluetooth de l'iPhone dans NetworkManager.

- `gi://NM` importé à la demande (l. 35), avec deux `Gio._promisify` posés à ce moment-là (l. 38-39).
- Signaux sur le client NM (l. 45) et sur la connexion active (l. 57), tous par `connectObject`,
  déconnectés l. 62 et dans `destroy()` l. 124-131.
- Minuterie de 30 s l. 215, retirée par `_settle()` (l. 192-200) avant d'être recréée, et dans `destroy()`.
- Rien n'est créé ni supprimé dans NetworkManager : seulement `activate` / `deactivate` (l. 112, l. 120).
- **Jamais exécuté pour de vrai** (seulement avec la doublure du Shell d'essai) : à lire deux fois.

### `lib/illustrations.js` — 126 lignes, 30 min

Rôle : lit les quatre dessins SVG, y écrit nom d'hôte, code et libellés, et rend une icône.

- Lecture **asynchrone** (`load_contents_async`, l. 56) : la lecture synchrone était un motif de rejet.
- `Gio._promisify` au niveau du module (l. 15) ; annulation dans `destroy()` (l. 67).
- Ce qui est écrit dans le SVG vient du nom d'hôte et du moteur : vérifier l'échappement XML.

### `lib/phoneLabels.js` — 101 lignes, 15 min

Rôle : les mots de l'iPhone en anglais et en français, et le choix de la langue.

- Les libellés français sont des données (relevés sur un iPhone), pas des commentaires.
- Les libellés anglais n'ont pas été comparés à un iPhone.

### `lib/model.js` — 27 lignes, 10 min

Rôle : assemble la classe `Model` à partir des modules de `lib/model/`.

- La ligne `parts.reduce(...)` (l. 26) : comprendre qu'elle empile les modules l'un sur l'autre.
- Ce que le fichier réexporte pour les tests.

### `lib/model/constants.js` (67 l., 15 min) et `lib/model/format.js` (91 l., 20 min)

Rôles : les délais et limites ; l'écriture des noms, numéros et durées.

- Chaque délai a une raison écrite au-dessus : vérifier qu'on sait la redire.
- `formatNumber()` ne connaît que la Suisse, la France et l'Amérique du Nord.

### `lib/model/core.js` — 460 lignes, 1 h 50

Rôle : l'état, l'aiguillage des événements du moteur, le temps.

- Aucune importation hors `constants.js` : pas de GLib, pas de minuterie. `nextDeadline()` (l. 348) dit
  quand rappeler `expire()` (l. 393).
- Le constructeur liste tout l'état ; `_reset()` dit ce qui est perdu quand le moteur disparaît et ce
  qui survit (`_seen`, `_shownNotes`, brouillons).
- Passage délicat : `setEngineState()` l. 135-160 (le parcours gardé pendant une relance demandée).
- Passage délicat : `handleEvent()` l. 162-300, surtout `bt_pair_confirm_request`, `bt_pair_result` et
  `_abandoned` (un appairage dont plus personne n'attend le code est refusé).

### `lib/model/quiet.js` (48 l., 10 min) et `lib/model/link.js` (143 l., 35 min)

Rôles : quand rien ne doit surgir ; l'état du lien et l'indicateur qui ne clignote pas.

- `_hushCalls()` : les appels ne passent que le Ne pas déranger, jamais la sourdine ni le partage.
- `_settle()` et `_watchLinks()` : les trois règles de l'en-tête de `link.js`.
- `_link()` : l'ordre des cas décide de ce que dit l'indicateur.

### `lib/model/messages.js` — 268 lignes, 1 h

Rôle : messages reçus, carte de réponse, « New Message… ».

- `_onMessage()` l. 62 : ce qui ne fait **pas** surgir de pastille (historique rejoué, message ancien,
  silence, déjà vu).
- `reply()` l. 239 : `bt_send_message`, délai, texte gardé en cas d'échec.
- Aucun contenu de message ne doit partir ailleurs que vers la vue.

### `lib/model/notifications.js` — 139 lignes, 35 min

Rôle : quelles notifications de l'iPhone deviennent des notifications GNOME.

- La règle « jamais deux fois » et l'empreinte par contenu (`_shownNotes`).
- `_replyOffer()` : l'action « Reply in app » n'est proposée que pour un expéditeur certain.

### `lib/model/calls.js` — 205 lignes, 50 min

Rôle : appels, réponse, raccrocher, côté du son.

- `setCallAudio()` et `_audioMoved()` : la carte ne montre jamais comme fait ce qui a seulement été demandé.
- `_syncCalls()` : `bt_set_calls` envoyé une seule fois par écart.

### `lib/model/daily.js` — 105 lignes, 25 min

Rôle : réglages tenus par le moteur, partage de connexion, batterie.

- `wantSetting()` et la fenêtre de 8 s (`SETTING_ECHO_MS`) contre l'écho d'une ancienne valeur.

### `lib/model/tour.js` — 655 lignes, 2 h 40

Rôle : le parcours d'accueil, de l'autorisation à « Done », et la confirmation du code.

- `startTour()` l. 43 : par quelle étape on entre, selon l'état.
- `_permissionCommands()` l. 35 : les commandes montrées (et copiées) quand rien ne peut être élevé.
- Passage délicat : `_tourResult()` l. 354 et `_tourVerify()` l. 394 — « relié » se constate, il ne se
  croit pas sur parole ; la réparation n'est proposée qu'une fois.
- `confirmPairing()` l. 634 et `closeTour()` l. 586 : un code en attente est refusé quand on ferme.
- La file d'affichage (`_tourGo`, `_tourPump`) : seul l'affichage attend, jamais le protocole.

### `lib/model/obex.js` (129 l., 30 min) et `lib/model/forget.js` (113 l., 30 min)

Rôles : la panne « messages et contacts non proposés » et « Fix It » ; l'oubli de l'iPhone.

- `_obexState()` : les cinq états et ce qui fait passer de l'un à l'autre.
- `forgetConfirm()` : `bt_unpair` ne part qu'après la confirmation.

### `lib/model/view.js` — 495 lignes, 1 h 30

Rôle : `view()` (l. 8), `_tourView()` (l. 236) et `_restCard()` (l. 291) : tout ce qui s'affiche, calculé
depuis l'état, sans rien modifier.

- Vérifier qu'aucune de ces fonctions n'écrit dans `this` (hors lecture).
- Tous les textes d'interface du modèle sont ici : les relire comme un utilisateur.
- En silence, `pill` est retiré mais `card` reste (l. 150-155).

### `lib/indicator.js` — 352 lignes, 1 h 20

Rôle : le bouton de la barre, qui devient pastille, et ses deux menus.

- Signal `open-state-changed` l. 103 sur notre propre menu (détruit avec le bouton).
- Second menu pour l'appel entrant, jamais modal, déclaré par `addTopChrome` l. 130 et détruit dans
  `_onDestroy()` l. 339-351 (la destruction de l'acteur le retire du gestionnaire de mise en page).
- `_later()` l. 291-298 : source `idle_add` retirée avant d'être recréée, et dans `_onDestroy()`.
- Animation de largeur (`ease`, l. 242) arrêtée à la destruction (l. 344).

### `lib/cards.js` — 865 lignes, 3 h

Rôle : ce qui descend sous l'indicateur : menu de repos, cartes de réponse, d'appel, de code.

- `clamped()` l. 216-267 : un « later » du compositeur (l. 254), retiré au signal `destroy` de l'étiquette (l. 259).
- Les signaux sont posés sur des widgets créés ici et détruits avec le menu à chaque reconstruction :
  vérifier qu'aucun n'est posé sur un objet qui survit (modèle, réglages, `global`).
- Les champs de saisie (`St.Entry` l. 441 et l. 507) : la frappe n'est envoyée qu'au modèle.
- `TopRow` (l. 38) : allocation faite à la main ; relire `vfunc_allocate`.

### `lib/setup.js` — 986 lignes, 3 h 30

Rôle : la boîte du parcours d'accueil ; n'affiche que `view().tour`.

- Minuterie du clavier `_wantFocus()` l. 183-195 : ne se recrée pas si elle existe, retirée au `destroy` (l. 90-95).
- `vfunc_key_press_event` l. 99 : Échap met le parcours de côté.
- Les boutons qui déclenchent quelque chose de sensible : « Continue » de l'autorisation (l. 535),
  « Copy Commands » (l. 529).
- Sur l'étape du code, aucun bouton n'a le clavier par défaut : Entrée ne confirme rien.
- Les textes : c'est là que l'utilisateur lit ce qui va être changé sur son système.

### `stylesheet.css` (692 l.), `stylesheet-light.css` (124 l.), schéma (71 l.) — 2 h

- Toutes les classes commencent par `landline-` : rien ne doit toucher au reste du Shell.
- Schéma : treize clés, descriptions en anglais, `forget-request` est un signal entre processus.

## 3. Hors de l'archive : assistant système et scripts

### `system/landline-setup` — 883 lignes de bash, 3 h

Rôle : `status`, `apply`, `revert` des deux réglages Bluetooth. **C'est le seul programme lancé en root.**

- Commentaires et messages encore **en français** (voir `docs/ego/reste-a-faire.md`).
- En tête : `NAME`, `OLD_NAME`, `MARKER` ; tous les chemins en dérivent (l. 39-89).
- Aucun argument libre : `status`, `apply`, `revert`, `version`, `--dry-run` (`main`, en fin de fichier).
- `PATH` fixé en dur quand la racine n'est pas factice (l. 53-56) ; `LANDLINE_ROOT` ne sert qu'aux essais
  — **à vérifier : `pkexec` et `sudo` effacent bien cette variable de l'environnement** (pkexec ne garde
  que quelques variables ; `sudo` dépend de `env_reset`).
- `remove_path … marque` : un fichier qui ne porte pas l'en-tête n'est jamais retiré.
- `install_self()` : la copie vers `/usr/local/libexec/landline/` et la fiche polkit.
- La reprise de l'ancien nom : `remove_old_install()`, `SKIP_OLD_DROPIN`.
- `system/io.github.d101c.landline.policy` doit être identique à ce qu'écrit `content_policy()` (un
  essai le vérifie).

### `system/landline-engine.service` — 43 lignes, 10 min

- `ExecStartPre=-/usr/bin/pkill -u %U -f ^landlined` : ne peut viser que notre démon lancé à la main.

### Scripts — 870 lignes, 2 h

| Fichier | Lignes | À regarder |
|---|---|---|
| `scripts/noms.sh` | 34 | les trois noms et les noms d'avant |
| `scripts/install-local.sh` | 336 | la fonction `migrate()` : l'ancien moteur arrêté avant le déplacement, jamais d'écrasement, `dconf load` seulement dans la vraie session ; `--dry-run` fidèle |
| `scripts/uninstall-local.sh` | 154 | `--purge` retire les données des deux noms |
| `scripts/pack-extension.sh` | 20 | ce qui entre dans l'archive |
| `scripts/build-engine.sh` | 83 | options CMake ; ne lance jamais le test « headless » |
| `scripts/renommer.sh` | 221 | section 4 de `docs/ego/reste-a-faire.md` |
| `scripts/reset-essai.sh` | 20 | efface les données du moteur : à ne pas lancer par mégarde |

## 4. Le moteur (`engine/`, C++)

Le moteur est le code de **Tether** (Zack Bartel, MIT), commit `9ca3c46`, repris au premier commit de ce
dépôt (`2520b44`). `engine/LICENSE` et `NOTICE.md` sont intacts.

```
$ git diff --stat 2520b44..HEAD -- engine | tail -1
 121 files changed, 12336 insertions(+), 2729 deletions(-)
```

Sur ces 121 fichiers : 17 sont les catalogues de traduction (`po/`, 2 022 lignes changées, toutes par le
renommage) ; 27 sont **nouveaux** (écrits par nous) ; 77 sont des fichiers de Tether **modifiés**. Tout
le reste de `engine/` (environ 350 fichiers : `apple/`, `extension/`, `docs/`, `src/gtk`, `src/tui`,
`src/dialog`, `packaging/`, la plupart de `src/core`) est de Tether, **tel quel**.

Deux couches de modifications, à lire séparément :

- **Avant le renommage** (`git diff 2520b44..7e77ccc -- engine` : 67 fichiers, +9 903 −404) : le travail
  de fond, décrit dans `docs/moteur-reconnexion.md`, `moteur-quotidien.md`, `moteur-api.md`,
  `contrat-appairage.md`.
- **Le renommage** (`git diff 7e77ccc..HEAD -- engine ':!engine/po'` : 56 fichiers, +443 −335) : presque
  uniquement des chaînes. Les seuls changements de logique : `secret_store.cpp` (reprise de l'ancienne
  clé du trousseau), `client.cpp` (`systemd_owns_tetherd()` reconnaît notre unité), `identity.hpp`
  (nouveau), `src/CMakeLists.txt` (interfaces d'origine hors compilation).

### Fichiers nouveaux (27)

- En-têtes : `bluetooth/ancs/subscription_guard.hpp`, `bond_health.hpp`, `call_audio_handover.hpp`,
  `le_first.hpp`, `pending_connect.hpp`, `phone_state.hpp`, `presence.hpp`, `reconnect_policy.hpp`,
  `supervision_tick.hpp`, `bus_watch.hpp`, `identity.hpp`, `log_file.hpp`.
- Sources : `bluetooth/call_audio_handover.cpp`, `bus_watch.cpp`, `log_file.cpp`.
- Tests : `test_advert`, `test_ancs_subscription_guard`, `test_bus_watch`, `test_call_audio_handover`,
  `test_le_first`, `test_log_file`, `test_pending_connect`, `test_phone_audio`, `test_phone_state`,
  `test_presence`, `test_reconnect_policy`, `test_supervision_tick`.

### Fichiers de Tether modifiés, par poids (lignes changées)

Où porter l'attention d'abord :

| Fichier | Lignes | Sujet |
|---|---|---|
| `src/core/src/bluetooth/connection.cpp` | 850 | supervision des liens, reconnexion, présence |
| `src/core/src/net.cpp` | 353 | commandes du socket, partie réseau éteinte par défaut |
| `src/core/src/bluetooth/bearer_supervisor.cpp` (+ `.hpp` 133) | 350 | maintien de BR/EDR et LE |
| `src/core/src/bluetooth/pairing.cpp` (+ `.hpp` 82) | 336 | appairage, confirmation du code par le client |
| `src/core/src/bluetooth/ancs/client.cpp` (+ `.hpp` 32) | 311 | notifications |
| `src/core/src/bluetooth/telephony.cpp` (+ `.hpp` 100) | 201 | appels |
| `src/core/src/bluetooth/profile_supervisor.cpp` (+ `.hpp` 87) | 157 | sessions MAP et PBAP |
| `src/core/src/bluetooth/monitor.cpp` | 150 | observation de BlueZ |
| `src/daemon/main.cpp` | 141 | démarrage, journal, bus |
| `src/core/src/bluetooth/phone_audio.cpp` (+ `.hpp` 70) | 124 | son de l'iPhone |
| `src/cli/main.cpp` | 95 | textes de l'outil (renommage) |
| `src/core/src/bluetooth/advert.cpp` (+ `.hpp` 89) | 73 | annonce LE |
| `src/core/src/secret_store.cpp` | 66 | **reprise de la clé du trousseau : non essayée en vrai** |
| `src/core/src/bluetooth/objects.cpp` | 61 | prérequis système |

Moins de 35 lignes chacun : `config.cpp`, `config.hpp`, `verbs.cpp`, `pbap_session.cpp`,
`connection.hpp`, `obex_transfer.cpp` / `.hpp`, `net.hpp`, `client.cpp`, `objects.hpp`, `monitor.hpp`,
`service.cpp`, `session_lock.cpp` / `.hpp`, `lan_info.cpp`, `camera.cpp`, `paths.cpp`, `agent.cpp`,
`map_session.cpp`, `telephony_pipewire.cpp`, `notification.cpp`, `bmessage.cpp`, `crypto.cpp`,
`extension_host.cpp`, `i18n.cpp`, `pbap_session.hpp`, les cinq `CMakeLists.txt`,
`packaging/systemd/tetherd.service.in`, `scripts/tether-reset.sh`.

Tests de Tether modifiés : `test_bluetooth_supervisor` (989), `test_bluetooth_pairing` (200),
`test_telephony` (153), `test_net` (148), `test_bluetooth_objects` (144), et une vingtaine d'autres où
seuls des noms ont changé.

Ce qui garde le nom de Tether, volontairement : l'espace de noms `tether::`, le dossier `include/tether/`,
les cibles CMake et `tether_test`, les macros `TETHER_*`, le protocole de l'application iOS
(`_tether._tcp`, certificat, hôte natif), les icônes `tether-app-*`, toute la documentation d'origine.

## 5. Liste de contrôle avant l'envoi

Code
- [ ] J'ai lu chaque fichier de `extension/` et je peux expliquer chaque fonction.
- [ ] `enable()` / `disable()` : chaque objet créé est détruit, chaque source retirée, chaque signal déconnecté.
- [ ] Aucune minuterie n'est recréée sans avoir été retirée (`extension.js` l. 249, 309, 360 ;
      `engineClient.js` l. 167, 295 ; `hotspot.js` l. 215 ; `indicator.js` l. 291 ; `setup.js` l. 183).
- [ ] `pkexec` n'apparaît que dans `lib/setupHelper.js`, sur `ROOT_HELPER`, après `rootHelper()`.
- [ ] `St.Clipboard` n'apparaît qu'une fois (`extension.js`, `_copyCommands`), en écriture.
- [ ] Aucune lecture de fichier synchrone, aucun `GLib.spawn*`, aucune variable d'environnement lue.
- [ ] Pas de `Gtk`/`Adw` hors de `prefs.js` ; pas de `St`/`Clutter`/`Meta` dans `prefs.js`.
- [ ] Commentaires en anglais, aucun renvoi à `docs/`, aucun code d'essai.

Archive
- [ ] `scripts/pack-extension.sh`, puis `unzip -l` : 45 entrées, pas de script, pas de `helper/`, `LICENSE` présent.
- [ ] `shexli --format text <zip>` : seulement EGO-A-005 et EGO-M-004.
- [ ] `metadata.json` : `description` identique à `docs/ego/description.txt` ; `version-name`.
- [ ] L'archive installée dans une vraie session s'active et se désactive sans erreur dans le journal
      (`journalctl --user -b | grep -i landline`), dix fois de suite.

Essais
- [ ] `tests/run.sh` (424 + 72 + 22), `system/tests/run.sh` (136), `system/tests/install.sh` (70),
      `scripts/build-engine.sh` (909), `tools/shell-test/run.sh`.
- [ ] Essai réel après migration : l'iPhone se relie sans nouvel appairage ; les messages gardés sont
      lisibles (clé du trousseau reprise) ; `landline-setup status` dit `ready`.
- [ ] Parcours complet sur une machine vierge, avec la vraie fenêtre polkit.

Fiche
- [ ] Le dépôt `github.com/d101c/landline` est public, le README à jour, le lien « Install the engine » ouvre la bonne section.
- [ ] Un inconnu peut installer le moteur en suivant le README (essayé sur une machine qui n'est pas la mienne).
- [ ] Captures de `docs/ego/captures/` envoyées ; description collée ; note de revue postée en commentaire.
- [ ] Les textes de la description et de la note correspondent au code relu (sous-processus, mot de passe, presse-papiers).
