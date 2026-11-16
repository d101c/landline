# Extension GNOME Shell

L'extension montre l'iPhone dans la barre du haut : un glyphe et un point au repos, une pastille quand un
message ou un appel arrive, une carte sous la pastille pour répondre. Elle ne parle qu'au moteur
(`engine/`), par son socket Unix, selon `docs/moteur-api.md`. Elle ne touche jamais au Bluetooth.

- Dossier : `extension/` — uuid `landline@d101c.github.io`, GNOME Shell 51 (seule version essayée ; les menus emploient les paramètres `open()`/`close()` de 51).
- Textes d'interface en anglais, passés par gettext (domaine `landline`), sans traduction pour l'instant.

## 1. Architecture

```
extension/
  metadata.json
  extension.js          assemble le tout ; enable() / disable()
  stylesheet.css        couleurs, rayons, tailles (style sombre et défaut)
  stylesheet-light.css  les quelques couleurs à retourner en style clair
  prefs.js              « Phone Settings » : réponses toutes faites, son, appels, moments
                        de silence, éloignement, batterie, couleur, langue de l'iPhone,
                        retour arrière des réglages Bluetooth (si l'assistant est installé
                        chez root), et en dernier, seul, l'oubli de l'iPhone
  LICENSE               GPL-2.0, comme les autres extensions de l'auteur
  schemas/              treize réglages (quick-replies, use-accent-color, call-control,
                        phone-language, forget-request ; muted, follow-do-not-disturb,
                        quiet-while-sharing, calls-through-do-not-disturb ;
                        phone-audio-here, lock-on-away, lock-away-seconds, battery-alerts)
  icons/                glyphe téléphone, raccrocher, envoyer (symboliques)
  illustrations/        quatre écrans d'iPhone dessinés (SVG à compléter) pour le parcours
  lib/
    engineClient.js     client du moteur (Gio, sans Shell)
    names.js            les noms partagés avec le moteur et son installation (unité, socket,
                        assistant), en un seul endroit
    model.js            modèle d'état (JavaScript pur, sans Shell ni GLib) : assemble les modules
                        de model/
    model/              core (événements, temps), quiet (silence), link (état du lien, indicateur
                        stable), messages, notifications, calls, daily (réglages tenus par le
                        moteur, connexion, batterie), tour (parcours), obex (panne des messages
                        et contacts), forget (oubli), view (ce qui est montré), constants, format
    indicator.js        l'indicateur / la pastille dans la barre
    cards.js            ce qui descend sous la pastille (menu et cartes)
    setup.js            le parcours d'accueil : une seule boîte, de l'autorisation à « Done »
    illustrations.js    remplit les dessins (nom d'hôte, code, interrupteurs, libellés) et les rend
    phoneLabels.js      les mots de l'iPhone, par langue (JavaScript pur), et le choix de la langue
    setupHelper.js      appels à l'assistant système `landline-setup`
    subprocess.js       lance un programme : attendu sans bloquer, terminé à l'annulation
    notifications.js    notifications des autres applis vers la liste de GNOME
    quiet.js            guette le Ne pas déranger de GNOME et le partage d'écran
    replyLinks.js       « Reply in app » : liens vers le client officiel (JavaScript pur)
    hotspot.js          la connexion Internet de l'iPhone, par NetworkManager
tools/
  faux-moteur.py            faux moteur : même protocole, données inventées
  faux-landline-setup    faux assistant système (même contrat, ne change rien)
  shell-test/               l'extension dans un Shell sans écran, isolé
  shell-test/bin/           faux pkexec et faux systemctl pour ce Shell
tests/                  tests du modèle et du client (gjs -m)
```

Sens de circulation :

```
moteur ──lignes JSON──▶ engineClient ──événements──▶ model ──view()──▶ indicator + cards
                              ▲                        │  └─onNotification─▶ notifications
                              └──────commandes─────────┘
```

### Client du moteur (`lib/engineClient.js`)

- `Gio.SocketClient` sur le socket Unix, lecture ligne à ligne asynchrone (`read_line_async`), sans jamais
  cesser de lire.
- Chemin : `$XDG_RUNTIME_DIR/landline/landlined.sock`, sans variable d'environnement pour le changer.
- Le protocole n'a pas d'identifiant de requête et accuse réception par une ligne `OK` qui n'est pas du
  JSON. Les commandes partent donc **une à la fois** : la suivante part quand la précédente a reçu son `OK`,
  ou sa réponse directe pour les commandes qui en ont une (table `DIRECT_REPLIES`), ou une erreur qui la
  nomme, ou au bout de 12 s (les commandes d'appel peuvent retenir le moteur 10 s). Chaque ligne JSON,
  réponse ou diffusion, est remise au modèle et distinguée par son champ `command`.
- À chaque connexion, la séquence de démarrage (`STARTUP_COMMANDS` dans `model.js`) : `subscribe`,
  `bt_status`, `bt_list_devices`, `bt_list_notifications`, `bt_list_calls`, `bt_list_threads`,
  `bt_list_call_history`, `bt_audio`, `bt_presence`. Les deux dernières sont récentes : un moteur plus
  ancien y répond par un simple `OK`, que le client accepte comme fin d'attente (`OPTIONAL_REPLIES`).
- Socket absent ou fermé : état `absent`, puis nouvel essai après 1, 2, 4, 8 puis 15 s, sans fin. Le bouton
  « Try Again » essaie tout de suite.
- Avant de dire que le moteur ne tourne pas, `extension.js` demande une fois
  `systemctl --user start landline-engine.service` : si la commande réussit, le socket est réessayé
  aussitôt puis 3 s plus tard ; « Engine not running » ne s'affiche que si cela n'a rien donné. L'essai est
  refait à chaque nouvelle disparition du moteur, jamais en boucle.
- Une commande envoyée alors que le moteur est absent est abandonnée : l'état est relu en entier à la
  connexion suivante.
- `stop()` annule les opérations en cours, ferme le socket et retire les deux minuteries.

### Modèle d'état (`lib/model.js`, `lib/model/`)

La classe `Model` est assemblée à partir d'un module par sujet (`lib/model/*.js`) : chaque module est une
fonction qui ajoute ses méthodes à la classe qu'on lui donne (`withCalls = Base => class extends Base`),
`ModelCore` porte l'état et l'aiguillage des événements. Tous travaillent sur le même objet ; les noms
de méthodes cités plus bas se trouvent dans le module de leur sujet.

Aucune dépendance : l'heure vient d'une fonction `now`, les commandes sortent par une fonction `send`. Il
reçoit les événements (`handleEvent`), les gestes de l'utilisateur (`cardOpened`, `openThread`, `reply`,
`answer`, `decline`, `hangUp`, `setCallAudio`, `startTour`, `confirmPairing`…) et rend par `view()` ce
qu'il faut montrer : `pill` (ton, icône, point, compteur, textes), `card` (menu, réponse, appel…) et
`tour` (le parcours d'accueil). Il ne pose aucune
minuterie : `nextDeadline()` dit quand rappeler `expire()`, et `extension.js` tient une seule minuterie.

Règles principales :

| Situation | Ce que dit `view()` |
|---|---|
| relié (`classic_connected` ou `le_connected`) | glyphe + point plein ; compteur à la place du point s'il y a des non-lus |
| appairé mais lien coupé | glyphe atténué + point creux ; le menu dit pourquoi d'après `bt_presence` (« Out of range », « Bluetooth is off on the iPhone »…) |
| les liens montent et retombent plus de 2 fois en 30 s | « Link again », tenu jusqu'au clic ou à 60 s de lien stable (voir « Indicateur stable ») |
| Ne pas déranger, écran partagé, sourdine | rien ne grossit : forme de repos, compteur nu (voir « Silence ») |
| réglages système exigés manquants et pas de lien | « Permission needed » ; le clic ouvre le parcours à l'autorisation |
| Bluetooth de l'ordinateur éteint | glyphe atténué + « Bluetooth is off » |
| iPhone choisi mais plus appairé | « Link again » ; le clic ouvre le parcours à la recherche |
| appairage présent mais inutilisable (`needs_repair`) | « Link again », jamais « relié » ; le clic ouvre le parcours à la réparation |
| aucun iPhone choisi | « Link iPhone » ; le clic ouvre le parcours |
| parcours mis de côté (Échap) | « Continue setup » (« Confirm link » si un code attend) ; le clic le rouvre où il en était |
| relié, contrôle d'appel coupé dans le moteur | `bt_set_calls enabled:true` envoyé une fois (sauf réglage contraire) |
| socket du moteur muet, démarrage tenté | « Engine not running », carte avec « Try Again » |
| `bt_message` non lu, entrant, de moins de 10 min | pastille 7 s ; ignorée, elle devient un non-lu |
| carte ouverte sur la pastille | carte de réponse ; `bt_mark_read` part pour ce fil |
| réponse envoyée | « Sending… », puis « Sent » 2 s au `bt_send_result`, ou « Not sent » (le texte est gardé) |
| `bt_calls` avec un appel `incoming` | « <Prénom> is calling » + carte Decline / Answer |
| `bt_calls` avec un appel `incoming`, l'ordinateur pouvant porter le son | carte Answer here / Answer on iPhone / Decline |
| appel `active` | pastille verte « <Prénom> · mm:ss » ; carte iPhone / This computer / Hang Up |
| `bt_battery_low` (20 %, 10 %) | une notification GNOME discrète, « iPhone battery at 20 % », une fois par franchissement |
| batterie passée sous 15 %, moteur sans `battery_state` | pastille « iPhone 12 % » 7 s, une fois |
| `bt_pair_confirm_request` hors assistant | « Confirm link » + carte avec le code |
| relié, messages et contacts non proposés (voir « Messages et contacts non proposés ») | avis dans le menu + « Fix It » |
| un iPhone choisi ou appairé | « Forget This iPhone… » dans Phone Settings (plus dans le menu : un clic par accident y était possible) |
| relié, messages lisibles | « New Message… » dans le menu |

Détails qui comptent :

- L'historique rejoué à l'ouverture d'une session (messages lus, envoyés ou anciens) ne fait rien surgir ;
  un même message (`handle`) n'est compté qu'une fois.
- Le nom de l'appelant manque dans `bt_calls` diffusé : le modèle demande `bt_list_calls` une fois par
  appel, et à défaut cherche le numéro dans les conversations.
- `bt_call_result` arrive deux fois (réponse et diffusion) : sans effet.
- Notifications : celles de Messages et du téléphone restent à la pastille, **sauf** si la pastille ne peut
  pas les montrer (messages illisibles, contrôle d'appel coupé) et sauf l'appel manqué, qui n'a pas d'autre
  trace. Tout le reste part vers la liste de GNOME — **seulement ce qui est nouveau** (voir « Les vieilles
  notifications ne reviennent pas »).

### Indicateur stable

Défaut vu en réel le 16 novembre 2026, appairage cassé d'un côté : les liens montaient et retombaient
toutes les quelques secondes, et l'indicateur suivait — relié, hors de portée, allumé, éteint, étiré.
Trois règles (`_settle()`, `_watchLinks()` dans `model.js`) :

- **Délai.** Un nouvel état de repos n'est montré que s'il a tenu 3 s (`LINK_SETTLE_MS`). Exceptions :
  « relié » confirmé (un lien monté **et** un service actif : `map_open`, `pbap_open` ou `ancs_ready`)
  se montre tout de suite ; un message et un appel entrants ne sont pas des états et n'attendent jamais ;
  le premier état après « rien », s'il n'est ni « relié » ni « hors de portée », est dit tout de suite.
- **Largeur constante au repos.** Le point et le compteur partagent un emplacement de largeur fixe
  (16 px) : l'indicateur a la même largeur relié, hors de portée, au démarrage et avec des non-lus
  (« 9+ » au-delà de 9).
- **Verrou « Link again ».** Si les liens tombent plus de 2 fois en 30 s (`FLAP_LIMIT`,
  `FLAP_WINDOW_MS`), ou si le moteur dit `needs_repair`, l'indicateur se fixe sur « Link again », quels
  que soient les liens ensuite. Il en sort par une action de l'utilisateur (le clic, qui ouvre alors le
  parcours **à la réparation** même si les liens sont montés à cet instant ; l'oubli) ou après 60 s de
  lien monté sans chute ni `needs_repair` (`STEADY_MS`). La disparition du moteur n'est pas une chute.

`view().link` est l'état montré ; `view().unsteady` vaut `flapping` ou `repair` pendant le verrou. Les
tests des autres règles créent le modèle avec `steady: false` (tout état se montre aussitôt) ; le
pilote du Shell d'essai fait de même hors de la partie « stable ».

### Silence : Ne pas déranger, écran partagé, sourdine

Quand rien ne doit surgir, l'extension ne montre **aucun contenu** : pas de pastille qui grossit, ni
prénom ni texte dans la barre, pas de carte d'appel qui descend, pas de bulle, pas d'alerte batterie.
L'indicateur garde sa forme de repos ; les non-lus s'additionnent derrière un compteur nu ; le menu,
ouvert exprès, montre tout (y compris la carte d'un appel qui sonne, avec ses boutons) et dit pourquoi
(« Do Not Disturb is on: nothing pops up here. »). À la sortie, rien n'est rejoué.

| Cause | Source | Réglage (Phone Settings, « Quiet times ») |
|---|---|---|
| Ne pas déranger de GNOME | `org.gnome.desktop.notifications show-banners` à faux (`lib/quiet.js`) | `follow-do-not-disturb`, vrai par défaut |
| écran partagé ou enregistré | `global.backend.get_remote_access_controller()`, signal `new-handle`, poignées `is_recording` : la source de l'indicateur de partage du Shell lui-même | `quiet-while-sharing`, vrai par défaut |
| sourdine | « Mute iPhone on this computer », interrupteur du menu | `muted`, faux par défaut ; silence complet, appels compris |
| écran verrouillé | le Shell désactive l'extension (elle ne déclare pas `unlock-dialog`) : rien n'existe sur l'écran de verrouillage | — |

Appels : en Ne pas déranger, rien non plus, sauf `calls-through-do-not-disturb` (faux par défaut), qui
ne vaut que pour le Ne pas déranger — jamais pendant un partage d'écran ni en sourdine.

Les notifications des autres applis reçues pendant un silence sont créées en urgence basse (listées,
sans bulle) ; en Ne pas déranger, GNOME retient de toute façon ses propres bulles. Un message arrivé
avant la session en cours avec le moteur (Shell verrouillé, ou tout juste démarré) est compté, pas
rejoué en pastille.

Limites : un partage d'écran commencé **avant** l'activation de l'extension n'est pas vu (les poignées
ne s'énumèrent pas) ; la détection n'a été jouée que par une doublure du pilote, aucun partage réel
n'étant possible dans le Shell sans écran.

### Les vieilles notifications ne reviennent pas

Constat réel (reconnexion de 19:18, 16 novembre 2026) : le moteur a renvoyé 31 notifications, toutes
`silent:true`, et l'extension d'alors les recréait dans la liste de GNOME à chaque reconnexion. Règles :

- une notification `silent` (déjà sur l'iPhone à la connexion), ou venue de la liste
  `bt_notifications`, ne crée **jamais** de notification GNOME : ni bulle, ni ligne dans la liste ;
- une notification déjà montrée, ou déjà vue comme ancienne, n'est pas remontrée, même renvoyée sans
  `silent` ou sous un autre `uid` : l'empreinte `app_id` + `timestamp` + titre + sous-titre + texte est
  gardée le temps de la session du Shell (`_shownNotes`, 2000 au plus). Le `uid` seul ne suffit pas : il
  est propre à une session ANCS. Une notification encore affichée peut être corrigée (nom d'appli) ;
- `bt_notification_removed` retire la nôtre si elle existe ; la liste complète retire celles qui n'y
  sont plus ;
- les messages déjà vus (`handle`) ne font plus grossir la pastille après une reconnexion ni après un
  redémarrage du moteur : `_seen` vit le temps de la session du Shell.

Les anciennes ne sont pas listées ailleurs (pas de section « On your iPhone » dans le menu).

### Le quotidien : son, batterie, présence, autres applis, connexion

Branché sur `docs/moteur-quotidien.md`. Tout est toléré absent : avec un moteur plus ancien, pas de
réglage du son dans le menu, un seul « Answer », la pastille de batterie d'avant.

- **Son.** « Play iPhone audio on this computer » (menu au repos et Phone Settings, éteint par défaut) :
  `bt_set_phone_audio` avec `route` `here` ou `phone`, état lu dans `bt_audio.phone_audio`.
- **Appels.** Risque signalé par le moteur : répondre depuis l'ordinateur avec le son laissé sur
  l'iPhone peut donner un appel silencieux. Quand l'ordinateur peut porter le son
  (`bt_audio.call_audio_switchable`), la carte d'appel entrant propose donc **« Answer here »**
  (`bt_call_action answer` avec `audio:"here"`) et **« Answer on iPhone »** (`audio:"phone"`), plus
  « Decline », au lieu d'un seul « Answer ». Après « Answer on iPhone », la carte d'appel ajoute « No
  sound on the iPhone? Tap Audio there and choose iPhone. ». Pendant l'appel, « iPhone » / « This
  computer » envoient `bt_set_call_audio` avec `persist:false` (cet appel seulement). La carte ne
  montre jamais ce qui a été demandé comme fait : tant que `calls.audio_route` ne dit pas que le son est
  arrivé, le côté demandé est marqué en cours (« 02:41 · moving to iPhone… ») ; si le moteur répond que
  le lien voix ne s'est pas fermé (`bt_call_audio_result` en échec), ou si rien n'a bougé au bout de
  **3 s** (`AUDIO_MOVE_MS` ; 6 s après « Answer here », `AUDIO_ANSWER_MS`), la carte dit « Couldn’t move
  the call to the iPhone » (ou « …to this computer »), revient sur l'état réel et dit où le faire sur
  l'iPhone. Si le son finit par bouger, l'erreur s'efface. Côté moteur : `docs/moteur-api.md`,
  section 9, « Rendre le son à l'iPhone en cours d'appel ».
- **Batterie.** `bt_battery_low` → notification GNOME « iPhone battery at 20 % », une par seuil
  franchi (un franchissement des deux à la fois n'annonce que le plus bas ; réarmement à seuil + 5),
  réglage `battery-alerts`. Pas en silence.
- **Présence.** `bt_presence` donne la raison montrée dans le menu : `out_of_range` « Out of range »,
  `switched_off` « Bluetooth is off on the iPhone », `computer_suspended` « Reconnecting after sleep… »,
  liens tombés mais `state:"here"` « Reconnecting… » ; `lock` `pending` / `locked` est ajouté (« this
  computer locks shortly », « this computer was locked »). Réglage « Lock this computer when I walk away
  with my iPhone » (Phone Settings, éteint par défaut, délai de 5 à 600 s) : `bt_set_lock_on_away` avec
  `enabled` et `grace_seconds`. Le texte dit le délai réel (départ constaté 20 à 35 s après la perte du
  signal) et que rien ne se déverrouille seul. C'est le moteur qui verrouille.
- **Réglages tenus par le moteur.** Phone Settings tourne dans un autre processus : les trois réglages
  ci-dessus passent par des clés miroir (`phone-audio-here`, `lock-on-away`, `lock-away-seconds`). Une
  clé changée part au moteur (`wantSetting()`), une valeur tenue par le moteur revient dans la clé
  (`onSetting`) ; pendant 8 s après un envoi, une valeur plus ancienne encore en route n'est pas prise
  pour un changement.
- **Autres applis : « Reply in app ».** Sur une notification de WhatsApp, Telegram ou Signal, une action
  ouvre le client officiel sur la conversation : `https://web.whatsapp.com/send?phone=…`,
  `tg://resolve?phone=…`, `sgnl://signal.me/#p/+…` (`lib/replyLinks.js`). Seulement si l'expéditeur est
  sûr : titre seul (un sous-titre, « Nom @ Groupe », « ~pseudo » écartent), puis soit le titre est un
  numéro complet, soit le carnet (`bt_list_contacts` sur le nom) contient **un seul** contact de ce nom
  exact avec **un seul** numéro, écrit avec son indicatif. Jamais pour un groupe. Les liens `tg:` et
  `sgnl:` ne sont proposés que si un programme de l'ordinateur les ouvre. L'action n'efface rien sur
  l'iPhone.
- **Connexion de l'iPhone.** NetworkManager crée un profil Bluetooth pour l'iPhone appairé
  (« <nom> Network »), que GNOME montre en tuile « Tether ». Le menu au repos offre la même chose sous
  son nom : « Use iPhone's Internet Connection », avec « Connecting… », « Connected… », ou en cas d'échec
  « Turn on Personal Hotspot (Partage de connexion) on your iPhone, then try again. » `lib/hotspot.js`
  repère par libnm (`gi://NM`) le profil dont l'adresse (`bluetooth.bdaddr`) est celle de l'iPhone,
  l'active et le désactive, suit son état ; pas de profil, pas d'entrée. Rien n'est créé, masqué ni
  supprimé : la tuile de GNOME reste.

### Parcours d'accueil

Un seul parcours, du premier clic jusqu'à « tout marche » : une `ModalDialog` du Shell (`lib/setup.js`)
qui ne fait qu'afficher `view().tour`. L'état vit dans le modèle ; fermer la boîte ne le perd pas.

En haut, toujours : « Step n of N — nom de l'étape », une barre de progression, et la ligne « On your
iPhone: … » qui dit où être sur le téléphone à cet instant. N compte les étapes que ce parcours-ci
traverse vraiment (7 avec l'autorisation, 6 sans, 3 pour finir les interrupteurs, 2 pour la seule
autorisation d'un iPhone déjà relié).

| Étape | Ce qu'elle montre | Sortie |
|---|---|---|
| `permission` | seulement les réglages qui manquent sur cette machine, un ou deux points (`tour.permissionNeeds` : `experimental`, `ertm`), l'avertissement si le Bluetooth doit redémarrer ; l'étape n'existe pas s'il n'y a rien à demander | « Continue » lance `pkexec … apply` ; « Waiting for your password… » tant que la fenêtre du système est ouverte ; reprise seule dès que `apply` a réussi. Refus : même étape, « No password, so nothing was changed », « Try Again » |
| `repair` | deux iPhone dessinés : ⓘ à côté du nom d'hôte, puis « Forget This Device » | « Done on My iPhone » : l'ordinateur retire alors l'appairage lui-même (`bt_unpair`), puis recherche |
| `find` | Réglages › Bluetooth dessiné ; « Looking for your iPhone… » puis « Found <nom> » | `bt_scan` ; un seul iPhone : `bt_pair` part tout seul ; plusieurs : liste |
| `code` | la demande de jumelage dessinée avec le vrai code, le même code en grand à côté, décompte | « They Differ » / « They Match », aucun n'est le bouton par défaut |
| `pairing` | « Pairing… », deux lignes qui se cochent (lien appels/messages, lien notifications) | les deux liens réellement montés |
| `switches` | deux iPhone dessinés (ⓘ, puis la fiche dans l'ordre réel, dont les trois interrupteurs s'allument en direct), trois lignes qui se cochent dans l'ordre de la fiche (Show Message Notifications, Sync Contacts, Share System Notifications), l'aide repliable « Don't see these switches? » ; `bt_retry_profiles` à l'entrée puis toutes les 3 s tant qu'un interrupteur manque | les trois activés ; « Finish Later » |
| `try` | un écran verrouillé dessiné ; « Waiting for a message… » | un message arrive (notification de Messages ou `bt_message`) : montré, « It works » ; « Skip » |
| `done` | batterie, contacts, conversations, appels ; trois lignes sur le quotidien | « Done » |
| `forget` | hors numérotation (pas de « Step n of N ») : la question, l'attente, puis les deux iPhone dessinés pour « Forget This Device » | « Close » ou « Link Again » |

Règles :

- **Étapes déjà satisfaites sautées au départ** (réglages bons, iPhone relié), **jamais en route** : ce
  qui est affiché avance par une file, chaque étape reste au moins 1,5 s (`TOUR_MIN_STEP_MS`) et le
  changement glisse (220 ms). Le protocole, lui, n'attend pas : seul l'affichage est retenu.
- **« Relié » se constate.** `bt_pair_result` réussi ou `already_paired` ne prouvent rien : l'étape
  `pairing` attend `classic_connected` **et** `le_connected` (45 s, 15 s après `already_paired`). Sans
  cela, ou si le moteur dit `needs_repair`, `stale_bond` ou `dual_bond:false` : étape `repair`. Une
  seconde panne après réparation finit en échec expliqué, sans boucle. Rien n'est retiré avant le clic
  sur « Done on My iPhone » ; si les liens montent entre-temps, la réparation est retirée d'elle-même.
- **Appairage déjà connu de l'ordinateur** au début de la recherche (appareil `paired` sans
  `bond_state:"ok"`) : réparation d'abord, pas de `bt_pair` qui sauterait le code. Après une
  réparation, `bt_pair` part avec `fresh:true`.
- **Santé de l'appairage** (`docs/contrat-appairage.md`) : `needs_repair` fait foi quand le moteur le
  publie. Avec un moteur plus ancien, l'extension déduit : appairage sans moitié LE
  (`capability.bond_has_le`, `le_available`) ou `link_reason` parlant d'authentification. Un `partial`
  sans remède (`needs_repair:false`) n'est pas envoyé en réparation : la ligne des notifications est
  alors dite indisponible.
- **Fermer.** Échap met le parcours de côté quand quelque chose est en cours (recherche, code, attente,
  interrupteurs, réparation) : l'indicateur dit « Continue setup » et un clic le rouvre à la même étape.
  Ailleurs Échap l'arrête. « Cancel », « Not Now », « Finish Later » l'arrêtent toujours (un code en
  attente est refusé). Relié mais interrupteurs incomplets : le menu propose « Continue Setup… ».
- **Clavier.** La boîte est modale ; après chaque changement le bouton principal reprend le clavier, et
  de nouveau dès que la boîte redevient la fenêtre du dessus (retour de la fenêtre de mot de passe).
  Sur l'étape du code, le clavier repose sur la boîte elle-même : Entrée ne confirme rien.
- Échecs dits en clair (délai, refus, iPhone introuvable, tentative en cours), « Try Again » reprend à la
  recherche. Une demande de code arrivée hors parcours reste traitée par la pastille « Confirm link ».

### Messages et contacts non proposés (OBEX injoignable)

Incident du troisième essai : le moteur ne joignait plus obexd (bus de session), l'ordinateur n'annonçait
donc ni messages ni contacts, et l'iPhone ne montrait sur sa fiche ⓘ que « Device Type » et « Share System
Notifications ». Attendre les interrupteurs ne sert alors à rien. Le modèle reconnaît ce tableau
(`_obexState()`), dans le parcours comme au repos :

- **signe certain** : `map_error` ou `pbap_error` vaut `bus_closed` (moteur récent) : dit tout de suite ;
- **repli** pour un moteur plus ancien : `classic_connected`, `map_open` et `pbap_open` faux, les deux
  motifs ni `none`, ni `forbidden` (interrupteur simplement éteint : le cas normal), ni `busy` (un autre
  ordinateur tient la session), aucun service relâché, appairage sain, pendant 20 s (`OBEX_STUCK_MS`).

| État (`tour.obex`, et avis du menu) | Ce qui est montré | Suite |
|---|---|---|
| `down` | « This computer can't offer messages and contacts right now. », les deux fiches ⓘ dessinées côte à côte (attendue / réduite) | « Fix It » : `systemctl --user restart landline-engine.service` |
| `restarting` | « Restarting the phone link… » ; le parcours survit à la disparition du moteur, l'indicateur ne dit pas « Engine not running » | 45 s au plus (`FIX_TIMEOUT_MS`) |
| `fixed` (parcours seulement) | « Messages and contacts are on offer again », rouvrir ⓘ sur l'iPhone ; l'aide repliable reste là | s'efface au premier interrupteur allumé |
| `still` | la panne est revenue après la relance : « Restarting did not bring messages and contacts back. » | « Forget This iPhone… » (repartir à neuf), plus de « Fix It » |
| `failed` | `systemctl` a échoué ou le moteur n'est pas revenu | « Fix It » de nouveau, et l'oubli |

Un moteur récent qui publie `bus_closed` sort de lui-même deux secondes plus tard et systemd le relance :
le modèle traite ce départ comme une relance demandée (parcours gardé, puis `fixed` ou `still`).
`bt_retry_profiles` n'est pas envoyé tant qu'une panne est dite : il ne s'agit pas des interrupteurs.

L'aide « Don't see these switches? » (étape des interrupteurs, hors panne) montre les mêmes deux fiches,
dit quoi faire (fermer la page, la rouvrir) et propose l'oubli.

### Première ligne du menu au repos

Une seule ligne (`TopRow`, `lib/cards.js`) : le glyphe du téléphone à gauche (ambre quand l'iPhone est
relié, atténué sinon), **le nom de l'iPhone au milieu de la largeur du menu** (celui que donne le moteur
dans `bt_devices`), l'état en un ou deux mots (`card.state` : « Linked », « Out of range », « Bluetooth
off », « Reconnecting… », « Not connected », « Paused ») puis la batterie (« 96 % », rouge sous 15 %)
calés à droite. Le nom reçoit la place que laissent les deux côtés quand chacun compte pour la largeur
du plus large : il est donc centré sur le menu, pas sur l'espace libre ; trop long, il est coupé par des
points de suspension (un nom d'un seul emoji tient au milieu). Les trois textes sont posés sur la même
ligne de base. Si la place centrée tombait sous 64 px, le nom prendrait l'espace entre les deux côtés
(cas non rencontré dans les essais). Ce que le mot d'état ne dit pas passe en phrase sous la ligne
(« Bluetooth is off on the iPhone. », « This computer was locked when the iPhone left. ») ; l'opérateur
n'est plus affiché (il reste dans le nom lu par les lecteurs d'écran, avec `statusLine`).

### Oublier l'iPhone

« Forget This iPhone… » se trouve dans « Phone Settings » (dernier groupe de la page, seul, en rouge ;
la question est posée par le Shell avant tout retrait) et dans le parcours là où il peut être bloqué (aide des interrupteurs, panne non réparée, réparation, échec). Déroulé, dans la boîte
du parcours (étape `forget`) :

1. la question (« Forget <nom> ? », ce qui va se passer en trois lignes) ; « Forget » n'est pas le bouton
   par défaut ; « Back » ramène à l'étape d'où l'on vient, « Cancel » ferme ;
2. `bt_unpair` pour l'iPhone choisi (à défaut, l'appairage connu de BlueZ) ; `bt_unpair_result` attendu
   15 s. `not_found` vaut succès (déjà oublié ici). Échec ou silence : dit, « Try Again » ;
3. les deux iPhone dessinés (ⓘ, puis « Forget This Device ») : l'iPhone garde sa propre fiche, que
   personne ne peut retirer à la place de l'utilisateur ; « Close » laisse l'état clair (« Link iPhone »,
   la sélection ayant été effacée par le moteur : `selection_cleared`), « Link Again » enchaîne sur le
   parcours d'appairage depuis la recherche (`bt_pair` avec `fresh:true`, et pas de seconde réparation en
   boucle si cela échoue encore).

« Phone Settings » tourne dans un autre processus : son bouton lève le réglage `forget-request`, que
l'extension remet aussitôt à faux avant d'ouvrir la question dans le Shell (un reste d'une session
précédente est ignoré à l'activation). Moteur absent : une notification le dit, rien n'est ouvert.

### Nouveau message

« New Message… » (menu au repos, quand les messages sont lisibles) ouvre une carte avec un champ : chaque
frappe envoie `bt_list_contacts` (`query`, 6 résultats), une ligne par adresse du contact ; un numéro
complet tapé à la main est proposé tel quel. Choisir une ligne (ou Entrée pour la première) ouvre la carte
de réponse habituelle sur ce fil, vide s'il est nouveau ; l'adresse rendue par le moteur sert de `thread`
à `bt_send_message`.

L'assistant système `landline-setup` (dossier `system/`) **n'est pas dans l'extension** et n'est jamais
élevé depuis un fichier que l'utilisateur peut modifier (`lib/setupHelper.js`) :

- `pkexec` n'est lancé que sur `/usr/local/libexec/landline/landline-setup`, et seulement si ce
  fichier appartient à root et n'est inscriptible ni par le groupe ni par les autres (`rootHelper()`).
  Aucun chemin ne vient de l'environnement. Le processus est attendu et son code traité (`runAsRoot()` :
  `done`, `partial`, `refused`, `failed`), pour `apply` (parcours) comme pour `revert` (« Undo » de Phone
  Settings, qui n'existe que si cette copie existe) ;
- `status` (sans droits) est lu sur cette copie, à défaut sur `~/.local/lib/landline/landline-setup`
  que pose `scripts/install-local.sh` ; il est lu au démarrage et après chaque `apply` ;
- sans copie de root, l'étape d'autorisation du parcours ne demande aucun mot de passe : elle montre la
  commande à lancer soi-même (`sudo '<copie locale>' apply`, à défaut les commandes de
  `capability.setup` du moteur), avec « Copy Commands » ; `status` est relu toutes les 2 s tant que
  l'étape est à l'écran, et le parcours avance seul quand les réglages sont faits. Lancée ainsi, la
  commande installe aussi la copie de root : la fois suivante, la fenêtre de mot de passe sert.

L'autorisation est proposée quand, pour un réglage exigé
(`experimental`, `ertm` ; `required.<clé>` à faux l'en dispense), la valeur courante ou la valeur de
`persistent` est fausse (`_systemNeeds()`). La classe d'appareil n'est plus demandée : `device_class` est
ignoré, et une étape de classe encore listée par un moteur plus ancien est écartée. Code 2 (réglages écrits, une activation échouée) : dit dans
le parcours, avec la consigne de redémarrer. Sans assistant, on retombe sur `capability.setup` du moteur :
carte avec les étapes en clair et « Copy Commands ». Bluetooth éteint : « Bluetooth is off », pas
« Permission needed ».

### Illustrations

`illustrations/*.svg` sont des modèles : `bluetooth` (Réglages › Bluetooth), `request` (la demande de
jumelage), `info` (la fiche ⓘ, voir ci-dessous) et `lock` (écran verrouillé). `lib/illustrations.js` y écrit le nom d'hôte tel que l'iPhone l'affiche, le code, l'état des
interrupteurs, les libellés (`{{L_…}}`), et rend une `Gio.BytesIcon` qu'un `St.Icon` dessine à la taille
voulue.

**La fiche ⓘ**, relevée sur une capture de l'iPhone de l'utilisateur (iOS 26), de haut en bas :
« Afficher les notifications de message » (interrupteur, légende « Autorisez l'affichage des
notifications de message sur cet appareil. ») ; « Synchroniser les contacts » (interrupteur, légende
« Autorisez cet accessoire à accéder à vos contacts pour les appels et les messages. ») ; section
« Groupes » : « Favoris du téléphone », « Historique », « Tous les contacts » (coches) ; « Type
d'appareil » (chevron) ; section « Notifications système » : « Partager les notifications système »
(interrupteur) ; « Oublier cet appareil ». Il n'y a pas de ligne « Nom ». Le dessin suit cet ordre et
ces libellés ; les groupes et le type d'appareil sont atténués (rien à y faire), les interrupteurs
encore éteints sont cerclés. Variante `bare` (ordinateur qui n'offre ni messages ni contacts) : « Type
d'appareil », « Partager les notifications système », « Oublier cet appareil ». Équivalents anglais :
« Show Message Notifications », « Sync Contacts », « Groups », « Phone Favorites », « Recents », « All
Contacts », « Device Type », « System Notifications », « Share System Notifications », « Forget This
Device » ; les deux légendes anglaises sont une traduction, pas un relevé.

**Langue de l'iPhone.** Les libellés des dessins viennent de `lib/phoneLabels.js` (anglais et français ;
les français ont été relevés sur un iPhone sous iOS 26). La langue est celle du réglage `phone-language`
(« Language of the iPhone » dans Phone Settings), sinon une estimation : la langue du bureau
(`GLib.get_language_names()`) ; si c'est l'anglais, celle des formats régionaux (`LC_TIME`) quand elle
est connue — un bureau en anglais avec des formats français est pris pour un iPhone en français. Les
textes de l'interface restent en anglais : quand l'iPhone parle une autre langue, ils rappellent son
libellé entre parenthèses (« tap Pair (Jumeler) », « Settings (Réglages) › Bluetooth ») et les légendes
sous les dessins citent le mot dessiné (« Tap “Oublier cet appareil” »). L'élément à toucher est cerclé d'ambre. Le téléphone est dessiné sombre dans
les deux styles du Shell. Piège : le chargeur d'images cherche `<svg` dans les premiers octets, donc le
commentaire d'en-tête est placé *dans* la balise racine.

### Contrôle d'appel

Le moteur démarre avec le contrôle d'appel coupé (`calls_enabled:false`). Dès qu'un iPhone est relié, le
modèle envoie `bt_set_calls` avec la valeur du réglage `call-control` (vrai par défaut, modifiable dans
« Phone Settings »), une seule fois par écart constaté.

### Cartes et vraies données

- Tout ce qui descend sous l'indicateur a **une largeur fixe** (23 em) ; noms, aperçus et réponses
  toutes faites finissent par « … » plutôt que d'élargir.
- Expéditeur : nom du contact, sinon numéro écrit lisiblement (`formatNumber` : +41 79 000 00 01), code
  court ou adresse iMessage tels quels. Pas de lettres : pictogramme à la place des initiales ; une
  adresse donne les initiales de sa partie locale.
- Carte de réponse : les derniers messages non lus du fil (trois au plus), en bulles ; quatre lignes
  pour un message seul, trois s'ils sont plusieurs, « Show more » / « Show less » au clic. Texte vide
  (photo, pièce jointe) : dit tel quel. Un message qui arrive carte ouverte s'y ajoute. Pas de réponses
  toutes faites pour un code court.
- Pastille : largeur maximale stricte (19 em), nom et texte bornés séparément.

### Vues

- `indicator.js` : un `PanelMenu.Button` ajouté par `Main.panel.addToStatusArea(uuid, bouton, 0, 'right')`,
  donc premier de la boîte de droite : la pastille grossit vers l'horloge et ne pousse rien. La pastille
  *est* le bouton de la barre (même forme, même survol que les autres) : seule sa couleur de fond change.
  La largeur glisse de l'ancienne à la nouvelle en 200 ms (`ease`), puis redevient libre.
- `cards.js` : le contenu du `PopupMenu` du bouton, reconstruit à chaque ouverture. Le menu de repos est
  fait d'éléments de menu standard ; les cartes sont des `St.Button` / `St.Entry` dans un élément de menu.
- Appel entrant : la carte se montre seule. Pour ne pas prendre le clavier à ce que l'utilisateur est en
  train de taper, c'est un second `PopupMenu`, jamais rendu modal, déclaré auprès du gestionnaire de mise
  en page (`addTopChrome`) pour recevoir les clics. Un clic sur la pastille la remplace par la carte
  ordinaire.
- `notifications.js` : une `MessageTray.Source` par appli, nommée d'après elle. Une notification déjà
  présente sur l'iPhone est listée sans bulle : c'est le champ `silent` de `bt_notification` qui les
  distingue (vrai pour toute la pile renvoyée à chaque reconnexion) ; ces notifications sont créées en
  urgence basse, que le Shell range dans la liste sans rien faire surgir. Effacée à la main ici, elle est effacée sur
  l'iPhone (`bt_notification_action`, action `negative`) ; retirée sur l'iPhone, elle disparaît ici.

### `disable()`

Minuteries retirées, processus en cours (`systemctl`, assistant système) terminés — sauf l'assistant
déjà lancé en root par `pkexec`, qui finit seul —, boîtes de dialogue détruites (y compris celle qui
finit son fondu de fermeture), client arrêté (socket fermé, opérations annulées), signaux des réglages déconnectés,
bouton détruit (son menu, la carte d'appel, la minuterie de reconstruction, l'animation), sources de
notification retirées sans rien relayer à l'iPhone. Le guetteur de silence (réglage de GNOME, contrôleur
d'accès distant) et la connexion de l'iPhone (client NetworkManager, minuterie) sont déconnectés et
détruits.

## 2. Essayer sans iPhone

### Faux moteur

```sh
tools/faux-moteur.py --socket /tmp/essai/faux.sock            # relié
tools/faux-moteur.py --socket /tmp/essai/faux.sock --etat hors-portee
tools/faux-moteur.py --socket /tmp/essai/faux.sock message attendre:9 appel
```

Il écoute sur le socket donné (il refuse le chemin du vrai moteur), répond comme le démon — réponses
directes, `OK` non JSON, diffusions aux abonnés, `bt_call_result` en double — et joue des scénarios donnés
en arguments ou tapés sur son entrée standard : `relie`, `hors-portee`, `message [texte]`,
`message-leo [texte]`, `appel`, `decroche`, `raccroche`, `batterie-faible`, `batterie N`, `permission`,
`permission-ok`, `a-relier`, `sans-iphone`, `appairage`, `notification`, `retire-notification`,
`bluetooth-eteint`, `bluetooth-allume`, `obex-injoignable` (liens montés, MAP et PBAP en échec `other`),
`obex-tenace` (une relance n'y change rien), `obex-retabli`, `bus-ferme` (motif `bus_closed`, puis le moteur
sort et revient seul), `relance` (tous les clients déconnectés, liens qui retombent puis remontent),
`refuse-oubli` / `accepte-oubli`, `expire-appairage`, `interrupteur notifications|messages|contacts`, `rafale [N]`, `refuse-envoi`,
`attendre:N`, `quitter`. Passe 4 : `liens-battants [N [s]]` (les liens montent et retombent N fois, une
bascule toutes les s secondes, sans qu'aucun service ne s'ouvre) et `liens-stables` ; `reconnexion` (le
lien tombe et remonte, toute la pile de notifications est renvoyée `silent`) et `renvoi-bruyant` (la même
pile sans `silent`, sous d'autres numéros) ; `eloigne [motif [verrou]]` (`bt_presence`) ;
`quotidien-ancien` / `quotidien-nouveau` (moteur d'avant ou d'après `bt_audio`, `bt_presence`,
`bt_battery_low`) ; `appels-sans-son-ici` / `appels-son-ici` ; `son-coince` / `son-libre` (le son d'un
appel ne repart pas vers l'iPhone) ; `notification-appli whatsapp|telegram|signal|groupe [expéditeur]`.
Il répond à `bt_audio`, `bt_presence`, `bt_set_phone_audio`, `bt_set_call_audio`, `bt_set_lock_on_away`,
publie `battery_state`, `calls.audio_route` et `bt_battery_low` au franchissement de 20 % et 10 %, et
ne liste plus la classe d'appareil parmi les prérequis. Vraies données : `message-numero`, `message-code`, `message-email`,
`message-long`, `message-emoji`, `message-vide`, `message-suite`, `message-de QUI [texte]`,
`historique-reel`, `appel-inconnu`. Appairage : `appairage-partiel` (appairage BR/EDR seul fait hors
parcours), `echecs-authentification`, `sans-le` / `avec-le`, `contrat-ancien` / `contrat-nouveau`
(moteur d'avant ou d'après `docs/contrat-appairage.md`), `delai balayage|demande|oubli|liens|relance N`.
Il répond aussi à `bt_scan` (l'iPhone apparaît au bout d'une seconde), à `bt_pair` (code 703405, liens
montés un instant plus tard, trois interrupteurs éteints ; `stale_bond` sur un appairage inutilisable,
`fresh`), à `bt_unpair` et à `bt_set_calls` (le contrôle d'appel démarre coupé, comme le vrai). Chaque commande reçue est affichée, précédée de `RECU`.
Toutes les données sont inventées (Camille Martin, Léo, numéros en +4179 000 00 0x).

### Tests

```sh
tests/run.sh        # modèle (424 vérifications) puis client contre le faux moteur (72)
```

### Shell de test, isolé de la session

```sh
tools/shell-test/run.sh
```

Le script lance `gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x720` dans un bus D-Bus
privé (`dbus-run-session`), avec `HOME`, `XDG_*` et le dossier d'exécution dans un dossier temporaire de
`/tmp` : ni les réglages, ni les extensions, ni la session de l'utilisateur ne sont touchés, et le socket
du vrai moteur n'existe pas dans ce dossier d'exécution : le faux moteur y écoute au même nom
(`$XDG_RUNTIME_DIR/landline/landlined.sock`, dans le dossier d'exécution de l'essai), si bien que
l'extension n'a besoin d'aucune variable pour le trouver. **L'extension copiée dans ce Shell est celle
qui serait publiée, sans rien pour les essais.** Tout ce qui sert aux essais vit dans le pilote
(`tools/shell-test/pilote@test`, hors de `extension/`), activé avant elle : il charge les modules de
l'extension (le Shell reprend les mêmes) et remplace trois choses par des doublures : l'assistant
système (`helper`, `helper user`, `nohelper` : copie de root, copie sans droits, rien), NetworkManager
(qui vit sur le bus système, le vrai : jamais joint ; `hotspot profil|sans-profil|refuse|accepte`) et le
partage d'écran (`sharing`). Il retrouve aussi lui-même les boutons, par leur libellé ou leur classe de
style : l'extension n'expose plus de table de contrôles. `tools/shell-test/bin` passe en tête du `PATH` :
le `pkexec` qui s'y trouve ne demande aucun mot de passe et n'élève aucun droit (il lance la commande, ou
refuse comme si l'utilisateur avait annulé, ou attend tant qu'un fichier témoin existe : le temps passé
devant la fenêtre de mot de passe). Le pilote sait aussi basculer le Ne pas déranger (`dnd`), changer un
réglage (`reglage`), amener le pointeur sur une bulle (`hoverbanner`), faire défiler une fenêtre
(`scroll`) et couper ou remettre l'indicateur stable (`steady`). Enfin le `systemctl` note l'appel et échoue, sauf `restart` qui fait jouer `relance` au faux moteur (et échoue
si le fichier `$FAUX_SYSTEMCTL_REFUSE` existe). `LC_ALL` est fixé à l'anglais pour que la langue devinée de
l'iPhone ne dépende pas de la machine. Pour la reprise après le mot
de passe, le pilote ouvre une vraie `ModalDialog` du Shell par-dessus le parcours, comme le fait l'agent
polkit, puis la referme.
Une petite extension d'essai (`tools/shell-test/pilote@test`) fait de vrais clics de souris et de vraies
frappes par un périphérique virtuel de Clutter, lit l'état et prend les captures. Le script déroule tous
les états, vérifie chacun, copie les captures dans `docs/captures/` et relève les erreurs JS du journal du
Shell (`tools/shell-test/out/shell.log`). `SECTIONS="parcours reparation panne cartes stable silence quotidien courant"`
limite l'essai à certaines parties ; `SCHEME=prefer-light SHOTS=<dossier>` le joue en style clair.

Pour jouer à la main dans une fenêtre, même principe avec `--devkit` à la place de `--headless` (non
essayé ici).

## 3. Installer dans la vraie session

Le moteur doit être installé (unité `landline-engine.service`, ou lancé à la main ; voir
`docs/moteur-api.md`, section 11). L'archive ne contient que `extension/` (avec `LICENSE`) : ni
l'assistant système, ni rien des essais.

```sh
cd ~/DEVELOPEMENT/iphone-link
scripts/pack-extension.sh          # écrit dist/landline@d101c.github.io.shell-extension.zip
gnome-extensions install --force dist/landline@d101c.github.io.shell-extension.zip
```

Fermer la session et la rouvrir (sous Wayland, le Shell ne découvre une nouvelle extension qu'au
démarrage), puis :

```sh
gnome-extensions enable landline@d101c.github.io
```

Pour retirer : `gnome-extensions disable landline@d101c.github.io` puis
`gnome-extensions uninstall landline@d101c.github.io`.

L'extension ne lit aucune variable d'environnement : la vraie session ne peut plus être branchée sur le
faux moteur. Les essais passent par le Shell isolé.

### À déclarer dans la description de la fiche (extensions.gnome.org)

- **Presse-papiers** : « Copy Commands » (carte de permission et étape d'autorisation du parcours)
  **écrit** les commandes dans le presse-papiers, sur un clic (`_copyCommands()` dans `extension.js`,
  seul appel à `St.Clipboard` de l'extension). L'extension ne **lit** jamais le presse-papiers (vérifié :
  aucun `get_text`, aucun autre accès). La règle d'EGO demande de le dire dans la description, qui ne le
  dit désormais (`docs/ego/description.txt`, repris dans `metadata.json`) ;
- le mot de passe administrateur n'est demandé que par `pkexec` sur l'assistant installé chez root ;
- `systemctl --user start|restart landline-engine.service`, les liens « Reply in app », et
  NetworkManager pour la connexion de l'iPhone.

Signalements restants de `shexli` sur l'archive : EGO-A-005 (ce presse-papiers, à déclarer) et
EGO-M-004 (faux positif connu : shexli 0.2.1 ne connaît pas la version 51).

## 4. Ce qui a été vérifié, et comment

- `tests/run.sh` : règles du modèle et client contre le faux moteur.
- `tools/shell-test/run.sh` : tous les états dans un vrai GNOME Shell 51 sans écran, avec de vrais clics
  et de vraies frappes ; le journal du Shell ne doit contenir aucune erreur JS. Captures dans
  `docs/captures/` :
  - `t00` à `t16` : le parcours d'accueil, étape par étape (autorisation, refus, attente du mot de
    passe, recherche, mis de côté, trouvé, code, code expiré, code refusé, jumelage, interrupteurs,
    deux coches, essai, essai réussi, retour, fin) ;
  - `r1` à `r6` : appairage inutilisable et réparation guidée ; `p4`, `p5` : autorisation d'un iPhone
    déjà relié ;
  - `o1` à `o10` : aide repliée et dépliée, panne détectée, relance, rétabli, relance sans effet, les
    avis du menu (panne, relance sans effet, relance refusée), motif `bus_closed` ;
  - `f1` à `f8` : oubli (question depuis le parcours, menu sans l'entrée, question demandée depuis
    Phone Settings, attente, côté iPhone, après oubli, refus de BlueZ, échec du parcours avec l'oubli
    proposé) ; `33` : le bas de Phone Settings, avec le groupe de l'oubli ;
  - passe 5 : `02`, `06`, `e1`, `e2` (première ligne du menu : nom centré, emoji seul, nom long coupé),
    `avant-02`, `avant-06` (la même avant la passe), `d3`, `d4` (motif de l'absence sous le nom), `e3`,
    `d1`, `e4` (bascule du son en cours, refusée, réussie), `t17` (autorisation sans copie de root : la
    commande à lancer soi-même) ;
  - `l1` à `l5` : iPhone en français (recherche, code, interrupteurs, aide, oubli) ;
  - `n2` à `n4` : nouveau message (champ, résultats, saisie) ;
  - `m01` à `m14`, `c1`, `c2` : menu et cartes avec de vraies données ;
  - `01` à `32`, `b1`, `n1`, `n5` : usage courant (`n1` : liste du calendrier vide après 31
    notifications préexistantes ; `n5` : une seule notification après reconnexion et renvoi) ;
  - `s1` à `s6` : indicateur stable (relié, hors de portée, non-lu à largeur égale, liens qui battent,
    « Link again », réparation ouverte par le clic) ;
  - `q1` à `q7` : silence (message et appel en Ne pas déranger, menu ouvert exprès, écran partagé, menu
    qui dit pourquoi, sourdine, appels autorisés) ;
  - `d1` à `d5` : quotidien (son qui ne repart pas, réglage du son, Bluetooth coupé sur l'iPhone, parti
    et verrouillé, « Reply in app » sur une bulle dépliée) ; `h1` à `h5` : connexion de l'iPhone
    (éteinte, en cours, établie, refusée, refusée avec le libellé français) ;
  - `p6` : autorisation réduite à un seul point ; `15` : carte d'appel à deux réponses.
- Reprise après le mot de passe et clavier : vérifiés avec le faux `pkexec` qui attend et une fenêtre
  modale du Shell ouverte par-dessus (capture `t03`). Constaté : le parcours reste ouvert derrière, il
  avance pendant que la fenêtre est encore là, et reprend le clavier sur son bouton dès qu'elle se
  ferme, sans clic. **Non vu** : la vraie fenêtre de l'agent polkit, que le Shell isolé ne peut pas
  faire apparaître.
- Les libellés français des dessins et l'ordre de la fiche ⓘ viennent de l'iPhone de l'essai (iOS 26).
  **Non comparés à un iPhone** : la phrase de la demande de jumelage en français, « Connecté » /
  « Non connecté », « Autoriser », « Réglages », et tous les libellés anglais.
- « Fix It » n'a été joué que contre le faux `systemctl` et le faux moteur ; le bouton de Phone Settings
  n'a pas été cliqué (son effet, le réglage `forget-request`, est joué par le pilote).
- Rien de cette version n'a été essayé avec le vrai moteur, un vrai iPhone ni le vrai `pkexec`.
- Passe 4, **non vu pour de vrai** : la connexion de l'iPhone par NetworkManager (`lib/hotspot.js` n'a
  tourné qu'avec la doublure du pilote : ni la recherche du profil, ni l'activation, ni le suivi d'état
  par libnm n'ont été exécutés) ; la détection d'un partage d'écran réel ; l'ouverture effective des
  liens « Reply in app » (le bouton est vu, le clic n'a pas été fait ; `web.whatsapp.com/send` n'est pas
  une forme documentée par WhatsApp) et la forme réelle des notifications de groupe de chaque appli ;
  le son (aucune des commandes `bt_set_*audio` n'a touché un vrai moteur) ; le verrouillage à
  l'éloignement ; les nouveaux réglages de Phone Settings (la fenêtre s'ouvre, les lignes ajoutées ne
  sont pas dans la capture `32`, prise en haut de page).

## 5. Écarts par rapport à la maquette

- L'indicateur est dans les icônes système à droite, pas à côté de l'horloge (demandé).
- La carte d'appel entrant ne prend pas le clavier (second menu non modal) ; un clic sur la pastille
  l'ouvre en menu ordinaire.
- Entre l'envoi et « Sent », la pastille dit « Sending… » jusqu'à la réponse du moteur ; un envoi refusé
  donne « Not sent » et la carte revient avec le texte.
- Couleur : ambre par défaut. La couleur d'accent du système est un réglage (« Phone Settings »), éteint
  par défaut : avec un accent gris-bleu, le point « relié » ne se distinguerait plus du point « hors de
  portée ».
- Le parcours d'accueil réunit les maquettes G (autorisation) et H (appairage) en une seule boîte, avec
  des étapes en plus : réparation, attente des liens, essai. Les étapes « autorisation » et « fin » n'ont
  pas de dessin d'iPhone : il n'y a rien à y faire sur le téléphone, et le bandeau le dit.
- Messages en bulles (la maquette montre le texte nu) : plusieurs messages à la suite restent lisibles.
- Pour un appel d'un numéro sans nom, la pastille montre le numéro seul, sans « is calling ».
- « Recent calls » est un sous-menu en lecture seule (pas de rappel d'un clic).
- La carte d'appel ne dit pas « Mobile » : le moteur ne donne pas le type de ligne.
- La notification d'un appel manqué va dans la liste de GNOME : la pastille n'en garde aucune trace.

## 6. Ce qui reste à faire

- Après « Fix It », rien ne dit si l'iPhone relit de lui-même les services proposés ou s'il faut
  couper puis rallumer son Bluetooth : à observer en vrai. Le texte dit seulement de rouvrir ⓘ.
- Pendant un silence, la pastille « Sending… » / « Sent » d'une réponse que l'utilisateur vient d'envoyer
  lui-même ne se montre pas non plus (règle « rien ne grossit » appliquée sans exception).
- Le verrou « Link again » compte les chutes de tous les liens à la fois ; un lien BR/EDR qui bat pendant
  que le LE tient n'est pas compté.
- « Reply in app » ne préremplit aucun texte, et n'existe pas pour Discord, Messenger, Instagram.
- `bt_retry_profiles` part toutes les 3 s pendant l'étape des interrupteurs, alors que
  `docs/moteur-api.md` conseille « une fois par geste » : à réduire si le moteur s'en plaint.
- Les messages d'échec du parcours (« tap Pair on the iPhone ») viennent du modèle, qui ne connaît pas
  la langue de l'iPhone : pas de rappel entre parenthèses là.
- « New Message… » : pas de favoris proposés avant la frappe, pas de message de groupe.

- Le choix de la copie installée de l'assistant (comparaison des versions) et le code de sortie 2 sont
  écrits mais n'ont été exercés par aucun essai.
- Essai réel de cette version : le parcours entier, la fenêtre polkit, la réparation avec le moteur de
  `docs/contrat-appairage.md`, et la valeur de `calls.audio` qui signifie « son sur l'ordinateur »
  (supposée `active`).
- Un ordinateur dont le Bluetooth ne peut pas porter le LE : après une réparation restée sans moitié
  LE, le parcours s'arrête sur un échec expliqué ; il ne propose pas de continuer sans notifications
  (sauf si le moteur publie `needs_repair:false`).
- Si le moteur disparaît pendant le parcours, celui-ci est abandonné et repart du début.
- `formatNumber` connaît la Suisse, la France et l'Amérique du Nord ; les autres numéros sont groupés
  par trois.
- Montrer la pastille dès la notification de Messages (elle précède `bt_message` de 15 s au plus), puis
  y rattacher le fil pour la réponse.
- Codes à usage unique (`otp_available`), second appel en attente, composition d'un numéro : non traités.
- Traductions (gettext prêt, aucun fichier `.po`).
- Fiche extensions.gnome.org : `shexli` signale l'accès au presse-papiers (bouton « Copy Commands », à
  déclarer dans la description) et juge la version 51 « implausible » (outil en retard).
