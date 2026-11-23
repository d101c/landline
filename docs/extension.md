# Extension GNOME Shell

L'extension montre l'iPhone dans la barre du haut : une icône animée qui dit l'état à elle seule au repos
(deux styles au choix), une pastille quand un message ou un appel arrive, une carte sous la pastille pour
répondre. Elle ne parle qu'au moteur
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
  schemas/              vingt réglages (quiet-apps, keep-history, show-app-icons ; quick-replies, use-accent-color, icon-style, call-control,
                        phone-language, forget-request ; muted, follow-do-not-disturb,
                        quiet-while-sharing, calls-through-do-not-disturb ;
                        phone-audio-here, lock-on-away, lock-away-seconds, battery-alerts ;
                        link-mode, link-mode-state ; tour-request)
  icons/                glyphe téléphone, raccrocher, envoyer (symboliques)
  illustrations/        écrans d'iPhone dessinés (SVG à compléter) pour le parcours, et
                        Réglages › Partage de connexion pour « Internet from iPhone »
  lib/
    engineClient.js     client du moteur (Gio, sans Shell)
    names.js            les noms partagés avec le moteur et son installation (unité, socket,
                        assistant), en un seul endroit
    model.js            modèle d'état (JavaScript pur, sans Shell ni GLib) : assemble les modules
                        de model/
    model/              core (événements, temps), quiet (silence), link (état du lien, indicateur
                        stable), messages, notifications, calls, daily (réglages tenus par le
                        moteur, batterie), internet (« Internet from iPhone » : le réglage unique,
                        ses états et ses phrases), tour (parcours), obex (panne des messages
                        et contacts), forget (oubli), icon (ce que dit l'icône, combien de temps elle bouge), view (ce qui est
                        montré), constants, format ;
                        demand (lien classique à la demande : mode, services, baux, pause des
                        notifications, batterie gardée), dial (passer un appel), phoneLink (un lien
                        tel: à confirmer, #50), protocol (tous les noms du socket propres au mode
                        sobre, en un seul endroit)
    indicator.js        l'indicateur / la pastille dans la barre
    statusIcon.js       l'icône : les calques d'un dessin empilés, et l'horloge qui les fait bouger
    iconArt.js          les dessins (JavaScript pur) : deux styles, neuf états, tracés et mouvements
    cards.js            ce qui descend sous la pastille (menu et cartes)
    setup.js            le parcours d'accueil : une seule boîte, de l'autorisation à « Done »
    illustrations.js    remplit les dessins (nom d'hôte, code, interrupteurs, libellés) et les rend
    phoneLabels.js      les mots de l'iPhone, par langue (JavaScript pur), et le choix de la langue
    setupHelper.js      appels à l'assistant système `landline-setup`
    subprocess.js       lance un programme : attendu sans bloquer, terminé à l'annulation
    quiet.js            guette le Ne pas déranger de GNOME et le partage d'écran
    replyLinks.js       « Reply in app » : liens vers le client officiel (JavaScript pur)
    appIcons.js         les vraies icônes des applis de l'iPhone : App Store (Soup 3, asynchrone,
                        cache disque), sinon l'appli installée sur l'ordinateur (issue #48)
    replyLinks.js       l'expéditeur sûr d'une notification et le lien de sa conversation (JavaScript pur)
    officialApps.js     les applis officielles : catalogue, choix de l'appli ou du client web, médias,
                        recherche en cache (JavaScript pur, le Shell donne le moteur de recherche) (#49)
    telLinks.js         liens tel: : numéro normalisé, fichier desktop, interface D-Bus (JavaScript pur) (#50)
    phoneLinks.js       écrit/retire le fichier desktop et les défauts tel:/callto: (Phone Settings ; le Shell
                        seulement pour la proposition du premier lancement, quand aucune appli ne les a)
    hotspot.js          la connexion Internet de l'iPhone, par NetworkManager (libnm) : état,
                        activation, et raison d'un échec
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
                              ▲                        │
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
qu'il faut montrer : `icon` (le dessin de la barre : `state`, `moving`, `dim`), `pill` (ton, textes),
`card` (menu, réponse, appel…) et `tour` (le parcours d'accueil). Il ne pose aucune
minuterie : `nextDeadline()` dit quand rappeler `expire()`, et `extension.js` tient une seule minuterie.

Règles principales :

| Situation | Ce que dit `view()` |
|---|---|
| relié (`classic_connected` ou `le_connected` ; en mode sobre, `le_connected` seul) | l'icône seule (`linked`), sans point ni compteur ; avec des non-lus, l'icône à bulle (`unread`) et le nombre dans le menu (voir « L'icône de la barre ») |
| relié, tout va bien | l'icône nue (`linked`) ; le menu ne dit **rien** du lien : nom de l'iPhone, niveau et pile couchée (voir « Ce que dit l'icône, ce que dit le menu (#51) ») |
| relié, quelque chose à faire (réglage de l'ordinateur, messages indisponibles, interrupteur coupé ou notifications non partagées sur l'iPhone, moteur plus ancien) | la marque ambre (`attention`) ; une ligne dans le menu, l'action juste dessous |
| relié, notifications en pause (`le_state:"paused"`) | les deux barres ambre (`paused`) ; dans le menu « Notifications paused », le geste et où le faire sur l'iPhone (voir « Lien classique à la demande ») |
| appairé mais lien coupé | le même dessin grisé (`away`) ; une ligne du menu dit pourquoi d'après `bt_presence` (« Out of range », « Bluetooth is off on the iPhone »…) |
| iPhone vu sans lien utile, lien revenu pas encore servi, liens qui échouent et nouvel essai, sortie de veille | l'icône de reconnexion, animée (`reconnecting`) ; une ligne du menu dit la raison (voir « Reconnexion ») |
| les liens montent et retombent plus de 2 fois en 30 s | « Link again », tenu jusqu'au clic ou à 60 s de lien stable (voir « Indicateur stable ») |
| Ne pas déranger, écran partagé, sourdine | rien ne grossit, rien ne bouge : icône de repos, sans bulle ni flèche (voir « Silence ») |
| réglages système exigés manquants et pas de lien | « Permission needed » ; le clic ouvre le parcours à l'autorisation |
| Bluetooth de l'ordinateur éteint | glyphe atténué + « Bluetooth is off » |
| iPhone choisi mais plus appairé | « Link again » ; le clic ouvre le parcours à la recherche |
| appairage présent mais inutilisable (`needs_repair`) | « Link again », jamais « relié » ; le clic ouvre le parcours à la réparation |
| la carte devient `direct` (« Link again » qui ouvre un dialogue) alors que le menu est ouvert | l'indicateur ferme le menu sans animation (`_syncCard()`, `_rebuild()`) ; `buildCard()` ne construit le menu au repos que pour `kind:'menu'` et rien pour une carte inconnue (#52) |
| aucun iPhone choisi | « Link iPhone » ; le clic ouvre le parcours |
| parcours mis de côté (Échap) | « Continue setup » (« Confirm link » si un code attend) ; le clic le rouvre où il en était |
| relié, contrôle d'appel coupé dans le moteur | `bt_set_calls enabled:true` envoyé une fois (sauf réglage contraire) |
| socket du moteur muet, démarrage tenté | « Engine not running », carte avec « Try Again » |
| `bt_message` non lu, entrant, de moins de 10 min | pastille 7 s ; ignorée, elle devient un non-lu |
| carte ouverte sur la pastille | carte de réponse ; `bt_mark_read` part pour ce fil |
| réponse envoyée | « Connecting to send… » si le lien classique est fermé, « Sending… », puis « Sent » 2 s au `bt_send_result`, ou « Not sent » (le texte est gardé) |
| `bt_incoming_call` (`state:"ringing"`) | la carte d'appel dès la notification : Decline et Answer on iPhone tout de suite, Answer here quand le mains-libres est prêt |
| « Call… », bouton d'appel d'une conversation, ligne des appels récents | carte de recherche, puis l'attente (`bt_call_progress`) annulable, puis la carte d'appel |
| `bt_calls` avec un appel `incoming` | « <Prénom> is calling » + carte Decline / Answer |
| `bt_calls` avec un appel `incoming`, l'ordinateur pouvant porter le son | carte Answer here / Answer on iPhone / Decline |
| appel `active` | pastille verte « <Prénom> · mm:ss » ; carte iPhone / This computer / Hang Up |
| `bt_battery_low` (20 %, 10 %) | pas de bulle : l'icône (`battery`) jusqu'à l'ouverture du menu ; dans le menu, le niveau et la pile couchée en rouge tant que `battery_state` vaut `low` ou `critical` (plus de phrase) |
| batterie passée sous 15 %, moteur sans `battery_state` | la même chose : icône `battery`, plus de pastille ; rouge dans le menu à 20 % ou moins |
| `bt_pair_confirm_request` hors assistant | « Confirm link » + carte avec le code |
| relié, messages et contacts non proposés (voir « Messages et contacts non proposés ») | marque `attention` ; une ligne dans le menu + « Fix It » |
| un iPhone choisi ou appairé | « Forget This iPhone… » dans Phone Settings (plus dans le menu : un clic par accident y était possible) |
| relié, messages lisibles ou « à la demande » | « New Message… » en bouton sous l'en-tête du menu |
| relié, contrôle d'appel actif (`calls_available` autre que `off` ; à défaut `calls` non nul) | « Call… » en bouton sous l'en-tête du menu, à côté de « New Message… » ; un bouton d'appel sur chaque conversation ; les appels récents activables |

Détails qui comptent :

- L'historique rejoué à l'ouverture d'une session (messages lus, envoyés ou anciens) ne fait rien surgir ;
  un même message (`handle`) n'est compté qu'une fois.
- Le nom de l'appelant manque dans `bt_calls` diffusé : le modèle demande `bt_list_calls` une fois par
  appel, et à défaut cherche le numéro dans les conversations.
- `bt_call_result` arrive deux fois (réponse et diffusion) : sans effet.
- Notifications : celles de Messages et du téléphone restent à la pastille, **sauf** si la pastille ne peut
  pas les montrer (messages illisibles, contrôle d'appel coupé) et sauf l'appel manqué, qui n'a pas d'autre
  trace. Tout le reste passe par la pastille et le menu, **jamais par les notifications de GNOME** (voir
  « Notifications des autres applis : un seul système »).

### Indicateur stable

Défaut vu en réel le 16 novembre 2026, appairage cassé d'un côté : les liens montaient et retombaient
toutes les quelques secondes, et l'indicateur suivait — relié, hors de portée, allumé, éteint, étiré.
Trois règles (`_settle()`, `_watchLinks()` dans `model.js`) :

- **Délai.** Un nouvel état de repos n'est montré que s'il a tenu 3 s (`LINK_SETTLE_MS`). Exceptions :
  « relié » confirmé (un lien monté **et** un service actif : `map_open`, `pbap_open` ou `ancs_ready`)
  se montre tout de suite ; un message et un appel entrants ne sont pas des états et n'attendent jamais ;
  le premier état après « rien », s'il n'est ni « relié » ni « hors de portée », est dit tout de suite.
- **Largeur constante au repos.** L'indicateur au repos n'est que l'icône, dans une boîte de 16 px :
  même largeur relié, hors de portée, au démarrage, avec des non-lus, un appel manqué, un message vocal
  ou des notifications en pause. Les icônes voisines ne bougent jamais.
- **Verrou « Link again ».** Si les liens tombent plus de 2 fois en 30 s (`FLAP_LIMIT`,
  `FLAP_WINDOW_MS`), ou si le moteur dit `needs_repair`, l'indicateur se fixe sur « Link again », quels
  que soient les liens ensuite. Il en sort par une action de l'utilisateur (le clic, qui ouvre alors le
  parcours **à la réparation** même si les liens sont montés à cet instant ; l'oubli) ou après 60 s de
  lien monté sans chute ni `needs_repair` (`STEADY_MS`). La disparition du moteur n'est pas une chute.

`view().link` est l'état montré ; `view().unsteady` vaut `flapping` ou `repair` pendant le verrou. Les
tests des autres règles créent le modèle avec `steady: false` (tout état se montre aussitôt) ; le
pilote du Shell d'essai fait de même hors de la partie « stable ».

### Reconnexion

Constat du 18 novembre (issue #30) : au retour à la maison, l'iPhone a essayé de se relier plusieurs fois
pendant trois minutes, et la barre ne disait que « absent », sans rien qui montre qu'une négociation était
en cours ni pourquoi elle durait. Un état `reconnecting` se place entre `away` et `linked`. Il est décidé
par `reconnectReason()` (`lib/model/reconnect.js`, fonction pure, testée seule), à partir de `_link()`, de
`bt_presence`, de `le_state` / `le_cause` et de ce que le modèle a vu passer (`_reconnectTrack()`) :

| Raison | Quand | Phrase du menu (sous « Reconnecting… ») |
|---|---|---|
| `setting_up` | un lien utile est remonté **sur le chemin du retour** (indicateur sur absent ou reconnexion, hors parcours d'accueil), l'iPhone ne l'a pas encore servi (`ancs_ready`, `map_open`, `pbap_open` faux) ; 15 s au plus (`RECONNECT_SETUP_MS`), ensuite « relié » quand même (iPhone qui ne partage rien) | « Almost there, getting notifications ready… » |
| `retrying` | dit par le moteur : `le_state:"waiting"` avec `le_cause:"unsteady"` (des liens sont morts avant d'avoir servi, l'iPhone est rappelé) et un signe de l'iPhone depuis moins d'une minute ; ou `holding_off` (moteur d'avant #29, ou retraits rétablis par l'environnement) | « The link keeps dropping, trying again… » ; avec `holding_off` seulement, le délai estimé d'après `le_strikes` (15 s, 30 s, puis le double, 120 s au plus) : « …trying again in 30 s », arrondi à 5 s, recalculé toutes les 5 s menu ouvert. Le moteur actuel ne publie aucun délai : rien n'est promis |
| `dropping` | vu ici, sans que le moteur le dise : deux liens morts avant d'avoir servi en une minute, ou un seul avec l'iPhone encore vu | « The link keeps dropping, trying again… » |
| `back` | `bt_presence` `here` et aucun lien utile | « Your iPhone is back, setting up the link… » |
| `sleep` | `le_state:"waiting"` / `le_cause:"after_resume"` ; moteur sans `le_state` : `bt_presence` `computer_suspended` reçu il y a moins de 2 min | « Reconnecting after sleep… » |

Jamais : notifications en pause dans le moteur (`bt_status` `enabled:false`, le menu dit « Paused » et
l'icône ne bouge pas, #42) ; `bt_presence` `switched_off` (Bluetooth coupé sur l'iPhone) ou `bluetooth_off` ; `le_state:"paused"`
(le moteur a attendu 180 s : le bloc « Notifications paused » dit pourquoi) ; hors de portée sans signe
de l'iPhone (vu, lien monté, fin d'un retrait) depuis une minute (`RECONNECT_HOLD_MS`) ; un seul lien mort
au bord de la portée (c'est quelqu'un qui s'en va). Cinq minutes au plus d'affilée (`RECONNECT_MAX_MS`) :
ensuite l'état d'avant (absent, ou « Link again ») et sa raison (« Not linked » / « The iPhone is within
reach, but the link is not up. »), jusqu'à un lien confirmé ou 5 s sans aucune raison
(`RECONNECT_CALM_MS`). Les cinq minutes sont comptées d'un bloc : une raison qui disparaît une passe ou
quelques secondes ne les remet pas à zéro, et ne fait pas redire « Reconnecting… » une fois le plafond
atteint (#42).

Calme de l'indicateur : `reconnecting` passe par `_settle()` comme les autres états (3 s) ; un lien servi
le remplace tout de suite. **Depuis « relié », une coupure doit durer 10 s** (`RECONNECT_AFTER_LINK_MS`)
avant d'être dite reconnexion, et un lien remonté sur un indicateur encore « relié » n'est jamais
`setting_up` : la micro-coupure d'un lien servi ne fait rien bouger. Le verrou « Link again » l'emporte
sur les raisons devinées (`back`, `setting_up`, `dropping`) mais pas sur ce que dit le moteur (`retrying`,
`sleep`) : un appairage cassé d'un côté avec un moteur ancien reste « Link again ». Raisons hors de
`away` / `linked` (Bluetooth de l'ordinateur, réparation, parcours…) : jamais.

Le libellé lu est « Reconnecting to the iPhone » ; le dernier niveau de batterie connu reste, marqué
« ~ ». Avec `bt_presence` sans lien et sans reconnexion, l'en-tête dit maintenant « Not linked » (au lieu
de « Reconnecting… », qui mentait quand rien n'était en cours) et la veille finie sans retour « Not back
since this computer slept ».

### L'icône de la barre

Le point (et le compteur) à droite du glyphe ont disparu : on le voyait vert ou jaune selon la couleur
d'accent, et il ne disait rien. L'état se lit sur l'icône. Planche validée : deux styles, réglage « Icon »
de Phone Settings (`icon-style` : `phone` par défaut, `handset`).

| État (`view().icon.state`) | Quand | A · Phone | B · Handset | Mouvement |
|---|---|---|---|---|
| `linked` | relié, rien n'attend | le téléphone seul | combiné + point ambre | — |
| `away` | lien coupé | le même, à 45 % | combiné et anneau à 45 % | — |
| `reconnecting` | reconnexion en cours (voir « Reconnexion ») | le téléphone à 70 %, décalé de 2 vers la gauche ; deux ondes « ) ) » qui sortent de son flanc droit | combiné à 70 % ; à la place du point de « relié », un anneau ouvert (trois quarts) | les ondes s'allument l'une après l'autre (la seconde 260 ms plus tard) puis retombent à 25 %, en boucle de 1,8 s ; l'anneau tourne, un tour en 2,4 s ; tant que dure la reconnexion (5 min au plus), immobile en silence. Animations coupées : les deux ondes à 75 %, l'anneau ouvert fixe |
| `unread` | non-lus (et en tête de la pastille d'un message) | + bulle ambre en haut à droite | + bulle ambre | la bulle rebondit toutes les 3 s, **une minute** après l'arrivée du dernier message, puis reste immobile tant qu'il y a des non-lus |
| `ringing` | appel entrant (`bt_incoming_call`, ou `bt_calls` `incoming`) | le téléphone vibre, deux traits de chaque côté | le combiné se balance, deux ondes | tant que ça sonne |
| `call` | appel en cours ; composition en cours (même dessin) | + point vert et onde | combiné vert, point et onde | onde lente (2,2 s), tout l'appel |
| `missed` | notification d'appel manqué (catégorie ANCS 2) nouvelle | + flèche rouge | + flèche rouge | — ; jusqu'à l'ouverture du menu, ou son retrait sur l'iPhone |
| `voicemail` | notification de messagerie vocale (catégorie 3) nouvelle | téléphone décalé + deux bobines ambre | les bobines seules | les bobines tournent une minute, puis restent ; jusqu'à l'ouverture du menu, ou son retrait sur l'iPhone |
| `paused` | `le_state:"paused"` ; aussi la pause voulue dans le moteur (`enabled:false`), lien absent | le téléphone décalé de 2 vers la gauche + deux barres ambre « ‖ » en haut à droite | + deux barres ambre à la place du point | les barres clignotent une minute après le début de la pause, puis restent |
| `attention` | relié, quelque chose à faire (`_attention()`, voir plus bas) | le téléphone décalé de 2,6 vers la gauche + disque ambre (r 3,2) d'où est découpé un « ! » | + le même disque à la place du point de « relié » (plus grand que lui, et marqué) | — |
| `internet` | « Internet from iPhone » en ligne ou en cours de connexion (voir « Internet from iPhone ») ; aussi quand l'autre lien est tombé mais que la connexion tient | le téléphone décalé de 3,4 vers la gauche, deux flèches ↑↓ hautes comme son écran, couleur du glyphe | à la place du point ambre, deux petites flèches ↑↓ ambre | en connexion : les flèches s'allument l'une après l'autre (mouvement des ondes de `reconnecting`, la seconde 450 ms plus tard) ; en ligne : immobile |
| `internet_failed` | la connexion a échoué ou est tombée, jusqu'à l'ouverture du menu | les mêmes flèches, rouges | les mêmes, rouges | — |
| `bare` | états à libellé (« Link iPhone », « Link again », « Permission needed », « Continue setup », « Engine not running », « Sending… ») ; grisé (`dim`) pour « Bluetooth is off » et au démarrage | le téléphone seul | le combiné seul, sans le point de « relié » | — |

Priorité au repos (`_iconAtRest()`, `lib/model/icon.js`), une seule marque à la fois, la première qui
s'applique : en ligne par l'iPhone alors que le lien est absent (`internet`) ; lien absent ou qui revient
(`away`, `reconnecting` ; `paused` pour la pause voulue) ; puis, relié et hors silence, appel manqué,
message vocal, non-lu, batterie faible pas encore vue ; puis notifications en pause (`paused`) ; puis
quelque chose à faire (`attention`) ; puis Internet (`internet`, `internet_failed`) ; sinon `linked`.
« Sent » montre une coche à la place de l'icône, deux secondes. Planche des deux styles, sombre et clair,
à 16 px réels et en grand : `tools/planche-icones.sh DOSSIER`.

### Ce que dit l'icône, ce que dit le menu (#51)

Tout ce qui dit que le lien tient, ou qu'il ne tient pas bien, passe par l'icône seule. Le menu au repos
ne dit rien du lien quand tout va bien : plus de « Linked » à côté du nom, plus d'opérateur, plus de
« Messages and calls connect when needed. » (mode sobre), plus de raison du mode en vigueur (« Everything
stays connected… » : Phone Settings la montre), plus de « Notifications are reconnecting… » (transitoire ;
la pause vient si cela dure), plus de ligne de tolérance du lien (« Steady link: rides out 6 s gaps ») ni
de « Keep the Link Steady… », plus de « Limited » ni de phrase de batterie faible.

Quand quelque chose ne va pas, une ligne courte sous l'en-tête (`card.notices`), et juste dessous la
seule action utile :

| Marque | Situation | Ligne du menu | Action |
|---|---|---|---|
| `away` | lien coupé | « Out of range », « Bluetooth is off on the iPhone », « Nearby, not linked », « Not back since this computer slept », avec « · this computer was locked » / « locks shortly » | — |
| `reconnecting` | reconnexion en cours | « Your iPhone is back, setting up the link… », « The link keeps dropping, trying again… »… | — |
| `paused` | pause voulue (`enabled:false`) | « Paused » | — |
| `paused` | notifications en pause | bloc « Notifications paused » + le geste et où le faire sur l'iPhone (la cause n'est plus que lue par les lecteurs d'écran) | — |
| `attention` (1) | réglage de l'ordinateur manquant ou non permanent | « Some features are off until a Bluetooth setting of this computer is changed. » / « …will not survive a restart yet. » | Finish Setting Up This Computer… |
| `attention` (2) | messages et contacts non proposés, ou lus par un autre ordinateur | phrase de la panne OBEX, ou « Another computer is reading the messages of this iPhone. » | Fix It (panne `down`/`failed`) |
| `attention` (3) | interrupteur de partage coupé sur l'iPhone, ou mode sobre impossible faute de notifications partagées (`link_mode_reason:"notifications_off"`) | « Some sharing switches are still off on the iPhone. » | Continue Setup… |
| `attention` (4) | moteur installé plus ancien | « A newer engine is ready to install. » | Update the Engine… |
| `battery` | batterie faible pas encore vue | — (le niveau et la pile couchée en rouge) | — |

Notifications en pause : la ligne d'`attention` attend que la pause soit finie (une seule chose à la
fois). Ne pas déranger et l'écran partagé ne sont plus dits dans le menu (décision du
19 novembre) : GNOME les montre lui-même.

La tolérance du lien LE (`le_params`, souhaitée, jamais exigée) n'est plus proposée que dans le parcours
(étape d'autorisation) et dans Phone Settings › This computer, ligne « Keep the link steady » visible
seulement si l'assistant de root est là et dit `le_params:false` ; son bouton « Set Up… » lève la clé
`steady-request`, et le Shell ouvre l'étape d'autorisation avec ce seul point (`startSteadyTour()`).

L'aperçu des icônes de Phone Settings montre, sous chaque style, une rangée d'événements (non-lu,
sonnerie, appel, appel manqué) et une rangée de marques (reconnexion, hors de portée, pause, attention,
batterie faible, Internet en échec), 32 px, avec leur nom en infobulle.

- **Technique** (`lib/statusIcon.js`, `lib/iconArt.js`). Un `St.Widget` de 16 px logiques (taille
  `icon-size` des autres icônes de la barre, donc à l'échelle de l'écran) empile un `St.Icon` par calque
  du dessin (glyphe, bulle, onde, chaque bobine…). Chaque calque est un SVG fabriqué en mémoire avec les
  tracés exacts de la planche et sa couleur (`Gio.BytesIcon`) : des icônes symboliques ne conviennent
  pas, le Shell y remplit tous les tracés et les traits de la planche deviendraient des aplats. Le
  mouvement est une transformation de l'acteur (translation, rotation et échelle autour d'un pivot,
  opacité) : aucune image n'est redessinée. Une seule `Clutter.Timeline`, attachée à l'icône, sert
  d'horloge : à chaque image de l'écran elle place les calques d'après les images-clés de la planche
  (`MOTIONS`, courbes d'accélération de CSS). Les `Clutter.KeyframeTransition` essayées d'abord
  laissaient un `GLib-GObject-CRITICAL` dans le journal au passage du ramasse-miettes pendant un appel.
- **Rien ne tourne quand rien ne bouge.** L'horloge n'existe que si l'état bouge (`icon.moving`), que
  l'icône est à l'écran et que `org.gnome.desktop.interface enable-animations` est vrai (suivi en
  direct) ; sinon le même dessin, immobile (les ondes, qui n'existent qu'en mouvement, sont masquées).
  La minute d'animation est tenue par le modèle (`_attentionAt`, `ATTENTION_MS`, `nextDeadline()`) :
  aucune minuterie propre à l'icône. Écran verrouillé : l'extension est désactivée, l'icône détruite.
  `disable()` et tout changement d'état arrêtent l'horloge avant de toucher aux calques.
- **Couleurs** fixes, dans la feuille de style, quelle que soit la couleur d'accent : `-landline-amber`
  `#f2b45c`, `-landline-green` `#6fd58e`, `-landline-red` `#ff8a7a` (propriétés de `.landline-icon`). Le
  glyphe prend la couleur du texte de la barre : blanc sur barre sombre, sombre sur barre claire. Sur la
  pastille ambre (message, sonnerie, états à libellé), les trois encres deviennent l'encre sombre de la
  pastille (ambre sur ambre ne se verrait pas) ; sur la pastille verte d'un appel elles restent.
- **Pastille.** L'icône est toujours le premier élément, à la même place : la pastille grossit à sa
  droite. Elle remplace les anciens pictogrammes de la pastille (téléphone, combiné d'appel, silhouette) ;
  les initiales de l'expéditeur restent. La pastille n'est rognée que pendant son glissement : au repos
  l'icône peut dépasser sa boîte d'un ou deux pixels (onde, bulle qui saute).
- **Répondeur.** Le moteur transmet la catégorie ANCS de chaque notification (`category`, 3 pour la
  messagerie vocale, `docs/moteur-api.md` §4) : l'état est déclenché par une notification **nouvelle**
  (ni `silent`, ni venue de la liste, ni déjà vue sous un autre numéro) de cette catégorie. Cette
  notification est aussi listée dans le menu (« On your iPhone »), comme celle de l'appel manqué : l'icône dit qu'il y en
  a un, pas de qui. Non vérifié avec un vrai iPhone (un message vocal d'opérateur sans messagerie
  visuelle n'arrive peut-être qu'en SMS).

### Silence : Ne pas déranger, écran partagé, sourdine

Quand rien ne doit surgir, l'extension ne montre **aucun contenu** : pas de pastille qui grossit, ni
prénom ni texte dans la barre, pas de carte d'appel qui descend, pas de bulle, pas d'alerte batterie.
L'indicateur garde son icône de repos, immobile (relié, absent, ou l'anneau des notifications en pause,
qui dit le lien et non ce qui arrive) : ni bulle, ni flèche d'appel manqué, ni bobines ; les non-lus
sont comptés dans le menu, qui, ouvert exprès, montre tout (y compris la carte d'un appel qui sonne, avec ses boutons), sans ligne d'explication
(GNOME montre Ne pas déranger lui-même). À la sortie, rien n'est rejoué.

| Cause | Source | Réglage (Phone Settings, « Quiet times ») |
|---|---|---|
| Ne pas déranger de GNOME | `org.gnome.desktop.notifications show-banners` à faux (`lib/quiet.js`) | `follow-do-not-disturb`, vrai par défaut |
| écran partagé ou enregistré | `global.backend.get_remote_access_controller()`, signal `new-handle`, poignées `is_recording` : la source de l'indicateur de partage du Shell lui-même | `quiet-while-sharing`, vrai par défaut |
| sourdine | « Mute iPhone on this computer », interrupteur du menu | `muted`, faux par défaut ; silence complet, appels compris |
| écran verrouillé | le Shell désactive l'extension (elle ne déclare pas `unlock-dialog`) : rien n'existe sur l'écran de verrouillage | — |

Appels : en Ne pas déranger, rien non plus, sauf `calls-through-do-not-disturb` (faux par défaut), qui
ne vaut que pour le Ne pas déranger — jamais pendant un partage d'écran ni en sourdine.

Les notifications des autres applis reçues pendant un silence ne font pas grossir la pastille : elles
sont seulement listées dans le menu. Un message arrivé
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

Ces règles décident aujourd'hui de ce qui fait **grossir la pastille** (plus aucune notification GNOME
n'est créée) ; les anciennes sont listées dans le menu, section « On your iPhone ».

### Notifications des autres applis : un seul système (branche `notifs`, prototype)

Tout ce qui vient du téléphone passe par la pastille et le menu ; `lib/notifications.js` (MessageTray) a
disparu. Le modèle (`lib/model/notifications.js`) tient **en mémoire seulement** ce qui attend sur
l'iPhone (`_phoneNotes`, par `uid`, dédoublonné par empreinte) ; rien n'est écrit sur le disque.

| Sujet | Règle |
|---|---|
| Notification reçue en direct, nouvelle | pastille 6 s (`NOTE_PILL_MS`) : icône de l'appli (sinon ses initiales, voir plus bas), nom de l'appli, « titre: premiers mots » ; mode `note` |
| Plusieurs en moins de 5 s (`NOTE_BURST_MS`) | une seule pastille « 3 notifications » + les applis ; le clic ouvre le menu |
| Clic sur la pastille d'une seule | carte `note` : appli, heure, titre, texte, « Dismiss » (efface aussi sur l'iPhone : `bt_notification_action` `negative`, si l'iPhone le permet) et « Open in Signal » / « Open in WhatsApp Web »… vers l'appli officielle de l'ordinateur (voir « Passer le relais aux applis officielles ») |
| Priorité | appel > message > notification d'appli : rien ne grossit pendant un appel, une sonnerie, un appairage, une carte ouverte, un envoi, ni par-dessus la pastille d'un message ; un message remplace la pastille d'une notification |
| Applis en sourdine | réglage `quiet-apps` (Phone Settings, « Notifications of other apps » : noms ou identifiants, sans casse) : jamais de pastille, seulement le menu, et pas comptées dans le résumé |
| Silence (Ne pas déranger, partage, sourdine) | rien ne grossit, rien n'est rejoué ensuite |
| Appel manqué, messagerie vocale | icône (`missed`, `voicemail`) + ligne dans « On your iPhone » et dans les appels récents ; pas de pastille |
| Batterie faible | icône `battery` (dessin ajouté aux deux styles) jusqu'à l'ouverture du menu + ligne du menu ; réglage `battery-alerts` |
| « On your iPhone » | section du menu : ce qui attend sur l'iPhone, groupé par appli, la plus récente en haut ; 6 lignes puis « Show all (N) » ; la liste défile au-delà d'une hauteur ; une ligne ouvre sa carte, sa croix l'efface (aussi sur l'iPhone), « Clear All » efface tout ce qui peut l'être. Nourrie par les notifications `silent`, la liste `bt_notifications` et le direct ; `bt_notification_removed` retire ; le lien LE qui tombe vide la liste (le moteur renvoie tout au retour). Absente si vide |
| Résumé au retour | les notifications `silent` (ou de la liste) **jamais vues** sont comptées ; 2,5 s après la dernière (`AWAY_SETTLE_MS`), une pastille 7 s « While you were away: 5 WhatsApp, 2 Mail and 3 more » (trois applis au plus), mode `summary` ; clic → menu. Une seule fois : un lien qui retombe et remonte avec les mêmes ne redit rien. Rien en silence ni par-dessus autre chose |
| « Earlier » | réglage `keep-history`, faux par défaut. Actif : ce qui a quitté l'iPhone reste listé dans un sous-menu « Earlier » (12 h, 200 entrées, `_earlier`, mémoire du Shell seulement) ; vidé par « Clear history », à l'extinction du réglage et à `disable()` (donc au verrouillage) |

`onNotification` n'est plus écouté par l'extension : `reply` (expéditeur sûr), `show`, `remove`, `reset`
restent émis pour les tests. Le menu de repos se reconstruit à chaque changement de la
liste : un sous-menu ouvert se referme alors (à reprendre).

### Icônes des applis de l'iPhone (issue #48)

Chaque notification ANCS porte l'identifiant de son appli (`app_id`, ex. `net.whatsapp.WhatsApp`) : il
arrive tel quel du moteur, rien n'a changé côté moteur. Le modèle le passe partout où une appli est
montrée : `pill.apps` (une notification : son appli ; plusieurs : les trois premières applis, une fois
chacune ; le résumé au retour : les trois applis qu'il nomme, dans son ordre), `appId` des cartes `note`,
des groupes et des lignes de « On your iPhone », et des entrées de « Earlier » (qui portent aussi
`initials`). Le modèle ne sait rien des icônes elles-mêmes.

`lib/appIcons.js` (`AppIcons`, sans Shell, testé seul) répond à `icon(appId)` tout de suite, sans
attendre :

| Ordre | Ce qui est montré |
|---|---|
| 1 | `{file}` : l'icône de l'App Store gardée dans `~/.cache/landline/app-icons/<identifiant>.png` (128 px, PNG), si le réglage `show-app-icons` est vrai |
| 2 | `{gicon}` : l'icône de la même appli installée sur l'ordinateur (`DESKTOP_APPS` : WhatsApp, Signal `signal-desktop`, Telegram `org.telegram.desktop`, Teams `com.microsoft.Teams`, Discord `com.discordapp.Discord`, Slack, Messenger/Caprine, Mail → Evolution/Thunderbird/Geary, Calendar → GNOME Agenda ; noms paquet, Flatpak et Snap), trouvée par `Shell.AppSystem.lookup_app()` (en mémoire, sans IO), réglage éteint ou non |
| 3 | `null` : les initiales dans un disque, comme avant ; rien dans la pastille d'une rafale |

Une icône pas encore connue est cherchée **une fois** (l'entrée `busy` dédoublonne les demandes en
vol) : `query_info_async` du PNG du cache ; sinon d'un témoin `<identifiant>.none` de moins d'un jour ;
sinon `GET https://itunes.apple.com/lookup?bundleId=<id>&country=<cc>` (Soup 3,
`send_and_read_async`, délai 20 s), `results[0].bundleId` doit être l'identifiant demandé,
`artworkUrl512` est réécrit en `…/128x128bb.png` (le serveur d'images d'Apple dessine la taille et le
format nommés), accepté seulement en HTTPS sur `*.mzstatic.com`, puis l'image (type `image/png`,
signature PNG, 512 Kio au plus) est écrite par `replace_contents_bytes_async` (dossier créé par
`make_directory_async`). Les deux requêtes (#55) : `Soup.MessageFlags.NO_REDIRECT` (une redirection
n'est jamais suivie : 3xx = échec définitif) et contrôle que la réponse vient du même schéma, hôte et
port que l'adresse vérifiée ; `send_async` puis lecture par morceaux de 64 Kio : un `Content-Length` au-delà
du plafond est refusé avant toute lecture, un corps sans longueur est coupé dès qu'il le dépasse
(`MAX_BYTES` = 512 Kio pour l'image, `MAX_LOOKUP_BYTES` = 1 Mio pour le JSON de la recherche). Le
repli des applis du bureau est retenu en mémoire (`_desktop`) et vidé par `appsChanged()` quand
`Gio.AppInfoMonitor` signale un changement (après le même délai de 1,5 s que les applis officielles) :
une appli installée ensuite prête son icône sans redémarrer le Shell. `onChange()` prévient l'indicateur, qui regroupe les arrivées dans un idle,
refait la pastille et remplace sur place l'icône des lignes du menu ouvert (`watchAppIcons`) : le menu
n'est pas reconstruit.

Échecs : une appli inconnue de l'App Store, une réponse qui n'est pas du JSON ou pas une image →
témoin `.none` sur le disque, pas redemandée avant 24 h (`RETRY_MS`), même après un redémarrage du
Shell ; pas de réseau, erreur 5xx ou 429 → retenue en mémoire seulement, 24 h aussi (le verrouillage de
l'écran, qui désactive l'extension, la fait oublier). Pays : région de la locale (`LC_MONETARY`, puis
`LC_TIME`, puis la langue : `fr_CH` → `ch`), `us` à défaut.

Réglage **`show-app-icons`** (vrai par défaut — **à confirmer**), Phone Settings › « Notifications of
other apps » › « Show app icons » : la phrase dit que l'identifiant de l'appli est envoyé à l'App Store
d'Apple, une fois par appli, et rien de la notification. Éteint : ce qui est en route est annulé, plus
aucune requête, les icônes du cache ne sont plus montrées (celles des applis du bureau, oui).

Affichage (`appIconActor()` dans `cards.js`) : l'icône de l'App Store est le fond (`background-image`,
`background-size: cover`) d'un `St.Bin` arrondi en CSS (≈ 22 % du côté, comme sur l'iPhone ; aucun
traitement d'image) ; celle du bureau, un `St.Icon` qui garde sa forme. Tailles : celles des initiales
remplacées (`landline-app-icon-pill` 1,25 em, `-small` 1,75 em, `-large` 2,15 em). En style clair, un
filet de 1 px garde le bord des icônes blanches (Calendar, Teams) dans le menu. Les icônes de la
pastille s'ajoutent au texte (`landline-with-apps` : 23 em au lieu de 19, 36 au lieu de 32 pour le
résumé).

`disable()` : `AppIcons.destroy()` annule le `Gio.Cancellable`, `abort()` la `Soup.Session` et la
lâche. Aucune icône de marque n'est livrée dans le zip : tout vient de l'App Store ou de l'ordinateur.

Essai : `tests/appIcons.test.js` (serveur `Soup.Server` local qui imite la recherche et le serveur
d'images ; cache, dédoublonnage, appli inconnue, image refusée, panne, nouvelle tentative un jour plus
tard, réglage éteint = aucune requête, repli du bureau, destruction en route ; redirection vers un autre
nom d'hôte jamais suivie, image trop grande avec ou sans `Content-Length`, recherche démesurée, appli du
bureau installée après coup). Dans le Shell isolé :
`FAUX_APPSTORE=<dossier de PNG hors dépôt> SHOTS=<dossier> INNER=tools/shell-test/proto-icones.sh
tools/shell-test/run.sh` (le pilote double `AppIcons._download` : l'App Store n'est jamais joint ; sans
`FAUX_APPSTORE`, aucune icône, comme sans réseau).

**Phone Settings, « Icon ».** Deux cartes cliquables côte à côte à la place de la liste déroulante : le
dessin en grand, le nom, et une rangée des états principaux (relié, non lu, sonnerie, appel, appel
manqué). Images fabriquées en mémoire par `iconArt.stateSvg()` à partir des tracés de la barre (une seule
source), sur une tuile sombre qui figure la barre ; aperçu statique.

**Essai court** : `INNER=$PWD/tools/shell-test/proto-notifs.sh tools/shell-test/run.sh` (un démarrage,
captures `n1-*` à `n5-*`). Faux moteur : `pile [N]` (notifications accumulées pendant l'absence,
renvoyées par `reconnexion`), `notification-appli mail`. **Le déroulé complet `inner.sh` n'a pas été
rejoué** : ses vérifications sur la liste de GNOME (`trayTitles`, `bannerTexts`, `dismiss`) sont à
réécrire.

### Passer le relais aux applis officielles (#49)

Le Bluetooth ne porte que le texte d'une notification : un vocal WhatsApp arrive en « 🎤 Voice message »,
une photo en « 📷 Photo ». L'appli officielle de bureau du service, si elle est installée, est elle-même
un appareil lié du compte et les a. La carte `note` propose de l'ouvrir ; **jamais un client non
officiel** (risque de bannissement du compte).

| Sujet | Règle |
|---|---|
| Catalogue | `lib/officialApps.js`, `OFFICIAL_APPS` : par service, les identifiants iOS, les fichiers desktop de l'appli officielle (paquet, Flatpak, Snap), le schéma de lien qu'elle ouvre, le client web officiel, la page de téléchargement officielle. WhatsApp (`net.whatsapp.WhatsApp`, `…WhatsAppSMB`) : pas d'appli Linux officielle → WhatsApp Web dans le navigateur ; aucune enveloppe tierce (ZapZap, WasIstLos…) n'est cherchée ni proposée, même installée (#54). Signal (`signal-desktop`, `org.signal.Signal`), Telegram (`org.telegram.desktop`, Telegram Web), Teams (`com.microsoft.Teams`, sinon teams.microsoft.com ; plus d'appli Linux à suggérer), Discord, Slack, Messenger (messenger.com seulement) |
| Détection | `AppFinder` : une recherche à `enable()`, puis 1,5 s après chaque `changed` de `Gio.AppInfoMonitor` (appli installée ou retirée, autre appli par défaut). Applis : `Shell.AppSystem.lookup_app()` (liste tenue en mémoire par le Shell) ; gestionnaires : `Gio.AppInfo.get_default_for_uri_scheme_async()` pour `sgnl`, `tg`, `msteams`, `discord`, `slack` et `https`. Rien n'est lu à l'arrivée d'une notification : la carte lit le cache (`appRoute()`), et se met à jour quand la recherche change quelque chose (`officialAppsChanged()`) |
| Pas de client non officiel | un gestionnaire de schéma ne compte que s'il est l'un des fichiers desktop du catalogue (un client tiers qui ouvre `tg:` n'est jamais proposé : Telegram Web à la place) |
| Choix | appli officielle installée, sinon client web officiel, sinon rien (« manquante ») |
| Bouton | « Open in Signal », « Open in WhatsApp Web », « Open in Telegram Web », « Open Discord in the Browser »… ; libellé long → le bouton prend toute la largeur au-dessus de « Dismiss ». **Sur la conversation** quand c'est sûr (règles de `replyLinks.js` inchangées : expéditeur seul, numéro complet dans le titre ou seul numéro du seul contact de ce nom) et que l'appli choisie ouvre ces liens : `sgnl://signal.me/#p/+…`, `tg://resolve?phone=…`, `https://web.whatsapp.com/send?phone=…` ; sinon l'appli (`Shell.App.activate()`) ou la page d'accueil du client web. Ouvrir n'efface rien sur l'iPhone |
| Média | `mediaKind()` reconnaît, en anglais et en français, l'émoji en tête (🎤 📷 🎥 📄…), le mot seul éventuellement suivi d'une durée (« Voice message (0:12) », « Message vocal », « Photo », « Vidéo », « Pièce jointe », « Sticker », « GIF »), dans un groupe après « Nom: », et les phrases « sent you a photo » / « a envoyé une photo ». Une phrase qui commence par « Photo » reste du texte. La carte ajoute une ligne en italique : « Voice message — play it in Signal », « Photo — see it in WhatsApp Web », ou « … on the iPhone » quand rien ici ne l'ouvre |
| Appli manquante | à l'ouverture de la carte (`openNote()` ou clic sur la pastille), si le service a une appli officielle pour Linux qui n'est pas installée : une ligne sobre « Telegram Desktop isn’t on this computer. The official app shows voice messages and media here. » et deux petits boutons « Get Telegram Desktop » (page officielle, dans le navigateur) et « Don’t Suggest Apps ». **Une seule fois par service, pour de bon** : le service est noté aussitôt dans `suggested-apps` (par `onSetting`). « Don’t Suggest Apps » éteint `suggest-apps` ; Phone Settings › « Suggest official apps » le rallume et vide la liste |
| Ne pas doubler | `skip-running-apps` (Phone Settings › « Leave notifications to apps open on this computer », **éteint par défaut**) : la notification d'un service dont l'appli de bureau est ouverte (`Shell.AppState.RUNNING`) ne fait pas grossir la pastille ; elle reste dans « On your iPhone ». Jamais pour un client web (un onglet du navigateur ne se voit pas) |

Limites : les notifications déjà là au retour de l'iPhone (liste, `silent`) n'ouvrent pas la conversation
(pas de recherche de contact pour elles), seulement l'appli ; un identifiant de fichier desktop absent du
catalogue (autre empaquetage) n'est pas reconnu.

### Liens tel: (#50)

Phone Settings › Calls › « Use Landline for phone links » (`lib/phoneLinks.js` ; éteint par défaut).

- **Proposé une fois au premier lancement** (décision du 19 novembre) : seulement si l'option est éteinte,
  jamais proposée (clé `phone-links-offered`) et **qu'aucune appli n'ouvre `tel:` ni `callto:`**
  (`phoneLinksState()` : ni défaut ni gestionnaire, à `enable()`, quand l'une des deux clés change et
  1,5 s après chaque `changed` de `Gio.AppInfoMonitor`). Le modèle la reçoit par
  `setPhoneLinksOffer()`. Discrète : une ligne du menu au repos, sous « Call… / New Message… », « Call
  the phone numbers you click in a browser or another app through your iPhone? » et deux petits boutons
  « Use Landline » / « Not Now ». Montrée seulement quand un appel peut être passé d'ici (`_canDial()`),
  la première fois que le menu s'ouvre ainsi : notée aussitôt (`onSetting('phone-links-offered', true)`,
  comme la suggestion d'appli de #49), elle reste tant que ce menu est ouvert, puis plus jamais, répondue
  ou non. « Use Landline » : le Shell revérifie qu'aucune appli ne les a pris entre-temps, puis
  `usePhoneLinks()` ; sinon, ou en cas d'échec, Phone Settings s'ouvre. Une autre appli les a : rien
  n'est demandé, l'option reste dans Phone Settings.

- **Allumé** : écrit `~/.local/share/applications/landline-phone-links.desktop` (`NoDisplay=true`,
  `MimeType=x-scheme-handler/tel;x-scheme-handler/callto;`, `Exec=gdbus call --session --dest org.gnome.Shell
  --object-path /io/github/d101c/Landline --method io.github.d101c.Landline.PhoneLinks.Call %u`), le rend
  défaut des deux schémas (`set_as_default_for_type`) et note les défauts d'avant dans
  `phone-links-previous` (`tel=org.gnome.Shell.Extensions.GSConnect.desktop`). **Si une autre appli est
  le défaut** (par exemple GSConnect pour `tel:`, Zoom pour `callto:`), la ligne le dit (« Phone links
  open in GSConnect, Zoom now. ») et l'allumer demande d'abord (« Use Landline for phone links? »,
  Cancel / Use Landline) ; Cancel ne change rien.
- **Échec à l'allumage** (#56) : si un schéma ne peut pas être pris après l'autre, tout est défait
  (fichier retiré, associations des schémas déjà pris remises à zéro et défaut de l'utilisateur rendu,
  `phone-links` faux, `phone-links-previous` vide) et l'erreur remonte (« The phone links could not be
  changed. »).
- **Éteint** : le fichier est retiré, puis pour chaque schéma encore à Landline les associations de
  l'utilisateur sont remises à zéro (`reset_type_associations`) et le défaut d'avant est réécrit s'il
  était un choix de l'utilisateur et existe encore (un défaut du système revient seul). Un schéma que
  l'utilisateur a donné entre-temps à une autre appli n'est pas touché.
- **Choix documenté pour l'extension désactivée** (écran verrouillé, extension retirée sans éteindre
  l'option) : le fichier reste, mais il pointe vers un objet D-Bus que l'extension n'exporte que tant
  qu'elle est active **et** que l'option est allumée (clé `phone-links`). Sans lui, `gdbus` échoue tout
  de suite (« Object does not exist ») : aucun processus ne reste, rien n'est lancé en root, aucune
  boucle. Le lien ne fait alors rien de visible. Pour rendre le défaut, éteindre l'option.
- **Le Shell** exporte `io.github.d101c.Landline.PhoneLinks.Call(s uri)` sur sa propre connexion
  (`Gio.DBusExportedObject`, chemin `/io/github/d101c/Landline`) ; si l'export échoue, l'avertissement
  va au journal et `enable()` continue (les liens échouent comme extension désactivée) (#56). L'appel
  ouvre le menu sur la carte `phonelink`. Fréquence (`PhoneLinkGate`, `telLinks.js`, #56) : le même lien
  dans la seconde alors que sa carte est ouverte (double clic) ne fait rien ; un autre lien remplace la
  carte ; un lien de plus de 256 caractères ouvre la carte, qui dit qu'il n'a pas de numéro ; au-delà de
  5 cartes ouvertes par des liens en une minute (`PHONE_LINK_BURST`, `PHONE_LINK_WINDOW_MS`), les
  suivants ne sont pas montrés, une ligne au journal le dit : aucun programme de la session ne peut faire
  redescendre la carte sans fin.
- **Numéro** (`phoneLinkNumber()`) : `tel:` ou `callto:` (avec ou sans `//`), `%20` décodés, espaces,
  tirets, points, parenthèses retirés, « +41 (0)79 » → `+4179`, paramètres après `;` ou `?` ignorés
  (`phone-context`, `ext`, `isub` : un numéro local est composé tel quel, l'iPhone connaît son pays),
  3 à 15 chiffres. **Refusés** : lettres (numéro « vanity », nom Skype), `*` et `#` (un code de service
  venu d'une page web ne doit jamais atteindre l'iPhone).
- **Carte** : « Call Camille Martin? » (nom tiré des conversations, puis des appels récents) et le numéro
  dessous, ou « Call +41 79 000 00 09? » / « Through your iPhone » ; le choix du son comme « Call… »
  (« This computer » d'abord) ; « Cancel » / « Call ». « Call » passe par `dial()` : même commande
  `bt_call_dial`, même carte `dialing`, mêmes échecs. Rien n'est composé sans ce clic. Si l'appel est
  impossible, une ligne rouge le dit à la place des boutons (« Close ») : moteur absent, iPhone non relié
  (« Your iPhone isn’t connected, so the call can’t be placed from here. »), contrôle d'appel éteint,
  appel déjà en cours, lien sans numéro. La carte passe avant celles d'un message en attente dans la
  pastille ; fermer le menu oublie le lien.
- **Pendant un appel** (#56) : ouverte pendant un appel, une sonnerie ou un appel en route depuis
  l'ordinateur, la carte s'affiche par-dessus la carte de l'appel (« You’re already on a call. » /
  « Another call is being placed. », Close) ; Close rend la carte de l'appel. Un appel qui arrive
  **après** le lien (`_phoneAskCallsChanged()`, appels et sonneries que la carte n'a pas vus) l'emporte
  et le lien est oublié : il ne réapparaît jamais à la fin de l'appel.
- **« Call » refusé** par `dial()` (#56) : la carte reste et le dit (« The call could not be placed. »,
  Close) au lieu de disparaître.
- **Clavier** (#53) : à l'ouverture, le focus va sur **Cancel** (Close si l'appel est impossible), jamais
  sur Call ; et une touche sur Call dans les 500 ms qui suivent la construction de la carte est ignorée
  (`PHONE_LINK_KEY_GUARD_US`, événement `KEY_PRESS`/`KEY_RELEASE`) : l'Entrée ou l'Espace qui a validé
  « Ouvrir Landline ? » dans le navigateur, ou une touche tapée quand la carte descend, ne compose
  jamais. Call reste accessible à la souris et par Tab.
- **Essai ciblé dans le Shell isolé** : `INNER=tools/shell-test/proto-cartes.sh SHOTS=<dossier>
  tools/shell-test/run.sh` (un démarrage, 11 vérifications, captures `c1-*` à `c3-*`) : la proposition du
  premier lancement (le pilote dit « aucun gestionnaire » par sa commande `offre`, la machine pouvant en
  avoir un), Not Now puis plus jamais ; focus sur Cancel, Tab + Entrée dans les 500 ms ignorés, Entrée
  ensuite et clic aussitôt composent, Entrée à l'ouverture annule ; « Link again » menu ouvert ferme le
  menu sans erreur JS (#52 ; sur le code d'avant : `Invalid value 'undefined' for property text` dans
  `restMenu`).

### Lien classique à la demande (mode sobre)

Conception : étude 11 (`iphone-link-study/11-classique-a-la-demande.md`), étapes X1, X2, X3. Un moteur
récent ne tient en permanence que le lien LE (notifications, batterie, présence) et n'ouvre le lien
classique (messages, contacts, appels) que lorsqu'un besoin le demande. Il le dit dans
`bt_status` / `bt_connection_changed` : `link_mode`, `link_mode_effective`, `link_mode_reason`,
`classic_state`, `classic_leases`, `classic_opened_by`, `messages`, `contacts`, `calls_available`,
`last_sync`, `next_sync`. L'extension lit ces champs quand ils sont là, et garde ses règles d'avant
quand ils manquent.

**Tous les noms sont dans `lib/model/protocol.js`** (commandes, événements, champs, valeurs) : aucun
module n'écrit l'un d'eux en dur. Si le moteur réel s'en écarte (à lister dans
`docs/moteur-sobre-ecarts.md`), c'est le seul fichier à changer.

| Sujet | Règle (`lib/model/demand.js`, sauf mention) |
|---|---|
| Mode sobre en vigueur | `link_mode_effective === "lean"` (`_lean()`). Absent ou `full` : règles d'avant |
| Relié | en mode sobre, `le_connected` seul (`_linked()`) ; un lien classique monté sans le LE n'est pas « relié » |
| Messages, contacts | utilisables si `messages` / `contacts` vaut `ready` ou `on_demand` ; sans ces champs, si `map_open` / `pbap_open`. « New Message… », l'aide aux interrupteurs (`forbidden`), « Reply in app » et la panne « Fix It » (les deux à `unavailable` pendant 20 s) en dépendent. Lien classique fermé au repos : **aucun avis d'erreur** |
| Ligne du menu | aucune (#51 : « Messages and calls connect when needed. » était un détail interne). `classic_state` n'est pas reflété dans le menu : il se reconstruirait à chaque ouverture du lien et perdrait le clic fait à cet instant ; l'attente est dite là où elle compte (réponse en route, appel en route) |
| Mode appliqué ≠ réglage | une ligne dit pourquoi d'après `link_mode_reason` (`no_le`, `notifications_off`, `phone_keeps_link`) |
| Baux | `bt_lease` `reading` tant qu'une conversation ou « New Message… » est à l'écran ; `setup` tant que le parcours est ouvert ; renouvelés toutes les 4 min (`LEASE_RENEW_MS`), rendus à la fermeture. Seulement si le réglage ou le mode en vigueur est `lean` |
| Envoi | `bt_send_progress` (`connecting`, `sending`) pilote la pastille et le sous-titre de la carte ; chaque étape relance le délai de 30 s. Pendant un envoi, la carte rouverte le dit et son bouton d'envoi est inactif. L'échec garde le texte et affiche la phrase du moteur |
| Message montré dès sa notification | avec `messages` utilisable, la notification de Messages fait surgir la pastille tout de suite (`lib/model/messages.js`, `_provisionalFrom()`) ; le `bt_message` qui suit prend sa place (même début de texte ; même expéditeur, même fil, ou groupe) sans seconde pastille. Ouverte avant : fil connu par le nom → carte de réponse normale ; fil inconnu → la bulle et « Getting the conversation from the iPhone… », puis le champ de réponse à l'arrivée du message. Sans message au bout de 60 s (`PROVISIONAL_MS`) : elle reste dans son fil s'il est connu, sinon part en notification GNOME. Retirée sur l'iPhone avant : elle disparaît |
| Notifications en pause | `le_state:"paused"` : deux barres ambre sur l'icône (état `paused`), nom lu « iPhone linked, notifications paused », et dans le menu un bloc « Notifications paused » + le geste (la cause est lue par les lecteurs d'écran) (« Turn Bluetooth off and on again on your iPhone. On the iPhone: Settings (Réglages) › Bluetooth. », le libellé suivant la langue de l'iPhone), par `le_cause` (`after_resume`, `phone_silent`, `phone_away`, `unresolved_address`, `advert_off_air` ; cause inconnue : `le_detail` du moteur). Non relié et `phone_away` : rien de plus que « Out of range ». `waiting` et relié : rien (#51). `off`, `unavailable` : rien (réglage, ou réparation) |
| Batterie | avec un moteur qui publie `le_state`, la dernière valeur connue reste à sa place à droite du nom, atténuée et précédée de « ~ », 12 h au plus (`BATTERY_KEEP_MS`) ; le lecteur d'écran dit son âge. Moteur plus ancien : elle disparaît avec le lien, comme avant |
| Réglage | Phone Settings, groupe « Connection » : « Light (recommended) » / « Always connected », avec la phrase qui dit la différence. Clé miroir `link-mode` → `bt_set_link_mode` ; `link-mode-state` (écrite par l'extension : vide = moteur à un seul mode, la ligne est grisée ; sinon « mode en vigueur:motif ») |

**Appel entrant (X2, `lib/model/calls.js`).** L'appel existe dès `bt_incoming_call` (`state:"ringing"`),
avant tout `bt_calls` ; la carte a alors toujours ses trois boutons. `answer_here` : `connecting` →
bouton grisé « Answer here · Connecting… » ; `ready` (ou appel vu par `bt_calls`) → actif ;
`unavailable` → « Answer here · unavailable » et « This computer couldn’t connect in time… ». Si le
moteur reste muet, la carte cesse d'attendre d'elle-même au bout de 17 s (`ANSWER_HERE_MS`).
« Answer on iPhone » et « Decline » envoient `bt_call_action` avec `uid` (et `path` s'il est connu),
action `answer` (`audio:"phone"`) ou `decline` ; ils sont grisés seulement si la notification n'a pas
l'action et qu'aucun `path` n'existe. `state:"ended"` sans appel en cours ferme la carte ; l'appel manqué
reste une notification GNOME (catégorie 2). `bt_calls` lu à la demande porte `incoming`, pour une
extension activée pendant la sonnerie. Pendant l'appel, « This computer » est grisé tant que
`calls_available` n'est pas `ready`. Sans `bt_incoming_call` : la carte d'avant.

**Passer un appel (X3, `lib/model/dial.js`).** Constat d'usage du 17 novembre : « Call… », rangé en bas du
menu, ne se trouvait pas. Il est maintenant **un bouton sous l'en-tête**, à côté de « New Message… », avant
les conversations ; chaque conversation du menu porte un bouton d'appel rond à droite, l'en-tête d'une
conversation ouverte aussi (clé `tel:`), et chaque ligne des appels récents qui a un numéro est activable
(icône d'appel en bout de ligne). Tous ouvrent la carte `dial` ; hors « Call… », elle est préremplie **sans
composer**. Ils sont offerts dès que le contrôle d'appel est actif, **dans les deux modes et avec un moteur
plus ancien**, que le mains-libres soit là ou non à cet instant : un appel qui ne peut pas partir le dit
(« Couldn’t reach the iPhone. », « Try Again »). Recherche par
`bt_list_contacts` (adresses `tel:` seulement), numéro tapé proposé en premier ; choix « This computer »
/ « iPhone », **« This computer » par défaut** (qui appelle d'ici est devant l'ordinateur ; le réglage
`call_audio` ne vaut que pour les appels entrants), absent si le moteur ne connaît pas `bt_audio`, ou si
le mains-libres est monté et dit que l'ordinateur ne peut pas porter le son. Le choix part dans `audio`
**même quand `call_audio_switchable` est faux** : il l'est tant que le lien classique est fermé (mode
sobre), et le choix n'arrivait alors jamais au moteur (essai réel du 17 novembre : appel passé d'ici,
son resté sur l'iPhone). Un clic sur une ligne
envoie `bt_call_dial` (`number`, `audio`, `operation_id`). Carte `dialing` : « Connecting to iPhone… »
(`connecting`), « Getting the call ready… » (`hands_free`), « Calling… » (`dialing`, ou moteur sans
`bt_call_progress`) ; « Cancel » (`bt_call_cancel`) tant que le numéro n'est pas composé, et seulement
avec un moteur qui connaît les deux modes. Échec : phrase selon `reason` (`no_link`, `no_hands_free` →
« Couldn’t reach the iPhone. » ; `refused`, `busy`, `calls_off`), « Try Again » ; `cancelled` → menu.
Sans nouvelle du moteur : 45 s (`DIAL_TIMEOUT_MS`). Numéro accepté et aucun appel : `bt_call_progress`
`no_call` (après 20 s côté moteur, 25 s d'attente au plus ici, `DIAL_PLACED_MS`) → « The call didn’t
start. », « Try Again ». Dès que l'appel paraît dans `bt_calls`, la pastille et la carte d'appel
habituelles prennent le relais. **Le son voulu ici est suivi par le moteur** : tant que
`calls.audio_wanted` vaut `here` et que `calls.audio_route` ne le confirme pas, la carte dit « moving to
this computer… » (sans délai propre à l'extension : le moteur redemande le lien voix trois fois) ;
`bt_call_progress` `audio_here` le confirme ; `audio_failed` (ou `calls.audio_failed`, pour une extension
activée en cours d'appel) → « Couldn’t bring the call sound to this computer », l'appel se poursuit sur
l'iPhone et reste pilotable ; motif `voice_link_unused` : la carte ajoute que l'appel peut être muet des
deux côtés et qu'il faut choisir « iPhone ». Si le son arrive ensuite, l'erreur s'efface. Le client n'attend pas la réponse de `bt_call_dial`
(`UNACKNOWLEDGED` dans `engineClient.js`) : elle peut venir trente secondes plus tard, et l'annulation doit
pouvoir partir entre-temps.

**Alignement sur le moteur réel** (`docs/moteur-sobre-ecarts.md`, branche `moteur-sobre`) : aucun nom ne
diffère. Ajusté côté interface : `messages` / `contacts` à `unavailable` seuls ne font plus une panne
« Fix It » (un service relâché ou un iPhone tout juste parti valent aussi `unavailable`) : il faut en plus
les motifs d'erreur d'avant ; `unknown` n'offre rien et ne signale rien ; le démarrage en mode complet
pour `notifications_off` est une ligne neutre du menu, sans réparation proposée ; `answer_here` peut
repasser de `unavailable` à `ready` ; `bt_call_result` reçu une seule fois (ou deux, moteur d'avant) donne
le même résultat ; « Call… » ne dépend plus de `calls_available:"ready"`. L'entrée provisoire d'un message
n'existe pas dans le moteur : c'est bien l'extension qui montre la notification puis la fusionne.

### Le quotidien : son, batterie, présence, autres applis, connexion

Branché sur `docs/moteur-quotidien.md`. Tout est toléré absent : avec un moteur plus ancien, pas de
réglage du son dans le menu, un seul « Answer », la pastille de batterie d'avant.

- **Son.** « Play iPhone audio on this computer » (menu au repos et Phone Settings, éteint par défaut) :
  `bt_set_phone_audio` avec `route` `here` ou `phone`, état lu dans `bt_audio.phone_audio`. Le choix est
  mémorisé par le moteur ; tant que la réponse n'est pas revenue (8 s), l'interrupteur montre ce qui a été
  demandé. En mode sobre, depuis #43, le choix seul ne tient plus le lien classique : le moteur l'ouvre
  quand on allume l'option (ou qu'on le redemande), le garde tant que le son joue, et le rend 60 s
  après la fin du son, plus 30 s de maintien (voir `docs/moteur-quotidien.md`). Sous l'interrupteur, une ligne dit où en est le son
  (`phoneAudio.state` de la vue) :
  - `connecting` « Connecting… » : demandé (interrupteur allumé, ou « Connect for Sound »), et
    `bt_audio.phone_audio_route` n'est pas encore `here`. En mode sobre le lien classique est fermé et la
    route vaut `unavailable` : la demande ouvre le lien (2 s environ), puis le moteur ouvre le flux A2DP
    (5 s après) ;
  - `playing` « Playing on this computer. » quand la route vaut `here`, suivi, jusqu'à la première
    fermeture du menu sur cet état (une fois par session du Shell), de « If the music stays on the iPhone,
    open Control Center, touch the AirPlay button and choose “<nom>” » : le nom est celui de
    l'adaptateur Bluetooth (`bt_status.adapters[].name`, ce que l'iPhone affiche), à défaut le nom d'hôte ;
    le libellé de l'iPhone suit sa langue (`phoneLabels.js`, `controlCenter`) ;
  - `standby` « Not connected for sound right now. » et une ligne **« Connect for Sound »** : l'option est
    allumée, rien n'est demandé, la route n'est pas `here` (le moteur a rendu le lien après un silence,
    ou l'iPhone est revenu). « Connect for Sound » renvoie `bt_set_phone_audio here` (`connectPhoneAudio()`),
    que le moteur prend pour une nouvelle demande, et repasse à `connecting` ;
  - `failed` au bout de **40 s** de demande sans route `here` (`PHONE_AUDIO_WAIT_MS` : ouverture 20 s au
    plus, mains-libres 8 s, flux 5 s) : « The iPhone did not connect for sound. Check that its Bluetooth is
    on and that it is nearby, then try again. », en rouge, avec « Connect for Sound » ; l'interrupteur reste
    allumé (le choix tient) ;
  - `away` : l'iPhone n'est pas relié ; rien n'est dit, rien n'est attendu.

  #38 (18 novembre) : avec GNOME 51, l'interrupteur (`PopupMenu.Switch`) est réactif et sa poignée porte un
  geste de glissement ; un clic **sur l'interrupteur** le faisait basculer à l'écran sans activer la ligne,
  donc sans rien envoyer au moteur (seul un clic sur le libellé marchait). `switchItem()` suit désormais
  `notify::state` de l'interrupteur : ligne, poignée, rail ou clavier passent tous par là. Le Shell
  d'essai clique la poignée (`pilote switch <nom>`, `rail` pour le rail).
- **Relié par le lien classique (#43).** En mode sobre, `_linked()` lisait le seul `le_connected`. Le
  18 novembre à 18:18 le lien LE est tombé, le classique (tenu par le son) est resté et les notifications
  ont continué par lui (`ancs_ready`), mais la barre disait l'iPhone absent et le menu « Notifications
  paused » (`phone_silent`). Désormais, LE absent, `classic_connected` et `ancs_ready` vrais : relié, et
  pas de pause annoncée (`_classicCarriesNotifications()`). Le moteur referme ce classique au bout de 30 s
  pour que le LE revienne.
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
- **Batterie.** `bt_battery_low` → icône `battery` et ligne du menu (plus de notification GNOME), une fois par seuil
  franchi (un franchissement des deux à la fois n'annonce que le plus bas ; réarmement à seuil + 5),
  réglage `battery-alerts`. Pas en silence.
- **Présence.** `bt_presence` donne la raison montrée dans le menu : `out_of_range` « Out of range »,
  `switched_off` « Bluetooth is off on the iPhone », `computer_suspended` « Reconnecting after sleep… »
  tant que la reconnexion dure (voir « Reconnexion »), « Not back since this computer slept » ensuite ;
  liens tombés mais `state:"here"` : la reconnexion (« Your iPhone is back, setting up the link… ») ; `lock` `pending` / `locked` est ajouté (« this
  computer locks shortly », « this computer was locked »). Réglage « Lock this computer when I walk away
  with my iPhone » (Phone Settings, éteint par défaut, délai de 5 à 600 s) : `bt_set_lock_on_away` avec
  `enabled` et `grace_seconds`. Le texte dit le délai réel (départ constaté 20 à 35 s après la perte du
  signal) et que rien ne se déverrouille seul. C'est le moteur qui verrouille.
- **Réglages tenus par le moteur.** Phone Settings tourne dans un autre processus : les trois réglages
  ci-dessus passent par des clés miroir (`phone-audio-here`, `lock-on-away`, `lock-away-seconds`). Une
  clé changée part au moteur (`wantSetting()`), une valeur tenue par le moteur revient dans la clé
  (`onSetting`) ; pendant 8 s après un envoi, une valeur plus ancienne encore en route n'est pas prise
  pour un changement.
- **Autres applis : « Open in … »** (ancien « Reply in app », voir « Passer le relais aux applis
  officielles »). La conversation n'est ouverte que si l'expéditeur est sûr : titre seul (un sous-titre,
  « Nom @ Groupe », « ~pseudo » écartent), puis soit le titre est un numéro complet, soit le carnet
  (`bt_list_contacts` sur le nom) contient **un seul** contact de ce nom exact avec **un seul** numéro,
  écrit avec son indicatif (`lib/replyLinks.js`, noté sur la notification : `replyNumber`). Jamais pour
  un groupe. L'action n'efface rien sur l'iPhone.
- **Connexion de l'iPhone** : voir « Internet from iPhone » ci-dessous.

### Internet from iPhone (issue #39)

Avant : il fallait agir sur l'iPhone (Partage de connexion) **et** sur l'ordinateur (« Use iPhone's
Internet Connection »), sans lien entre les deux. Maintenant **un seul réglage**, « Internet from
iPhone », dans le menu au repos ; l'ordinateur fait tout ce qu'il peut faire, dit exactement le geste
qui reste à faire sur l'iPhone quand il en reste un, et l'icône de la barre le montre.

**Ce que l'ordinateur pilote, et ce qu'il ne pilote pas.**

- Il pilote la connexion : NetworkManager crée de lui-même un profil Bluetooth pour l'iPhone appairé
  (« <nom> Network », `bluetooth.type panu`, `bluetooth.bdaddr` = l'adresse de l'iPhone, `autoconnect
  no`), que GNOME montre en tuile « Tether ». `lib/hotspot.js` le trouve par libnm (`gi://NM`), l'active,
  le désactive et suit son état. Rien n'est créé, masqué ni supprimé : la tuile de GNOME reste. Pas de
  profil (iPhone qui n'annonce pas de NAP), pas d'entrée.
- Il ne pilote **pas** le Partage de connexion de l'iPhone : aucun profil Bluetooth ni aucune API
  publique ne permet à un accessoire de l'allumer. Apple : pour un appareil qui se connecte en Bluetooth,
  « Autoriser d'autres utilisateurs » doit être allumé ; le Partage de connexion instantané (sans rien
  toucher sur l'iPhone) n'existe qu'entre appareils du même compte Apple ou du même partage familial
  (support.apple.com HT204023, HT209459). Un ordinateur Linux n'en est pas. Ce qui reste incertain et
  n'a pas été essayé : si un appareil déjà appairé peut se joindre quand l'interrupteur est éteint mais
  que l'écran Partage de connexion est ouvert (Apple demande de « rester sur cet écran » pendant la
  connexion).
- Le lien classique n'a pas besoin d'être ouvert d'avance : l'activation de NetworkManager passe par
  `org.bluez.Network1.Connect`, dont la connexion L2CAP vers le service réseau de l'iPhone monte
  elle-même le lien BR/EDR (« page »). Pas de bail du moteur pour cela. Le moteur, en mode sobre, ne
  ferme jamais un lien que le partage utilise (`held_by_other`, lu sur `Network1.Connected`) ; deux trous
  sont bouchés dans cette branche (`classic_demand.hpp`) : une fermeture déjà demandée n'est plus
  répétée si le partage a pris le lien entre-temps (le lien est alors traité comme celui de l'iPhone,
  fermé 2 min après la fin du partage), et une ouverture de données qui échoue sur un lien que le partage
  utilise n'est plus suivie d'une fermeture (tests `TetheringStartedDuringACloseCancelsTheClose`,
  `AFailedOpeningLeavesALinkTetheringUses`).

**Ce que dit NetworkManager d'un échec** (`src/core/devices/bluetooth/nm-device-bt.c`), lu par
`lib/hotspot.js` sur le signal `state-changed` du `NM.DeviceBt` de l'iPhone, avec l'état le plus avancé
atteint par la tentative (`failureKind()`) :

| Raison | Quand | Ce que cela veut dire |
|---|---|---|
| `refused` | `FAILED` / `BT_FAILED` avant `CONFIG` | la demande au service réseau de l'iPhone a échoué : Partage éteint, ou iPhone absent |
| `dropped` | `FAILED` / `BT_FAILED` après `CONFIG` | BNEP accepté, puis le lien est parti avant l'adresse |
| `no-internet` | `FAILED` / `IP_CONFIG_UNAVAILABLE` ou DHCP | relié à l'iPhone, pas d'adresse en 45 s : données cellulaires coupées, forfait |
| `lost` | `FAILED` après `ACTIVATED` (`CARRIER`, `BT_FAILED`) | la connexion établie est tombée : l'iPhone s'est éloigné, ou on a coupé le Partage |
| `timeout` | rien en 50 s (au-delà des 45 s du DHCP) | l'activation est retirée |
| `error` | NetworkManager refuse la demande elle-même | — |

BlueZ répond à tous les refus par le même « Input/output error » (`profiles/network/connection.c` :
`cancel_connection(nc, -EIO)`) : `refused` ne dit donc pas si l'iPhone est là. Le modèle le sait par le
moteur : iPhone absent (`away`, `reconnecting`, Bluetooth coupé), c'est « Your iPhone is not nearby. »,
sinon c'est le Partage. Journal de l'hôte, 16 novembre 18:46 (seul essai fait, avant ce travail) :
`bnep0 connected`, renommé `enxf8e4e3905645` par systemd-udev (`73-usb-net-by-mac.link`, l'adaptateur
Bluetooth étant sur USB), puis `bnep0 disconnected` 170 ms après la connexion (12 ms après le renommage) et `ip-config -> failed (reason
'bluetooth-failed')` : un `dropped`. Le renommage est connu pour gêner NetworkManager (Launchpad
1645257 : « cannot find device bnep0 » quand il arrive trop tôt) ; ici NetworkManager l'a suivi, et c'est
le lien qui est tombé : Partage probablement éteint, à confirmer avec le vrai iPhone.

**États et phrases** (`model/internet.js`, `_internetView()`, rendu par `cards.js`) :

| État | Interrupteur | Sous l'interrupteur | Action |
|---|---|---|---|
| éteint | non | — | — |
| `connecting` | oui | « Connecting to your iPhone… » | — |
| `on` | oui | « Online through your iPhone · battery 96 % » | — |
| `waiting` : allumé alors que l'iPhone est absent | oui | « Connects as soon as your iPhone is back. » ; la connexion part seule au retour du lien | — |
| refusé (`refused`, `dropped`), iPhone là | non | en rouge « Your iPhone did not share its connection. » puis « On your iPhone, open Settings › Personal Hotspot and turn on Allow Others to Join. Then try again. » (mots de l'iPhone entre parenthèses s'il parle une autre langue) ; **le dessin de Réglages › Partage de connexion**, « Autoriser d'autres utilisateurs » cerclé d'ambre | « Try Again », « Hide » / « Show Me How » |
| refusé, iPhone absent | non | « Your iPhone is not nearby. » / « Bring it closer, then try again. » | « Try Again » |
| `no-internet` | non | « Your iPhone answered, but gave no Internet. » / « Check that Mobile Data is on, on your iPhone. Then try again. » | « Try Again » |
| `timeout` | non | « Your iPhone did not answer in time. » / « Unlock it and keep it close, then try again. » | « Try Again » |
| `error` | non | « This computer could not start the connection. » / « Try again in a moment. » | « Try Again » |
| `lost`, l'iPhone parti (absent au moment de la chute, ou dans la minute qui suit) | non | sans rouge : « Stopped when your iPhone went out of range. » | « Reconnect When It's Back » : l'interrupteur repasse à oui, état `waiting` |
| `lost`, l'iPhone toujours là | non | « Your iPhone stopped sharing its connection. » + le geste et le dessin | « Try Again » |

- **Le dessin, une fois.** Il est montré de lui-même tant que la connexion n'a jamais abouti dans la
  session ; ensuite il attend « Show Me How ». « Hide » le range.
- **Arrêt quand l'iPhone s'éloigne.** NetworkManager le fait déjà : le lien BR/EDR tombe (délai de
  supervision), BNEP avec, et le périphérique passe en `FAILED` (`CARRIER` ou `BT_FAILED`) ; le profil est
  en `autoconnect no`, rien ne repart seul. L'extension le dit et propose de le garder (« Reconnect When
  It's Back »).
- **Icône.** `internet` en ligne et en cours (les flèches alternent), `internet_failed` jusqu'à
  l'ouverture du menu ; en ligne, l'icône dit Internet même si l'autre lien est tombé (l'iPhone est
  forcément là). Lecteur d'écran : « iPhone linked, online through the iPhone », « …connecting to its
  Internet », « …its Internet did not connect ».
- **Fil.** L'interrupteur appelle `model.setInternet(on)` ; le modèle décide (attendre, demander) et
  passe la main par `onInternet(on)`, que l'extension exécute au tour suivant de la boucle
  (`GLib.idle_add`) sur `Hotspot.set()`. `Hotspot` rend `{state, failed, reason}` à `model.setHotspot()`.

**À essayer avec le vrai iPhone** : interrupteur éteint puis allumé sur l'iPhone, écran Partage ouvert
ou non ; la raison réellement vue pour « éteint » (`refused` ou `dropped`) ; s'éloigner en ligne (délai
avant `lost`, et lequel du moteur ou de NetworkManager le voit d'abord) ; données cellulaires coupées
(`no-internet`) ; le renommage `enx…` sur plusieurs essais. Libellés français du dessin (« Autoriser
d'autres utilisateurs », « Mot de passe Wi-Fi », « Partage familial », « Optimiser la compatibilité ») à
relever sur l'iPhone ; les deux lignes sous le titre sont un résumé, pas un relevé.

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

### Installer le moteur en un clic (Réglages › Engine)

Issues #20 et #21. Une page « Engine » dans les réglages (`lib/enginePage.js`), seule à télécharger ou
écrire quoi que ce soit : tout se passe dans le processus des préférences, jamais dans celui du Shell.
Elle s'ouvre d'elle-même quand le moteur manque, qu'il est plus ancien que celui que cette version
installe, ou qu'il ne répond pas (essai du socket de 1,5 s à l'ouverture). Côté Shell, la carte
« moteur absent » mène à cette page : « Install the Engine… » quand `~/.local/lib/landline/engine/landlined`
n'existe pas (lu par `query_info_async`), « Engine Settings… » en second bouton sinon ; et le menu au
repos dit « Update the Engine… » quand `engine/VERSION` est plus ancien que la version épinglée.

**Ce qui est épinglé.** `lib/engineRelease.js`, seul endroit : version, URL de l'archive
(`landline-engine-<v>-x86_64.tar.zst` des releases GitHub), SHA-256 de l'archive, de
`system/landline-setup` et de `engine/landline-le-params`. Pas de manifeste distant : une nouvelle
version du moteur, c'est une nouvelle version de l'extension. Épinglé : le moteur 0.2.0 (archive
`scripts/build-release.sh`, `docs/release.md`). Des sommes à zéro (valeurs provisoires) font échouer
toute vérification, et la page le dit (« This build of Landline has no engine release pinned yet »).
`tests/engineRelease.test.js` installe la vraie archive (`dist/` ou `$LANDLINE_DIST`), servie en local,
dans un HOME temporaire, et vérifie que les trois sommes épinglées sont les siennes ; sans archive il est
sauté.

L'unité écrite garde `LD_LIBRARY_PATH=…/engine/lib`, comme `system/landline-engine.service` : inutile
avec l'archive (RPATH `$ORIGIN/lib`), sans effet gênant, et un essai vérifie que les deux unités ont les
mêmes directives.

**Les étapes**, chacune une ligne avec son icône d'état (en attente, en cours, fait, échec, sautée) et,
en haut, le dessin de l'étape en cours (`illustrations/engine-*.svg`, dessins à nous) :

| Étape | Ce qui est fait | Échec : ce qui est dit, ce qui est proposé |
|---|---|---|
| Download | libsoup 3, vers `~/.cache/landline/<archive>.part` puis renommé ; somme calculée au fil de l'eau ; barre de progression ; une copie complète d'un essai précédent est reprise si sa somme est la bonne | « The download did not work » : Try Again, Show Steps by Hand |
| Check | SHA-256 de l'archive contre la valeur épinglée ; une archive fausse est supprimée | « This download can’t be trusted » : Try Again (télécharge à nouveau), Show Steps by Hand |
| Install | libgnome-autoar (tar.zst) dans `~/.local/lib/landline/.staging`, contenu vérifié (démon exécutable, `VERSION` = version épinglée, `landline-setup` et `landline-le-params` aux sommes épinglées), assemblé en `engine.new` (`engine/` + `system/`), puis bascule : `engine` → `engine.old` (un seul ancien gardé), `engine.new` → `engine` ; si le moteur en place est déjà la version épinglée (Reinstall, nouvel essai après un échec postérieur à la bascule), il est remplacé et `engine.old`, le vrai précédent, reste tel quel ; écrit `~/.config/systemd/user/landline-engine.service` et `obex.service.d/landline.conf` | « could not be unpacked / copied / is not an engine » ; rien n'est basculé avant que tout soit vérifié : Try Again (reprend à la vérification), Show Steps by Hand |
| Start | API D-Bus de systemd sur le bus de session : `Reload`, `EnableUnitFiles`, `ResetFailedUnit`, puis `MainPID`, `ExecMainStartTimestampMonotonic` de l'unité lus, `RestartUnit` ; ensuite, chaque seconde, 20 s au plus : l'unité doit être `active` avec un `MainPID` **et** une heure de démarrage différents de ceux d'avant (`RestartUnit` rend la main dès le travail mis en file, et l'ancien moteur répond encore), puis `protocol_info` sur le socket, puis le même processus toujours là | « The new engine does not answer » : **Use Previous Engine** (si `engine.old` existe), Try Again, Show Steps by Hand (les commandes de retour arrière) |
| Bluetooth settings | `landline-setup status` (sans droits) ; déjà bon : étape cochée « Already right » ; sinon « Continue… » lance le script racine constant (`docs/systeme.md` §3 bis) | fenêtre du mot de passe fermée : « No password, nothing changed », Continue… de nouveau, **Skip for Now**, ou la commande à la main : `printf … | sha256sum -c - && sudo …/engine/system/landline-setup apply` (les deux fichiers vérifiés contre les sommes épinglées avant `sudo`, chemins passés par `GLib.shell_quote`) ; pour un moteur posé par `install-local.sh`, `sudo ~/.local/lib/landline/landline-setup apply` précédé d'un commentaire (pas de somme épinglée) ; le reste marche sans |
| Link your iPhone | « Link My iPhone… » lève la clé `tour-request` ; le Shell ouvre le parcours d'accueil dès que le moteur répond (30 s au plus) et ferme la fenêtre des réglages | — |

**État au repos** (`classify()`, `lib/engineInstall.js`) : rien → « Install » ; moteur sans `VERSION`
(posé par `scripts/install-local.sh`) → « An engine is already here », « Replace » après une question
(l'actuel devient `engine.old`) ; version plus ancienne → « Update » ; la même → « Reinstall » ; plus
récente → rien ; moteur absent mais `engine.old` présent (bascule interrompue) → « Put Back ».
« Use Previous Engine » reste offert tant que `engine.old` existe.

**Une chose à la fois.** Pendant une installation, l'étape du mot de passe, un retour arrière ou une
désinstallation, « Uninstall the Engine… » et les boutons sont grisés (seul Cancel d'une installation
reste), et `EngineInstaller.run()`, `rollback()` et `uninstall()` refusent eux-mêmes (`busy`).

**Désinstaller** (« Uninstall the Engine… », avec confirmation) : `StopUnit`, `DisableUnitFiles`, retire
`engine`, `engine.old`, `engine.new`, `.staging`, `~/.local/lib/landline/landline-setup`, l'unité, le
complément obex, l'archive en cache, puis `Reload`. Les données du moteur (appairage, réglages,
messages gardés) restent. Si l'assistant de root est en place, une seconde réponse « Uninstall and Undo
Bluetooth Changes… » lance d'abord `pkexec /usr/local/libexec/landline/landline-setup revert`.

**Pour EGO.** Aucun processus lancé pour installer : réseau (libsoup), décompression (autoar), écritures
(Gio) et systemd (D-Bus) depuis la fenêtre des préférences. Les deux seuls `pkexec` de la page : le
script constant (`pkexec /bin/sh -c '<script>' landline-bootstrap <deux chemins>`) et le `revert` sur la
copie de root. `shexli` sur l'archive : rien de nouveau (EGO-A-005 et EGO-M-004, déjà connus).

**Code.** `lib/engineInstall.js` (règles pures : classement, machine à états des étapes, remèdes,
textes de l'unité et du complément, script racine, commandes à la main), `lib/engineInstaller.js`
(téléchargement, vérification, extraction, bascule, retour arrière, désinstallation, sonde du socket ;
systemd derrière l'interface `SystemdUser`), `lib/enginePage.js` (la page). Le statut de l'assistant est
lu d'abord sur `engine/system/landline-setup`, puis sur `~/.local/lib/landline/landline-setup`
(installation à la main) : `names.js`, `LOCAL_HELPERS`.

Captures sans rien installer : `tools/shell-test/engine-page.sh` (mutter sans écran, HOME temporaire,
systemd et moteur en doublures) écrit `docs/captures/engine-*.png`.

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
dans `bt_devices`), puis, calés à droite, la batterie : le niveau (« 96 % », « ~96 % » pour le dernier
connu) et une petite pile **couchée** (`BatteryGauge`, `St.DrawingArea` : debout, on la confondrait
avec le téléphone de l'icône), remplie au niveau, rouge quand elle est faible (`card.batteryLow`), un
éclair découpé quand le moteur dit qu'elle charge (`card.batteryCharging` ; le moteur ne le sait pas
aujourd'hui : `null`, rien de dessiné). Plus de mot d'état (#51). Le nom reçoit la place que laissent les deux côtés quand chacun compte pour la largeur
du plus large : il est donc centré sur le menu, pas sur l'espace libre ; trop long, il est coupé par des
points de suspension (un nom d'un seul emoji tient au milieu). Les deux textes sont posés sur la même
ligne de base. Si la place centrée tombait sous 64 px, le nom prendrait l'espace entre les deux côtés
(cas non rencontré dans les essais). Ce qui ne va pas passe en une ligne sous l'en-tête (voir « Ce que
dit l'icône, ce que dit le menu (#51) »).

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
- Notifications des autres applis : voir « Notifications des autres applis : un seul système ».

### `disable()`

Minuteries retirées, processus en cours (`systemctl`, assistant système) terminés — sauf l'assistant
déjà lancé en root par `pkexec`, qui finit seul —, boîtes de dialogue détruites (y compris celle qui
finit son fondu de fermeture), client arrêté (socket fermé, opérations annulées), signaux des réglages déconnectés,
bouton détruit (son menu, la carte d'appel, la minuterie de reconstruction, l'animation), historique
« Earlier » vidé, recherches d'icônes d'applis annulées et session Soup lâchée. Le guetteur de silence (réglage de GNOME, contrôleur
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
`message-leo [texte]`, `appel-manque [nom]` et `repondeur` (ce que l'appli Téléphone publie : catégories
ANCS 2 et 3), `appel`, `decroche`, `raccroche`, `batterie-faible`, `batterie N`, `permission`,
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
appel ne repart pas vers l'iPhone) ; `notification-appli whatsapp|telegram|signal|teams|mail|calendrier|groupe [expéditeur]`. Reconnexion (#30) : `lien-inutilise
appel ne repart pas vers l'iPhone) ; `notification-appli whatsapp|telegram|signal|discord|teams|mail|groupe [expéditeur]`, avec `-vocal`,
`-photo` ou `-video` après le nom de l'appli pour un média (« 🎤 Voice message », #49). Reconnexion (#30) : `lien-inutilise
[fautes [cause]]` (l'iPhone vu, un lien LE qui meurt 0,9 s plus tard sans avoir servi, `waiting` /
`unsteady` ou `holding_off`, `le_strikes`), `rappel` (fin d'un retrait : `unsteady`), `reveil` (`waiting` /
`after_resume`, présence `computer_suspended`).
Il répond à `bt_audio`, `bt_presence`, `bt_set_phone_audio`, `bt_set_call_audio`, `bt_set_lock_on_away`,
publie `battery_state`, `calls.audio_route` et `bt_battery_low` au franchissement de 20 % et 10 %, et
ne liste plus la classe d'appareil parmi les prérequis. Comme le vrai, `phone_audio_route` vaut
`unavailable` tant que le lien classique est fermé ; en mode `sobre`, `phone_audio:"here"` prend un bail
`audio` qui ouvre le lien (délai `ouverture`) ; `son-silence` le rend, comme le moteur après un silence
(#43), et `refuse-ouverture` fait échouer l'ouverture (#38). L'adaptateur s'appelle `portable-essai`. Vraies données : `message-numero`, `message-code`, `message-email`,
`message-long`, `message-emoji`, `message-vide`, `message-suite`, `message-de QUI [texte]`,
`historique-reel`, `appel-inconnu`. Appairage : `appairage-partiel` (appairage BR/EDR seul fait hors
parcours), `echecs-authentification`, `sans-le` / `avec-le`, `contrat-ancien` / `contrat-nouveau`
(moteur d'avant ou d'après `docs/contrat-appairage.md`), `delai balayage|demande|oubli|liens|relance N`.
Il répond aussi à `bt_scan` (l'iPhone apparaît au bout d'une seconde), à `bt_pair` (code 703405, liens
montés un instant plus tard, trois interrupteurs éteints ; `stale_bond` sur un appairage inutilisable,
`fresh`), à `bt_unpair` et à `bt_set_calls` (le contrôle d'appel démarre coupé, comme le vrai). Chaque commande reçue est affichée, précédée de `RECU`.
Toutes les données sont inventées (Camille Martin, Léo, numéros en +4179 000 00 0x).

Lien classique à la demande : **par défaut le faux moteur reste le moteur d'avant** (aucun champ ni
événement nouveau), pour que les essais écrits avant ne changent pas. `sobre` le fait parler comme le
moteur récent en mode `lean` (lien classique ouvert par bail, gardé puis fermé ; `bt_send_progress`,
`bt_incoming_call`, `bt_call_progress` ; `bt_set_link_mode`, `bt_lease`, `bt_call_cancel`), `complet`
en mode `full`, `protocole-ancien` revient en arrière. `garde-le-lien` / `rend-le-lien` (motif
`phone_keeps_link`) ; `notifications-en-pause [cause]` / `notifications-reprises` (`le_state`) ;
`refuse-ouverture` / `accepte-ouverture` ; `mains-libres jamais|normal` ; `composition
normale|echec-lien|echec-mains-libres|refus|sans-appel` (`no_call`) ; `son-appel normal|echec|inutilise`
(le son d'un appel voulu ici : `audio_here`, ou `audio_failed` avec `no_voice_link` /
`voice_link_unused` ; `calls.audio_wanted`, `calls.audio_failed`) ; délais réglables `ouverture`, `mains-libres`, `maintien`,
`composition`, `sonnerie`, `envoi`, `reessai`. `sans-accord` / `accord` : démarrage sans accord des
notifications mémorisé (`notifications_off`). `message-de inconnu` : un expéditeur dont aucun fil
n'existe. Comme le moteur réel : en protocole récent `bt_call_result` n'arrive qu'une fois à un client
abonné, `bt_incoming_call` n'est émis que si le contrôle d'appel est actif, `bt_call_dial` échoue tout de
suite (`no_hands_free`) en mode complet sans lien, `bt_lease` vaut `held:true` par défaut,
`bt_retry_profiles` tient le lien, et la présence compte un lien classique ouvert. En protocole récent, `appel` annonce d'abord l'appel par
`bt_incoming_call`, `bt_calls` suit à l'arrivée du mains-libres ; `raccroche` pendant la sonnerie est
l'appelant qui renonce (appel manqué). `bt_call_dial` réussit aussi en protocole ancien quand le
contrôle d'appel est actif.

### Tests

```sh
tests/run.sh        # modèle (928 vérifications), applis officielles et liens tel: (88), liens tel:
                    # écrits pour de vrai dans un HOME/XDG temporaire (15), client contre le faux
                    # moteur (101), icônes des applis contre un faux App Store local (39),
                    # assistant (24), installation du moteur : règles (59) et essai dans un
                    # dossier temporaire (82)
```

`tests/engineInstaller.test.js` installe pour de vrai, mais dans un HOME, un `~/.config`, un `~/.cache`
et un dossier d'exécution temporaires : une archive factice (`tar --zstd`) servie par
`python3 -m http.server` sur 127.0.0.1, systemd en doublure (le vrai n'est jamais appelé), le moteur qui
répond en doublure ou `tools/faux-moteur.py`. Il couvre la somme fausse, le réseau coupé, la première
installation, la mise à jour, le moteur muet puis le retour arrière, la reprise après un refus de
systemd, l'installation à la main reconnue, la bascule interrompue, une archive qui n'est pas un moteur
ou dont l'assistant n'est pas le bon, la désinstallation, et le script racine joué avec `/bin/sh` (sans
`pkexec`, dossier de travail déplacé hors de `/run`) : bons fichiers, assistant ou auxiliaire modifiés,
mauvais arguments, lien symbolique.

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
(qui vit sur le bus système, le vrai : jamais joint ; `hotspot profil|sans-profil|refuse|accepte|sans-internet|lache`) et le
partage d'écran (`sharing`). Il retrouve aussi lui-même les boutons, par leur libellé ou leur classe de
style : l'extension n'expose plus de table de contrôles. `tools/shell-test/bin` passe en tête du `PATH` :
le `pkexec` qui s'y trouve ne demande aucun mot de passe et n'élève aucun droit (il lance la commande, ou
refuse comme si l'utilisateur avait annulé, ou attend tant qu'un fichier témoin existe : le temps passé
devant la fenêtre de mot de passe). Le pilote sait aussi basculer le Ne pas déranger (`dnd`), changer un
réglage (`reglage`), amener le pointeur sur une bulle (`hoverbanner`), faire défiler une fenêtre
(`scroll`), cliquer à un endroit de l'écran (`clicxy`) et couper ou remettre l'indicateur stable (`steady`). Enfin le `systemctl` note l'appel et échoue, sauf `restart` qui fait jouer `relance` au faux moteur (et échoue
si le fichier `$FAUX_SYSTEMCTL_REFUSE` existe). `LC_ALL` est fixé à l'anglais pour que la langue devinée de
l'iPhone ne dépende pas de la machine. Pour la reprise après le mot
de passe, le pilote ouvre une vraie `ModalDialog` du Shell par-dessus le parcours, comme le fait l'agent
polkit, puis la referme.
Une petite extension d'essai (`tools/shell-test/pilote@test`) fait de vrais clics de souris et de vraies
frappes par un périphérique virtuel de Clutter, lit l'état et prend les captures. Le script déroule tous
les états (285 vérifications), vérifie chacun, copie les captures dans `docs/captures/` et relève les erreurs JS du journal du
Shell (`tools/shell-test/out/shell.log`). `SECTIONS="parcours reparation panne cartes stable silence quotidien internet sobre icones courant"`
limite l'essai à certaines parties (`icones` : les deux styles de l'icône, état par état) ; `HEURE=1`
écrit l'heure devant chaque vérification, pour la rapprocher d'une ligne du journal du Shell ; le pilote sait aussi poser un réglage texte (`texte link-mode full`) et
fermer les fenêtres (`fermefenetres`) ; `SCHEME=prefer-light SHOTS=<dossier>` le joue en style clair.

Pour jouer à la main dans une fenêtre, même principe avec `--devkit` à la place de `--headless` (non
essayé ici).

**Essai court #49 / #50** : `INNER=$PWD/tools/shell-test/proto-relais.sh SHOTS=<dossier> tools/shell-test/run.sh`
(22 vérifications, captures `r1-*` à `r6-*`) : vocal Signal (« Open in Signal » sur la conversation si
Signal Desktop est installé sur la machine : les applis vues sont celles du système), Telegram absent
(suggestion une fois, notée dans `suggested-apps`), Teams, photo WhatsApp, puis liens tel: par `gdbus`
sur le bus privé : option éteinte (échec immédiat), confirmation, « Call » (`bt_call_dial`), numéro
inconnu, « Cancel », iPhone absent, extension désactivée (échec immédiat) ; enfin Phone Settings :
l'interrupteur « Use Landline for phone links » pose la question (une autre appli a `callto:`), Échap ne
change rien, « Use Landline » écrit le fichier dans le HOME de l'essai et prend `tel:`, `gio open
tel:…` fait descendre la confirmation (le chemin entier), éteindre retire le fichier et rend `tel:`.
Le pilote sait cliquer à un endroit de l'écran (`pointe x y`, pour une fenêtre GTK ; coordonnées
réglables : `ONGLET_IPHONE`, `INTER_LIENS`, `BOUTON_OUI`, `DEFILE_APPELS`, `DEFILE_APPLIS`). Aucun clic
sur « Open in … » (il ouvrirait la vraie appli). Le démarrage du pilote est parfois lent : un « le
pilote n'a pas pris » dans `session.log` se rejoue.

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
- Icônes des applis (réglage `show-app-icons`, vrai par défaut) : depuis le Shell, une requête par appli
  à `itunes.apple.com/lookup` (l'identifiant de l'appli et le pays) puis l'image sur `*.mzstatic.com`,
  gardée dans `~/.cache/landline/app-icons`.
- `systemctl --user start|restart landline-engine.service`, « Open in … » (appli officielle ou client
  web, sur un clic), et NetworkManager pour la connexion de l'iPhone ;
- liens tel: (#50) : fichier `~/.local/share/applications/landline-phone-links.desktop` et défauts
  `tel:`/`callto:` écrits par Phone Settings (ou par le Shell sur « Use Landline » de la proposition
  unique du menu, seulement quand aucune appli ne les a), objet D-Bus exporté sur la connexion du Shell tant que
  l'option est allumée. Déclaré dans `docs/ego/description.txt` (repris dans `metadata.json`).
- Réglages › Engine : téléchargement (libsoup) de l'archive épinglée depuis GitHub, sur un clic ;
  écritures dans `~/.local/lib/landline`, `~/.cache/landline` et `~/.config/systemd/user` ; systemd par
  D-Bus ; `pkexec /bin/sh -c '<script constant>'` pour les réglages système ; le presse-papiers (GTK)
  écrit par « Copy Commands » de cette page.

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
  - `p6` : autorisation réduite à un seul point ; `15` : carte d'appel à deux réponses ;
  - `x01` à `x33` : lien classique à la demande, contre le faux moteur en mode `sobre` — `x01` menu en
    mode sobre ; `x02` à `x04` envoi à froid (« Connecting to send… », « Sending… », « Sent ») ; `x05`
    carte rouverte pendant l'envoi ; `x06`, `x07` envoi impossible (pastille, carte avec le texte gardé) ;
    `x08` à `x10` message montré dès sa notification, fil en route, puis complété ; `x11`, `x12`
    notifications en pause (anneau dans la barre, bloc du menu, batterie « ~96 % ») ; `x13`, `x14` Phone
    Settings, groupe « Connection » (Always connected, puis Light) ; `x15` mode sobre après la veille ;
    `x16` à `x19` carte d'appel entrant (connexion, prêt, en cours, indisponible) ; `x20` appel manqué
    dans la liste de GNOME ; `x21`, `x22` carte « Call » (vide, résultats et choix du son) ; `x23` à `x25`
    composition (connexion, mains-libres, appel) ; `x26`, `x27` appel sortant, puis établi ; `x28` échec
    avec « Try Again » ; `x29`, `x30` bouton d'appel d'une conversation et carte préremplie ; `x31`,
    `x32` appels récents activables et carte préremplie ; `x33` le menu avec le moteur d'avant ; `x34`
    le menu en mode complet (mêmes boutons d'appel) ; `x35` le tableau réel du 17 novembre (lien LE
    absent, le reste actif, iPhone en français) ; `x36` démarrage en `notifications_off` ; `x37`, `x38`
    le parcours d'accueil en mode sobre (interrupteurs, fin) ; `x39` à `x41` le son d'un appel passé
    d'ici (« moving to this computer… », « Couldn’t bring the call sound to this computer », « The call
    didn’t start. » avec « Try Again ») ;
  - `ia-*` (style Phone) et `ib-*` (style Handset) : l'icône de la barre agrandie huit fois, état par
    état — `1-relie`, `2-absent`, `3-pastille-message`, `3-non-lu-film` (huit instants à la suite,
    empilés : la bulle saute), `3-non-lu-immobile` (après la minute), `4-sonne-film`,
    `4-sonne-sans-animation` (animations du bureau coupées), `5-appel-film`, `6-manque`,
    `7-repondeur-film`, `8-pause-film`, `9-link-iphone` ; `ia-9-bluetooth`, `ia-10-silence` ; chaque
    `-film` garde aussi ses instants un à un (`-1`, `-2`…) ; `i-barre-entiere` et `i-barre-droite`
    (alignement avec le Wi-Fi, le Bluetooth et la batterie) ; `i-menu-non-lu` (« 1 unread message »
    dans le menu).
- Icône animée : chaque état des deux styles a été vu en capture, et le mouvement constaté de deux
  façons : les poses relevées (position, angle, échelle, opacité de chaque calque) changent d'un
  relevé à l'autre, et les captures `-film` montrent des instants différents. Le Shell sans écran
  interdit les animations faute de carte graphique : le pilote lève cet interdit (`animations force`),
  ce qui n'existe pas dans une vraie session. **Écrit et couvert par les tests du modèle, non vu** : la
  composition en cours (même dessin que l'appel), l'icône sur la pastille à la couleur d'accent du
  système, le réglage « Icon » cliqué dans Phone Settings (c'est la clé `icon-style` que le pilote
  pose), l'écran à l'échelle 2, et la fluidité réelle : une capture ne dit rien de la cadence ni de la
  charge du processeur, qui n'ont pas été mesurées. Le répondeur n'a été joué que par le faux moteur.
- Son d'un appel passé d'ici : joué contre le faux moteur seulement (`son-appel`, `composition
  sans-appel`), jamais avec le moteur corrigé ni un vrai appel.
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
- Lien classique à la demande : tout a été joué contre le faux moteur seulement (`sobre`, `complet`,
  `protocole-ancien`). Vu en capture : chaque écran de `x01` à `x38`. **Écrit et couvert par les tests
  du modèle, mais non vu en capture** : « Notifications are reconnecting… » (`le_state:"waiting"`), les
  causes `unresolved_address` et `advert_off_air`, la carte d'appel en cours avec « This computer » grisé
  (appel pris sur l'iPhone, mains-libres pas encore là), l'échec `busy` / `calls_off` / `refused`, le
  message resté sans suite qui part en notification GNOME, la ligne de Phone Settings grisée (moteur à
  un seul mode) et son sous-titre quand le mode appliqué diffère. Le style clair a été joué une fois
  (`SCHEME=prefer-light`, 60 vérifications, captures hors dépôt) et six écrans regardés : menu, pause,
  carte d'appel en connexion et indisponible, carte « Call », tableau du LE absent. Dans
  Phone Settings, le choix n'a pas été cliqué : c'est la clé `link-mode` qui a été posée par le pilote,
  la liste déroulante a suivi (captures `x13`, `x14`).
- Passe 4, **non vu pour de vrai** : la connexion de l'iPhone par NetworkManager (`lib/hotspot.js` n'a
  tourné qu'avec la doublure du pilote et celle des tests : ni la recherche du profil, ni l'activation, ni
  le suivi d'état et des raisons d'échec par libnm n'ont été exécutés ; le profil réel a seulement été lu
  avec `nmcli`, sans rien activer) ; la détection d'un partage d'écran réel ; l'ouverture effective des
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
- Couleur : ambre par défaut pour la pastille et les boutons. La couleur d'accent du système est un
  réglage (« Phone Settings »), éteint par défaut ; l'icône au repos ne la suit jamais.
- Icône : sur la pastille ambre, l'icône est dessinée d'une seule encre sombre (la planche la montre
  sur fond noir, où la sonnerie n'apparaît en pratique que pastille déployée). Les bobines du style Phone
  font 2,5 px de diamètre à 16 px : on voit qu'il y a deux ronds, à peine qu'ils tournent.
- Le parcours d'accueil réunit les maquettes G (autorisation) et H (appairage) en une seule boîte, avec
  des étapes en plus : réparation, attente des liens, essai. Les étapes « autorisation » et « fin » n'ont
  pas de dessin d'iPhone : il n'y a rien à y faire sur le téléphone, et le bandeau le dit.
- Messages en bulles (la maquette montre le texte nu) : plusieurs messages à la suite restent lisibles.
- Pour un appel d'un numéro sans nom, la pastille montre le numéro seul, sans « is calling ».
- « Recent calls » : un clic sur une ligne ouvre la carte « Call » préremplie ; il ne compose pas.
- Notifications en pause : la barre ne porte pas de texte (largeur constante au repos), seulement
  l'anneau ambre sur l'icône ; les mots sont dans le menu et dans le nom lu par les lecteurs d'écran.
  L'anneau ne clignote qu'une minute (la planche ne fixait pas de durée) : une pause peut durer des
  heures, et la barre ne doit pas se redessiner sans fin.
- Batterie ancienne : « ~96 % » atténué, sans âge visible (la ligne du haut n'a pas la place) ; l'âge est
  lu par les lecteurs d'écran.
- La carte d'appel ne dit pas « Mobile » : le moteur ne donne pas le type de ligne.
- La notification d'un appel manqué (et celle d'un message vocal) va dans la liste de GNOME ; l'icône
  porte la flèche (les bobines) jusqu'à l'ouverture du menu.

## 6. Ce qui reste à faire

- Après « Fix It », rien ne dit si l'iPhone relit de lui-même les services proposés ou s'il faut
  couper puis rallumer son Bluetooth : à observer en vrai. Le texte dit seulement de rouvrir ⓘ.
- Pendant un silence, la pastille « Sending… » / « Sent » d'une réponse que l'utilisateur vient d'envoyer
  lui-même ne se montre pas non plus (règle « rien ne grossit » appliquée sans exception).
- Le verrou « Link again » compte les chutes de tous les liens à la fois ; un lien BR/EDR qui bat pendant
  que le LE tient n'est pas compté.
- « Open in … » ne préremplit aucun texte ; Discord, Slack, Teams et Messenger s'ouvrent sans la
  conversation (leurs liens ne se font pas par numéro) ; Instagram n'est pas au catalogue. Telegram par
  sa bibliothèque officielle (TDLib) : décision séparée (#49).
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
- Mode sobre : **rien n'a été essayé avec le moteur réel**, qui n'existait pas quand l'extension a été
  écrite. Les noms suivent l'étude 11 ; tout écart se corrige dans `lib/model/protocol.js`. À constater
  en vrai : la forme exacte de `bt_incoming_call` (`caller`, `since`), si `bt_call_dial` répond `OK` tout de
  suite ou seulement par `bt_call_result`, ce que vaut `messages` pendant une ouverture, et si le titre de
  la notification de Messages suffit à retrouver le fil (homonymes : la carte attend alors le message).
- Un envoi en cours ne s'annule pas (l'étude le souhaite, aucune commande n'existe pour cela).
- Hors mode sobre, avec un moteur récent en mode complet, la pastille se montre aussi dès la notification
  de Messages ; avec un moteur plus ancien elle attend `bt_message`, comme avant.
- La fusion notification / message compare les soixante premiers caractères : une pièce jointe (texte
  descriptif dans la notification, vide dans le message) donne deux traces.
- Appel pris sur l'iPhone avant le mains-libres : la carte reste sur « Answering on the iPhone… » jusqu'à
  ce que le moteur dise `ended` ou montre l'appel ; rien n'est supposé.
- Codes à usage unique (`otp_available`), second appel en attente, touches DTMF pendant un appel : non traités.
- Traductions (gettext prêt, aucun fichier `.po`).
- Fiche extensions.gnome.org : `shexli` signale l'accès au presse-papiers (bouton « Copy Commands », à
  déclarer dans la description) et juge la version 51 « implausible » (outil en retard).
