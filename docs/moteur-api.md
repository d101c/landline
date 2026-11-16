# Moteur : comment un client parle au démon

Lecture du code de `engine/` (Tether, commit `9ca3c46`). Tous les chemins sont relatifs à `engine/`.
Les numéros de ligne valent pour ce commit ; ils bougeront dès qu'on modifiera les fichiers.

**Mise à jour (branche `moteur-bluetooth-seul`).** Le texte décrit maintenant le moteur modifié : partie
réseau coupée par défaut, bulles et fenêtre de confirmation laissées au client, journal des appels. Les
numéros de ligne restent ceux du commit amont `9ca3c46` : ils situent le code, à quelques dizaines de lignes
près dans les fichiers modifiés (`net.cpp`, `pairing.cpp`, `connection.cpp`, `src/daemon/main.cpp`). Les
réglages de démarrage sont regroupés en section 11.

**Mise à jour (branche `moteur-quotidien`).** Trois sujets du quotidien : le son de l'iPhone reste sur
l'iPhone par défaut (section 9, commande et événement `bt_audio`), l'éloignement est publié
(section 7, `bt_presence`), la batterie faible est annoncée (section 3, `bt_battery_low`). L'étude, les
choix et les essais à faire avec l'iPhone sont dans `moteur-quotidien.md`.

La documentation amont du protocole est `docs/PROTOCOL.md`. Elle est juste mais incomplète : les commandes
d'appel (`bt_call_*`), `bt_scan`, `bt_solicit`, `bt_set_calls`, `bt_set_lock_on_away`, `bt_set_phone_audio`
et plusieurs événements n'y figurent pas. Ce document part du code.

## 1. En bref

- **Transport** : un socket Unix de type flux, `$XDG_RUNTIME_DIR/landline/landlined.sock`
  (soit `/run/user/1000/landline/landlined.sock`). Pas de D-Bus côté client.
- **Format** : une ligne = un objet JSON, terminée par `\n`. Le champ `command` nomme la commande ou
  l'événement.
- **Événements** : il existe un vrai flux. Le client envoie `{"command":"subscribe"}` une fois, puis reçoit
  tout changement d'état sans interroger. Aucune interrogation périodique n'est nécessaire.
- **Portée** : toutes les commandes `bt_*` ne sont acceptées que sur ce socket local. Rien de ce qui touche
  aux messages, contacts, notifications ou appels n'est joignable par le réseau.
- **Sécurité** : aucune authentification sur le socket. La protection est le dossier `landline/` en mode 0700
  dans le répertoire d'exécution de l'utilisateur (`src/core/src/net.cpp:1136-1159`).

Une extension GNOME Shell peut donc parler au moteur avec `Gio.SocketClient` + `Gio.UnixSocketAddress`,
lire ligne par ligne avec `Gio.DataInputStream.read_line_async()` et écrire du JSON. Aucun binaire
intermédiaire n'est requis.

## 2. Mécanique du socket

| Sujet | Comportement | Où |
|---|---|---|
| Chemin du socket | `get_runtime_dir() + "/landlined.sock"` ; sous Flatpak le dossier devient `app/<id>/landline` | `src/core/src/net.cpp:1136`, `:1190` |
| Instance unique | verrou `landlined.lock` dans le même dossier | `src/core/src/net.cpp:1169` |
| Acceptation | chaque connexion est enregistrée, non bloquante | `src/core/src/net.cpp:1259` |
| Découpage | tampon par client, découpé sur `\n` ; plus de 16 Mio sans `\n` = client expulsé | `src/core/src/net.cpp:1282-1300` |
| JSON invalide | ignoré, mais le démon répond quand même `OK\n` | `src/core/src/net.cpp:1861-1865` |
| Diffusion | `broadcast_local_event()` écrit à tous les abonnés | `src/core/src/net.cpp:296` |
| Démarrage automatique | le client C++ lance `landlined` si le socket ne répond pas, sauf `LANDLINE_NO_AUTOSTART=1` ou unité systemd activée | `src/core/src/client.cpp:91-166` |

### Trois formes de réponse

1. **Réponse directe** : une ligne JSON écrite au seul demandeur (les commandes de lecture : `bt_status`,
   `bt_list_*`, `bt_connection`…).
2. **Accusé `OK`** : la ligne littérale `OK\n`, **qui n'est pas du JSON**. C'est la réponse par défaut de
   toute commande qui ne répond pas directement (`src/core/src/net.cpp:1864`). Le vrai résultat arrive
   ensuite comme événement diffusé.
3. **Événement diffusé** : une ligne JSON envoyée à tous les abonnés, demandeur compris s'il est abonné.

### Pièges à connaître avant d'écrire le client

- **Pas d'identifiant de requête.** Réponses directes et événements arrivent sur le même flux, sans
  corrélation. Seules quatre commandes renvoient un `operation_id` fourni par le client : `bt_pair`,
  `bt_unpair`, `bt_send_message`, `send_file`. Le plus simple : traiter chaque ligne reçue selon son champ
  `command`, comme un événement, que ce soit une réponse ou une diffusion. Les réponses directes portent des
  noms distincts (`bt_threads`, `bt_messages`, `bt_contacts`…), donc cela fonctionne sans ambiguïté.
- **Ignorer les lignes `OK`.** Le client GTK amont fait exactement cela
  (`src/gtk/daemon_client.cpp:94`).
- **Le client C++ amont lit mal.** `Client::send_and_wait()` fait un seul `read()` d'au plus 1 Mio et suppose
  qu'il contient la réponse entière (`src/core/src/client.cpp:214-233`). Sur une connexion abonnée, il peut
  renvoyer un événement à la place de la réponse. Ne pas le prendre pour modèle : lire par lignes.
- **Toujours lire.** Les écritures du démon vers un client lent bouclent avec des pauses de 1 ms jusqu'à une
  limite, sous verrou (`src/core/src/net.cpp:255-275`, `:296`). Un client abonné qui cesse de lire ralentit
  tout le démon avant d'être abandonné.
- **Les commandes d'appel bloquent la boucle.** `bt_call_dial`, `bt_call_action` et `bt_call_tones`
  s'exécutent sur la boucle d'événements et attendent la pile téléphonie jusqu'à 10 s
  (`src/core/src/bluetooth/telephony.cpp:19`, `:195`). Pendant ce temps aucun client n'est servi.
- **Le démon n'affiche plus de bulles par défaut.** `desktop_popups_enabled` vaut maintenant `false`
  (`include/tether/bluetooth/config.hpp`) : le code libnotify (`src/daemon/notification.cpp`) reste en place
  mais ne montre rien. L'extension n'a rien à envoyer pour éviter les doublons. Pour retrouver les bulles
  du démon : `{"command":"set_desktop_popups","enabled":true}` (persistant, écrit dans `bluetooth.json`).
- **La confirmation d'appairage arrive au client.** Le démon ne lance plus `tether-dialog`. Voir la
  section 6.
- **Les commandes réseau sont refusées.** Partie réseau coupée (défaut), `accept_device`, `forget_device`,
  `pair_request`, `discover`, `send_file`, `file_start`, `file_chunk`, `file_end` reçoivent
  `{"command":"error","message":"network_disabled","request":"<commande>"}` (plus `operation_id` s'il était
  fourni). Voir la section 11.

### Abonnement

| Commande | Effet | Où |
|---|---|---|
| `{"command":"subscribe"}` | inscrit la connexion puis envoie aussitôt, dans cet ordre : `protocol_info`, `state_snapshot`, `otp_available` (s'il y a un code en attente), `bt_connection_changed` | `src/core/src/net.cpp:1307-1319` |
| `{"command":"unsubscribe"}` | désinscrit ; répond `{"command":"unsubscribed"}` | `src/core/src/net.cpp:1320` |
| `{"command":"protocol_info"}` | `{"command":"protocol_info","version":1,"capabilities":[…]}` ; `calls` n'y figure que si le contrôle d'appel est activé, `files` et `peers` que si la partie réseau est active ; `bluetooth.audio`, `bluetooth.presence` et `bluetooth.battery_low` disent que le moteur connaît `bt_audio`, `bt_presence` et `bt_battery_low` | `src/core/src/net.cpp:400`, `:1325` |
| `{"command":"state_snapshot"}` | état de la partie Wi-Fi (pairs TLS, fichiers reçus, pare-feu), plus `network_enabled` (bool). Sans intérêt pour le Bluetooth | `src/core/src/net.cpp:931`, `:1328` |

`subscribe` n'envoie **pas** `bt_status`, ni la liste des appareils, ni les notifications en cours. Séquence
de démarrage conseillée pour l'extension :

```
→ {"command":"subscribe"}
→ {"command":"bt_status"}
→ {"command":"bt_list_devices"}
→ {"command":"bt_list_notifications"}
→ {"command":"bt_list_calls"}
→ {"command":"bt_audio"}
→ {"command":"bt_presence"}
```

Puis ne plus rien demander : `bt_status` et `bt_devices` sont rediffusés à chaque changement vu par BlueZ
(`src/daemon/main.cpp:695-699`), `bt_connection_changed` à chaque changement réel de l'état publié.

Si la connexion tombe (démon arrêté), se reconnecter avec un délai ; le client GTK réessaie toutes les 2 s
(`src/gtk/daemon_client.cpp:48-51`).

## 3. État de connexion, services et batterie

### `bt_connection` → `bt_connection_changed`

C'est **le** message central pour la pastille.

- Requête : `{"command":"bt_connection"}` — réponse directe (`src/core/src/net.cpp:1706`).
- Événement : même objet, diffusé seulement quand il change (`src/core/src/bluetooth/connection.cpp:844-857`,
  branché en `src/daemon/main.cpp:274`). Le superviseur le recalcule toutes les secondes
  (`connection.cpp:1437-1438`) mais ne publie pas un contenu identique.
- Construction : `connection.cpp:859-878` puis `connection.cpp:832-842`.

| Champ | Type | Sens |
|---|---|---|
| `device_present` | bool | l'iPhone est connu de BlueZ |
| `device_paired` | bool | appairé |
| `classic_connected` | bool | lien BR/EDR actif (porte messages, contacts, appels) |
| `le_available` | bool | le lien a une moitié LE (sans elle, pas de notifications) |
| `le_connected` | bool | lien LE actif |
| `map_open` / `pbap_open` | bool | session messages / contacts ouverte (maintenu à vrai pendant le cycle mains-libres, voir « Réglages de reconnexion ») |
| `map_error` / `pbap_error` | chaîne | `none`, `forbidden` (réglage désactivé sur l'iPhone), `busy` (un autre ordinateur tient la session), `no_record`, `unavailable`, `no_daemon` (obexd absent), `bus_closed` (le moteur a perdu le bus de session, voir ci-dessous), `other` — `src/core/src/bluetooth/profile_supervisor.cpp:26-72` |
| `map_held` / `pbap_held` / `ancs_held` | bool | service relâché volontairement sur cet ordinateur |
| `ancs_ready` | bool | notifications actives |
| `ancs_reason` | chaîne | phrase d'état des notifications |
| `link_reason` / `profile_reason` | chaîne | phrases prêtes à afficher (en anglais, traduites par gettext) |
| `bond_state` | chaîne | santé de l'appairage : `none`, `ok`, `partial`, `broken` — voir « Santé de l'appairage » ci-dessous |
| `bond_detail` | chaîne | précision : `""`, `bredr_only`, `le_only`, `auth_failures`, `key_missing` |
| `needs_repair` | bool | l'appairage est à retirer des deux côtés et à refaire ; **ne pas afficher « connecté »** tant qu'il est vrai |
| `battery` | entier ou `null` | pourcentage de batterie de l'iPhone |
| `battery_source` | chaîne ou `null` | d'où vient `battery` : `le` (service Batterie, pas de 1 %) ou `hfp` (indicateur mains-libres, pas de 20 %) |
| `battery_state` | chaîne ou `null` | `ok`, `low` (20 % ou moins), `critical` (10 % ou moins) |
| `calls` | objet ou `null` | état de la téléphonie, voir section 8 ; `null` si le contrôle d'appel est désactivé |

### Batterie et état du téléphone

Ce que le moteur sait de l'iPhone, et d'où (`phone_battery()`, `connection.cpp` ;
`src/core/include/tether/bluetooth/phone_state.hpp`) :

| Information | Champ | Source | Disponible sur cette machine |
|---|---|---|---|
| Niveau de batterie | `battery`, `battery_source`, `battery_state` | service Batterie LE lu par BlueZ (`org.bluez.Battery1.Percentage`), sinon indicateur mains-libres `BattChg` (0 à 5, publié × 20) | oui par le LE, dès que le lien LE est monté |
| En charge ou non | — | aucune : `Battery1` ne porte que le pourcentage, le mains-libres n'a pas d'indicateur de charge, et l'iPhone n'expose pas la caractéristique « Battery Level Status » | **non**, et rien à activer |
| Opérateur | `calls.operator` | mains-libres de BlueZ (`org.bluez.Telephony1.OperatorName`) | non avec PipeWire (`calls.indicators:false`) |
| Force du signal | `calls.signal` (0 à 5) | idem (`Signal`) | non avec PipeWire |
| Réseau disponible | `calls.service` | idem (`Service`) | non avec PipeWire |
| Itinérance | `calls.roaming` | idem (`Roaming`) | non avec PipeWire |
| Appels par Wi-Fi | `calls.wifi_calling` | **lu dans le nom du réseau** : vrai si `operator` contient « wifi call » à la casse et à la ponctuation près (« WiFi-Calling », « Wi-Fi Call »). Il n'existe pas d'indicateur mains-libres pour cela | non avec PipeWire ; hypothèse à vérifier avec BlueZ |

**Fréquence.** Rien n'est interrogé auprès de l'iPhone pour ces valeurs. BlueZ reçoit les notifications
du service Batterie et les indicateurs mains-libres et met ses propriétés à jour ; le superviseur relit
l'instantané de BlueZ **chaque seconde** et ne publie `bt_connection_changed` que si l'objet a changé. La
batterie arrive donc à chaque changement d'un point (source `le`), opérateur et signal à chaque
changement annoncé par l'iPhone (source BlueZ). Lien LE tombé : `battery` repasse à `null`.

Avec la téléphonie PipeWire (cette machine, WirePlumber d'usine), `calls.indicators` est faux :
`operator`, `signal`, `battery`, `service`, `roaming` de l'objet `calls` restent à leur valeur nulle et ne
doivent pas être affichés. Les obtenir demande de rendre le mains-libres à BlueZ (retirer `hfp_hf` de
`bluez5.roles`, réglage global de WirePlumber) : le moteur ne le fait pas. Voir `moteur-quotidien.md`.

#### Événement `bt_battery_low`

```json
{"command":"bt_battery_low","level":20,"threshold":20,"state":"low","source":"le","initial":false}
```

| Champ | Type | Sens |
|---|---|---|
| `threshold` | entier | le seuil franchi : `20` ou `10` |
| `level` | entier | le pourcentage lu à ce moment (inférieur ou égal au seuil) |
| `state` | chaîne | `low` ou `critical`, comme `battery_state` |
| `source` | chaîne | `le` ou `hfp` |
| `initial` | bool | vrai si c'est la **première** valeur lue depuis le démarrage du moteur : la batterie était déjà basse, aucun franchissement n'a été vu. À l'extension de décider si elle notifie |

Règles (`BatteryWatch`) :

- un événement quand le niveau atteint **20 % ou moins**, un autre quand il atteint **10 % ou moins**, une
  seule fois par franchissement ; une chute qui traverse les deux d'un coup n'annonce que `10` ;
- un seuil ne peut être annoncé de nouveau qu'après une remontée de 5 points au-dessus de lui : 25 %
  réarme tout, 15 % réarme le seuil de 10 %. Un niveau qui oscille entre 20 et 21 % n'annonce qu'une fois ;
- un niveau inconnu (iPhone parti) n'annonce rien et ne réarme rien : un iPhone qui revient au même
  niveau ne redéclenche pas ;
- l'état est gardé tant que le moteur tourne et que l'iPhone choisi ne change pas ; un redémarrage du
  moteur le remet à zéro (d'où `initial`) ;
- avec la source `hfp` (pas de 20 %), seul le seuil de 20 % peut être atteint ;
- il n'y a pas de commande de lecture : l'état courant se lit dans `battery_state`. Les seuils sont des
  constantes (`BATTERY_LOW_PERCENT`, `BATTERY_CRITICAL_PERCENT`, `BATTERY_REARM_MARGIN`).

Quand aucun iPhone n'est choisi, l'objet se réduit à `device_present:false`, `device_paired:false`,
`link_reason`, `profile_reason`, `bond_state:"none"`, `bond_detail:""`, `needs_repair:false`
(`ConnectionManager::status()`, `connection.cpp`).

### Motif `bus_closed`

`map_error` et `pbap_error` valent `bus_closed` quand la connexion du moteur au bus D-Bus de session est
fermée (le bus a redémarré sous lui) ou que le bus est introuvable : obexd est injoignable, quoi que
l'iPhone autorise. `map_open` et `pbap_open` passent à faux dans le même message, et `profile_reason` le dit
en clair. Rien n'est à régler sur l'iPhone : **ne pas afficher l'aide aux interrupteurs** sur ce motif.

- Connexion fermée : le moteur écrit la cause dans son journal et **sort avec le code 75** environ 2 s plus
  tard ; systemd le relance (`Restart=on-failure`). Le client voit donc `bus_closed`, puis son socket se
  fermer, puis un moteur neuf : se reconnecter comme d'habitude. Même sortie si c'est le bus **système**
  (BlueZ) qui se ferme, sans motif publié.
- Bus introuvable au démarrage : le moteur reste en vie, publie `bus_closed` et reprend seul dès que le bus
  existe.
- `LANDLINE_BUS_WATCH=0` : le moteur ne sort plus ; `bus_closed` reste publié indéfiniment.

Détail : `moteur-reconnexion.md`, 9.1.

### Santé de l'appairage (`bond_state`, `bond_detail`, `needs_repair`)

Les trois mêmes champs figurent dans `bt_connection_changed`, `bt_status` et `bt_pair_result`. Ils viennent
de deux sources (`src/core/include/tether/bluetooth/bond_health.hpp`) :

- **ce que BlueZ dit des clés**, lu au premier tour de supervision et dans `bt_status` sans attendre un
  échec : `Device1.Paired`, `Bearer.LE1.Bonded`, `Bearer.BREDR1.Bonded` (`assess_bond()`) ;
- **ce que le lien fait en marche** : coupures `org.bluez.Reason.Authentication` et refus de connexion
  « key-missing » (`AuthFailureTracker`, branché dans `BearerSupervisor`).

| `bond_state` | Quand | `bond_detail` | `needs_repair` |
|---|---|---|---|
| `none` | rien d'appairé de ce côté | `""` | `false` |
| `ok` | les deux moitiés, ou `Bearer.LE1` non renseigné (BlueZ sans API expérimentale : impossible à juger) | `""` | `false` |
| `partial` | `Bearer.LE1` renseigné et `Bonded:false`, sans abonnement ANCS actif | `bredr_only` | `true` si la machine peut porter le LE (`capability.mode == "full"`), sinon `false` |
| `partial` | LE appairé mais `Bearer.BREDR1.Paired:false` (signalé, non condamné : cette interface n'est pas renseignée de façon fiable) | `le_only` | `false` |
| `broken` | 5 coupures `Reason.Authentication` en 120 s | `auth_failures` | `true` |
| `broken` | 2 refus de connexion BR/EDR « key-missing » sans lien entre les deux | `key_missing` | `true` |

`broken` l'emporte sur `partial`. Il ne retombe que si le lien LE tient 60 s d'affilée (un lien qui échoue
à s'authentifier est coupé en quelques secondes), ou quand l'appareil disparaît de BlueZ (`bt_unpair`,
`bt_pair` avec `fresh`). Tant qu'il dure, le moteur **se tient à l'écart** : plus de connexion BR/EDR, plus
de composition LE, plus de cycle mains-libres, plus d'annonce LE, plus d'ouverture MAP/PBAP (une session
déjà ouverte est laissée). Une fenêtre d'essai de 30 s s'ouvre toutes les 10 minutes ; un nouveau refus la
referme aussitôt. `link_reason` porte alors la phrase « The pairing with the iPhone no longer works… ».
Une ligne de journal, une seule fois : `bluetooth: the bond with the iPhone is broken: 5 authentication
failures within 120s. Standing back…`.

Constantes : `AUTH_FAILURES_BEFORE_BROKEN` (5), `AUTH_FAILURE_WINDOW_SECONDS` (120),
`KEY_MISSING_BEFORE_BROKEN` (2), `BOND_RECOVERY_SECONDS` (60), `BROKEN_BOND_RETRY_SECONDS` (600),
`BROKEN_BOND_PROBE_SECONDS` (30).

### `bt_status`

- Requête : `{"command":"bt_status"}` — réponse directe (`src/core/src/net.cpp:1332`).
- Aussi diffusé après tout changement de réglage et à chaque changement BlueZ.
- Construction : `src/core/src/net.cpp:420-471`.

Champs : `available`, `version`, `device_address` (l'iPhone choisi), `adapter`, `enabled`, `ancs_enabled`,
`ancs_content_enabled`, `calls_enabled`, `retention`, `retention_ready`, `desktop_popups_enabled` (faux par
défaut), `pair_dialog_enabled` (faux par défaut), `popup_previews_enabled`, `clipboard_auto_share`, `hide_unknown_senders`, `muted_apps` (tableau de
`{app_id, app_name}`), `airpods_enabled`, `airpods_pause`, `airpods_handoff`, `lock_on_away`,
`lock_away_seconds`, `wake_on_return`, `yield_when_inactive`, `contacts_by_last_name`, `phone_audio`,
`phone_audio_route`, `phone_audio_rule`, `call_audio`,
`apple_device_id`, `adapters` (tableau), `capability`, et la santé de l'appairage : `bond_state`,
`bond_detail`, `needs_repair` (mêmes valeurs que ci-dessus), plus **`bond_address`** — l'adresse dont
parlent ces trois champs. C'est l'iPhone choisi ; quand aucun ne l'est, c'est le premier iPhone que BlueZ
tient déjà pour appairé. Un appairage créé hors du moteur (demande venue de l'iPhone, acceptée par un autre
agent Bluetooth) est donc visible dès le démarrage, avant tout `bt_pair` : `bond_state:"partial"`,
`bond_detail:"bredr_only"`, `needs_repair:true` (`report_bond()`, `objects.cpp` ; `put_bt_bond()`,
`net.cpp`). Si le superviseur a constaté `broken` pour cette adresse, c'est `broken` qui est publié.

`capability` (`src/core/src/bluetooth/objects.cpp:789-812`) décrit ce que la machine sait faire :
`mode` (`full`, `compatibility`, `blocked`), `bearer_api`, `adapter_present`, `powered`, `le_central`,
`le_peripheral`, `advertising`, `class_ok`, `bonded_device_present`, `bond_has_le`, `bond_carries_ancs`,
`secure_connections`, `ertm_disabled`, `adapter_id`, `reasons` (phrases), et **`setup`** : la liste des
prérequis système manquants, chacun avec une phrase et la commande à lancer. C'est ce que lit
`landlinectl bt setup`. Voir `moteur-inventaire.md`, section 5.

**Classe d'appareil (passe 4).** `class_ok` reste publié (vrai si l'adaptateur porte la classe
mains-libres `0x..0408`), mais **pour information seulement** : la classe n'est plus une étape de `setup`,
ne figure pas dans `reasons` et ne bloque rien. `setup` ne peut donc plus contenir que deux étapes : le
mode expérimental de BlueZ et l'ERTM. Constat du 16 novembre 2026 : un iPhone sous iOS 26 appairé avec la
classe d'origine (`0x7c0000`) propose et accorde messages, contacts, notifications et appels. `landlinectl
--bt-status` écrit `class=hands-free` ou `class=default` au lieu de `ok` / `wrong`.

### `bt_list_devices` → `bt_devices`

- Requête : `{"command":"bt_list_devices"}` — réponse directe (`src/core/src/net.cpp:1378`).
- Diffusé à chaque changement BlueZ et pendant un balayage.
- Par appareil (`src/core/src/bluetooth/objects.cpp:756-780`) : `path`, `adapter`, `address`, `name`,
  `paired`, `bonded`, `trusted`, `connected`, `classic_connected`, `le_bearer`, `le_bonded`, `le_connected`,
  `preferred_bearer`, `map`, `pbap`, `ancs`, `ancs_notifying`, `services_resolved`, `iphone`,
  `apple_nearby`, `airpods`, `classic_bonded` (bool, ou `null` si `Bearer.BREDR1` ne le dit pas),
  `bond_state`, `bond_detail` (lecture statique seulement, sans `broken`).

`iphone` et `apple_nearby` servent à repérer un iPhone candidat avant appairage.

### Autres commandes d'état

| Commande | Paramètres | Réponse | Où |
|---|---|---|---|
| `bt_scan` | — | `OK`, puis `bt_devices` chaque seconde pendant 20 s, puis `bt_scan_result` `{success, message}` | `src/core/src/net.cpp:621-645`, `:1336` |
| `bt_set_device` | `address` | `OK`, puis `bt_status` ; choisit un iPhone déjà appairé et relance la supervision | `src/core/src/net.cpp:1412` |
| `bt_set_enabled` | `enabled` | `OK`, puis `bt_status` ; met la supervision en pause | `src/core/src/net.cpp:1515` |
| `bt_set_adapter` | `adapter` (`hci1` ou adresse) | `OK`, puis `bt_status` | `src/core/src/net.cpp:1508` |
| `bt_hold` | `profile` (`map`, `pbap`, `ancs`, `all`), `held` | `OK`, puis `bt_status` ; non persistant | `src/core/src/net.cpp:1579`, `connection.cpp:1626` |
| `bt_set_yield_when_inactive` | `enabled` | `OK`, puis `bt_status` | `src/core/src/net.cpp:1548` |
| `bt_retry_profiles` | — | `OK`, puis `bt_connection_changed` si quelque chose a bougé ; voir ci-dessous | `src/core/src/net.cpp`, `ConnectionManager::retry_profiles()` |
| `bt_diagnostics` | — | réponse directe : rapport anonymisé avec chronologie des transitions | `src/core/src/net.cpp:1710`, `src/core/src/bluetooth/diagnostics.cpp:269` |

#### `bt_retry_profiles` : « j'ai activé l'interrupteur »

`{"command":"bt_retry_profiles"}` demande au moteur d'essayer **tout de suite** d'ouvrir les sessions
messages et contacts encore fermées et de rechercher le service de notifications, sans attendre ses
échéances. À envoyer quand l'utilisateur a des chances d'être en train de toucher aux interrupteurs de
Réglages › Bluetooth › ⓘ : ouverture du menu, écran d'aide, bouton « Réessayer ».

- Réponse : `OK`. Le résultat arrive par `bt_connection_changed` (`map_open`, `pbap_open`, `ancs_ready`),
  **seulement s'il change** ; un profil toujours refusé ne produit aucun message.
- Sans effet sur un profil relâché (`map_held`, `pbap_held`) ni quand aucun iPhone n'est supervisé.
- L'envoyer n'est pas nécessaire pour être réactif : tant qu'un profil est en `forbidden` et que le lien
  BR/EDR est monté, le moteur le réessaie seul toutes les 2 s pendant 5 min (à partir du premier refus, de
  l'ouverture de l'autre profil, ou de cette commande), puis toutes les 15 s. La commande relance ces
  5 min. Ne pas l'envoyer en boucle : une fois par geste de l'utilisateur suffit.
- Une session qui s'ouvre est publiée aussitôt, avant la lecture du carnet ou des messages.

Notifications : le moteur voit « Partager les notifications système » en 1 à 2 s pendant les 5 min qui
suivent la montée du lien, en 1 à 5 s ensuite. Détail : `moteur-reconnexion.md`, 9.2 et 9.3.

## 4. Notifications (ANCS)

| Message | Sens | Paramètres | Forme | Où |
|---|---|---|---|---|
| `bt_notification` | événement | — | une notification, voir ci-dessous | `src/daemon/main.cpp:738-740` |
| `bt_notification_removed` | événement | — | `{uid}` : retirée sur l'iPhone | `src/daemon/main.cpp:762-768` |
| `bt_list_notifications` | requête | — | réponse directe `bt_notifications` `{notifications:[…]}` | `src/core/src/net.cpp:1684`, `connection.cpp:1604` |
| `bt_notification_action` | requête | `uid` (entier), `action` (`positive` ou `negative`) | `OK`, puis `bt_notification_action_result` `{uid, success}` | `src/core/src/net.cpp:1693` |
| `bt_mute_app` | requête | `app_id`, `app_name`, `muted` | `OK`, puis `bt_status` (`muted_apps`) | `src/core/src/net.cpp:485`, `:1607` |
| `bt_set_ancs` | requête | `enabled` | `OK`, puis `bt_status` | `src/core/src/net.cpp:1497` |
| `bt_set_ancs_content` | requête | `enabled` | `OK`, puis `bt_status` | `src/core/src/net.cpp:1520` |
| `bt_solicit` | requête | — | `OK`, puis `bt_solicit_result` `{success, message}` ; remet l'annonce en ondes | `src/core/src/net.cpp:1465` |
| `set_desktop_popups` | requête | `enabled` | `OK`, puis `bt_status` ; rallume ou coupe les bulles du démon (coupées par défaut) | `src/core/src/net.cpp:1583` |
| `set_popup_previews` | requête | `enabled` | `OK`, puis `bt_status` | `src/core/src/net.cpp:1589` |

Une notification (`src/core/src/bluetooth/ancs/notifications.cpp:238-252`) :

```json
{"command":"bt_notification","uid":42,"app_id":"net.whatsapp.WhatsApp","app_name":"WhatsApp",
 "title":"…","subtitle":"…","body":"…","category":4,"timestamp":1794830000,
 "silent":false,"positive_action":true,"negative_action":true}
```

- `category` suit la numérotation ANCS : 0 autre, 1 appel entrant, 2 appel manqué, 3 messagerie vocale,
  4 social, 5 agenda, 6 courriel, 7 actualités, 8 santé, 9 finance, 10 lieu, 11 divertissement.
- `silent` est vrai pour une notification déjà présente sur l'iPhone à la connexion : à lister, pas à faire
  surgir. Le démon applique la même règle quand ses bulles sont rallumées (`src/daemon/main.cpp:744-746`).
- `app_name` est d'abord déduit de l'identifiant puis corrigé quand l'iPhone répond : la même notification
  peut être rediffusée avec le bon nom.
- Les notifications de Messages (`com.apple.MobileSMS`) arrivent ici aussi, mais le démon ne les fait pas
  surgir : la copie affichable est `bt_message` (section 5).
- ANCS ne propose que deux actions, sans réponse en texte libre ni lien profond.
- Les codes à usage unique repérés dans les notifications et les messages sont diffusés par `otp_available`
  `{otp, otp_id, sender_domain}` (`src/core/src/net.cpp:160-177`).

## 5. Messages et contacts (MAP, PBAP)

| Message | Sens | Paramètres | Forme | Où |
|---|---|---|---|---|
| `bt_list_threads` | requête | — | réponse directe `bt_threads` `{threads:[…]}` | `src/core/src/net.cpp:811-859`, `:1417` |
| `bt_list_messages` | requête | `thread` | réponse directe `bt_messages` `{thread, messages:[…]}`, 200 au plus | `src/core/src/net.cpp:861-880`, `:1428` |
| `bt_message` | événement | — | un message nouveau, reçu ou envoyé d'ici | `src/daemon/main.cpp:275-278`, `src/core/src/net.cpp:668-670` |
| `bt_send_message` | requête | `thread`, `body`, `operation_id` (facultatif) | `OK`, puis `bt_send_result` `{thread, success, message?, operation_id?}` et, si réussi, `bt_message` | `src/core/src/net.cpp:652-678`, `:1460`, `connection.cpp:86-155` |
| `bt_mark_read` | requête | `handle` ou `handles` (tableau), `read` | `OK`, puis `bt_message_read` `{handles, read, success, synced, message?}` | `src/core/src/net.cpp:1432`, `connection.cpp:260-288` |
| `bt_remove_thread` | requête | `thread`, `before` (entier) | `OK`, puis `bt_threads` ; masque localement, ne supprime rien sur l'iPhone | `src/core/src/net.cpp:1443` |
| `set_hide_unknown_senders` | requête | `enabled` | `OK`, puis `bt_status` et `bt_threads` | `src/core/src/net.cpp:1601` |
| `bt_list_contacts` | requête | `query` (facultatif), `limit` (100 par défaut) | réponse directe `bt_contacts` `{query, contacts:[{name, favorite, addresses:[…]}]}` | `src/core/src/net.cpp:882-911`, `:1421` |
| `bt_set_contacts_by_last_name` | requête | `enabled` | `OK`, puis `bt_status` | `src/core/src/net.cpp:1555` |
| `bt_set_retention` | requête | `retention` (`encrypted`, `plaintext`, `none`) | `OK`, puis `bt_status` | `src/core/src/net.cpp:1663` |

Formes :

- Fil (`src/core/src/bluetooth/messages.cpp:236-246`, complété en `net.cpp:823-855`) : `thread` (la clé),
  `name`, `address`, `preview`, `timestamp`, `unread`, `count`, `group`, `repliable`, et pour un groupe
  `reply_status`, `reply_reason`.
- Message (`src/core/src/bluetooth/messages.cpp:222-234`) : `handle`, `thread`, `address`, `name`, `body`,
  `timestamp`, `outgoing`, `read`, `folder`.
- Clé de fil : `tel:+41791234567` pour un numéro, `email:…` ou l'adresse en minuscules pour un identifiant
  Apple, `group:name:…` / `group:members:…` pour un groupe. Les adresses rendues par `bt_list_contacts`
  ont la même forme et se passent telles quelles comme `thread` à `bt_send_message`.

Points à savoir :

- **Les messages ne sont pas poussés par l'iPhone.** Le démon relit la boîte toutes les 15 s
  (`MESSAGE_POLL_SECONDS`, `connection.cpp:309`, `:929-931`). Un message reçu apparaît donc avec jusqu'à 15 s
  de retard ; la notification ANCS correspondante arrive plus tôt. Côté client, en revanche, tout est
  événement : pas d'interrogation à faire.
- La première lecture d'une session renvoie tout l'historique : ces messages sont diffusés en `bt_message`
  mais sans bulle (`backfill`, `connection.cpp:1097-1107`). L'événement ne porte pas ce drapeau : l'extension
  doit ne faire surgir que les messages `read:false`, `outgoing:false` et récents.
- Un envoi réussi n'a pas d'autre trace que `bt_message` : le dossier « envoyés » de l'iPhone reste vide.
- La liste des contacts ne se lit que par `bt_list_contacts` : le cache sur disque est chiffré.
- Il n'y a pas d'événement « contacts mis à jour ». Les contacts sont relus à chaque nouvelle session PBAP
  (`connection.cpp:1112-1150`).

## 6. Appairage

| Message | Sens | Paramètres | Forme | Où |
|---|---|---|---|---|
| `bt_pair` | requête | `address`, `operation_id` (facultatif), `strategy` (facultatif), **`fresh`** (bool, facultatif, faux par défaut) | `OK`, puis événements ci-dessous | `run_bt_pair()`, `src/core/src/net.cpp` |
| `bt_pair_progress` | événement | — | `{step, detail, operation_id?}` | `src/core/src/net.cpp:732-739` |
| `bt_pair_confirm_request` | événement | — | `{code, address, timeout_seconds, operation_id?}` : code à six chiffres à comparer | `build_pair_confirm_request()`, `src/core/src/net.cpp:681-700` |
| `bt_pair_confirm` | requête | `accept` (bool), `operation_id` | `OK` ; délai de réponse 60 s | `src/core/src/net.cpp:609`, `:1393` |
| `bt_pair_result` | événement | — | `{success, status, reason, message, address, dual_bond, auth_strategy_used, bond_state, bond_detail, needs_repair, operation_id?}` | `to_json(PairResult)`, `src/core/src/bluetooth/pairing.cpp` |
| `bt_unpair` | requête | `address`, `operation_id` | `OK`, puis `bt_unpair_result` : même forme, plus `removed` et `selection_cleared` — voir « Retirer l'appairage » | `run_bt_unpair()`, `src/core/src/net.cpp` |

Étapes (`step`) émises par `pair_device()` (`src/core/src/bluetooth/pairing.cpp:582-850`), dans l'ordre
habituel :

| `step` | Sens | Ligne |
|---|---|---|
| `discovering` | l'iPhone n'est pas encore vu, balayage | 603 |
| `already_paired` | déjà appairé **et** appairage sain : rien à faire | 618 |
| `stale_bond` | déjà appairé mais appairage partiel ou cassé ; `detail` = `bond_detail`. Aucune autre étape ne suit | — |
| `removing_bond` | `fresh` : l'appairage existant est retiré de BlueZ ; suivi de `discovering` | — |
| `warning` | annonce impossible. (La classe d'appareil ne donne plus d'avertissement : une ligne `INFO` du journal, « pairing with the adapter's own device class », rien vers le client) | 834 |
| `connecting` | tentative « connexion d'abord » (celle qui donne aussi la moitié LE) | 719 |
| `pairing` | tentative « appairage explicite » (repli : pas de notifications) | 722 |
| `confirm` | `detail` = le code à comparer | 646 |
| `confirmed` / `declined` / `unanswered` | réponse ; `detail` = `client` (ou `dialog` si `pair_dialog_enabled`) | 663 |
| `rediscovering` | l'appareil a disparu de BlueZ, nouveau balayage | 700 |
| `retrying` | l'iPhone a refusé, nouvelle tentative ou repli | 749, 755 |
| `error` | `detail` = message BlueZ | 714, 727 |
| `paired` | lien créé | 797 |
| `settling` | attente de 3 s que le lien BR/EDR se pose | 811 |
| `soliciting` | annonce en ondes ; `detail` dit d'activer les réglages sur l'iPhone | 829 |

`status` final : `paired`, `already_paired`, `stale_bond`, `not_found`, `rejected`, `timeout`, `error`,
`busy` (un autre appairage est en cours). **`reason`** est le motif stable de l'échec : chaîne vide en cas
de succès, égal à `status` sinon. **`dual_bond`** dit si le lien couvre aussi le LE : sans lui, messages et
contacts fonctionnent mais pas les notifications. `bond_state`, `bond_detail`, `needs_repair` donnent la
santé de l'appairage à la fin de l'opération.

### `bt_pair` sur un appareil déjà appairé

Avant, un appareil déjà appairé donnait toujours `already_paired` puis `settling`, `soliciting` et un
succès, sans code — y compris pour un appairage partiel créé hors du moteur. Désormais
(`plan_existing_bond()`, `pairing.hpp`) :

| Appairage existant | `fresh` | Ce qui se passe |
|---|---|---|
| sain (`needs_repair:false`) | absent | `already_paired`, comme avant |
| partiel ou cassé (`needs_repair:true`) | absent | étape `stale_bond`, puis `bt_pair_result` `{success:false, status:"stale_bond", reason:"stale_bond", bond_state, bond_detail, needs_repair:true}`. Rien n'est modifié : ni BlueZ, ni la sélection, ni la supervision |
| quelconque | `true` | étape `removing_bond`, `Adapter1.RemoveDevice`, étape `discovering` (balayage de 30 s au plus), puis l'appairage normal avec code |
| aucun | `true` | l'appairage normal ; `fresh` est sans effet |

Avec `fresh`, l'appareil disparaît de BlueZ et doit être redécouvert : l'iPhone doit être déverrouillé,
Réglages > Bluetooth ouvert, et l'ordinateur oublié sur l'iPhone. Si la redécouverte échoue, le résultat
est `not_found` et l'ancien appairage est **déjà retiré** (la sélection reste ; `bond_state` passe à
`none`). Si `RemoveDevice` échoue : `error`, rien n'a changé.

L'état `broken` constaté par le superviseur est transmis à `bt_pair` : un appairage dont les clés semblent
complètes mais que l'iPhone refuse donne aussi `stale_bond`.

### Retirer l'appairage : `bt_unpair`

`{"command":"bt_unpair","address":"…","operation_id":"…"}` → `OK`, puis `bt_unpair_result`
`{success, status, reason, message, address, removed, selection_cleared, bond_state:"none", …}`.

1. `Adapter1.RemoveDevice` : l'objet BlueZ et toutes ses clés (`removed:true`).
2. Si l'adresse est l'iPhone choisi (`selection_cleared:true`) : `device_address` est vidé dans la
   configuration, la supervision s'arrête — sessions MAP et PBAP fermées côté obexd, client ANCS et client
   d'appels libérés, routage audio du téléphone remis au repos — et l'annonce LE est retirée.
3. Diffusion de `bt_unpair_result`, de `bt_status`, puis d'un `bt_connection_changed` réduit
   (`device_present:false`, `bond_state:"none"`).

`status` : `unpaired` ; `not_found` (inconnu de BlueZ **et** non choisi) ; `error` (BlueZ a refusé : la
sélection est gardée). Un iPhone que BlueZ ne connaît plus mais qui est encore choisi donne `unpaired`,
`removed:false`, `selection_cleared:true`. Les messages et le carnet gardés en mémoire ne sont pas effacés
par cette commande. L'iPhone garde sa propre fiche : elle se retire à la main (« Oublier cet appareil »).

Après un succès, le démon enregistre l'adresse, active la supervision et diffuse `bt_status`
(`net.cpp:743-763`).

### Confirmation du code : ce que le client reçoit et renvoie

Le démon ne lance plus `tether-dialog`. Quand l'iPhone affiche son code, **tous les clients abonnés**
reçoivent, dans cet ordre :

```json
{"command":"bt_pair_progress","step":"confirm","detail":"482913","operation_id":"pair-1"}
{"address":"AA:BB:CC:DD:EE:FF","code":"482913","command":"bt_pair_confirm_request","operation_id":"pair-1","timeout_seconds":60}
```

| Champ de `bt_pair_confirm_request` | Type | Sens |
|---|---|---|
| `code` | chaîne | les six chiffres à comparer avec ceux affichés sur l'iPhone |
| `address` | chaîne | l'adresse passée à `bt_pair`, telle que le client l'a écrite |
| `timeout_seconds` | entier | délai de réponse, 60 ; passé ce délai le démon refuse |
| `operation_id` | chaîne | celui de `bt_pair` ; **absent** si `bt_pair` n'en portait pas |

Le client répond par une seule ligne :

```json
{"command":"bt_pair_confirm","accept":true,"operation_id":"pair-1"}
```

- `accept` : `true` si l'utilisateur confirme que les codes sont identiques, `false` sinon. Champ absent =
  refus.
- `operation_id` : recopier celui de la demande. Si la demande en portait un, une réponse qui en porte un
  autre (ou aucun) est **ignorée** ; si la demande n'en portait pas, toute réponse est acceptée.
- Le démon accuse réception par `OK`, puis diffuse `bt_pair_progress` avec `step` = `confirmed` ou
  `declined` et `detail` = `client`, et plus tard `bt_pair_result`.
- Sans réponse en 60 s : `declined`, puis `bt_pair_result` en échec. Il n'y a pas de message d'annulation :
  si l'utilisateur ferme la fenêtre, envoyer `accept:false`.
- Si plusieurs clients sont abonnés, tous reçoivent la demande et la première réponse valable l'emporte.
- Si aucun client n'est abonné, personne ne répond et l'appairage échoue au bout de 60 s : l'extension doit
  être abonnée **avant** d'envoyer `bt_pair`.

Code : `resolve_confirmation()` (`src/core/src/bluetooth/pairing.cpp`), `ask_client_to_confirm()` et
`build_pair_confirm_request()` (`src/core/src/net.cpp`). L'ancien ordre (fenêtre `tether-dialog` d'abord,
client en repli) revient avec `"pair_dialog_enabled": true` dans `bluetooth.json`.

Après l'appairage, l'utilisateur doit activer sur l'iPhone, dans Réglages > Bluetooth > (i) : « Afficher
les notifications de message », « Synchroniser les contacts » et « Partager les notifications système »
(libellés relevés sur iOS 26 ; l'ordre de la fiche est dans `extension.md`, « Illustrations »). L'état se lit
dans `bt_connection_changed` : `map_error:"forbidden"` tant que le premier manque, `ancs_ready:false` tant
que le dernier manque.

## 7. Éloignement : présence et verrouillage

| Message | Sens | Paramètres | Forme | Où |
|---|---|---|---|---|
| `bt_presence` | événement | — | l'iPhone est parti ou revenu, voir ci-dessous | `PresenceTracker`, `src/core/include/tether/bluetooth/presence.hpp` ; `supervise_away_lock()`, `connection.cpp` |
| `bt_presence` | requête | — | réponse directe : le dernier `bt_presence` publié, ou `state:"unknown"` | `src/core/src/net.cpp` |
| `bt_set_lock_on_away` | requête | `enabled` (bool), `grace_seconds` (entier, 1 à 3600), `wake_on_return` (bool) — **tous facultatifs** | `OK`, puis `bt_status` (`lock_on_away`, `lock_away_seconds`, `wake_on_return`) | `apply_lock_on_away()`, `presence.hpp` |

Un champ absent de `bt_set_lock_on_away` garde sa valeur. (Avant, `{"enabled":true}` seul remettait le
délai à 30 s.) Valeurs par défaut : `lock_on_away:false`, `lock_away_seconds:30`, `wake_on_return:false`.

### Événement `bt_presence`

```json
{"command":"bt_presence","state":"away","reason":"out_of_range","away_seconds":3,
 "lock":"pending","lock_in_seconds":27,"wake":false}
```

| Champ | Type | Sens |
|---|---|---|
| `state` | chaîne | `here` (un lien au moins atteint l'iPhone), `away` (plus aucun), `unknown` (jamais atteint depuis le démarrage du moteur, ou supervision arrêtée ; **jamais diffusé**, seulement rendu par la requête) |
| `reason` | chaîne | pourquoi il est parti ; vide si `here` |
| `away_seconds` | entier | `away` : durée de l'absence à l'instant du message ; `here` : durée de l'absence qui vient de finir (0 à la première arrivée) |
| `lock` | chaîne | ce que fait le verrouillage, voir plus bas |
| `lock_in_seconds` | entier | `lock:"pending"` : secondes restantes avant le verrouillage ; 0 sinon |
| `wake` | bool | vrai seulement dans le `here` qui suit un verrouillage fait par le moteur, si `wake_on_return` est actif : « réveiller l'écran de connexion ». Faux dans la réponse à la requête |

`reason` :

| Valeur | Motif BlueZ (`Disconnected`) | Sens | Verrouille ? |
|---|---|---|---|
| `out_of_range` | `org.bluez.Reason.Timeout` | perte de signal : l'iPhone s'est éloigné (ou s'est éteint, batterie vide) | oui |
| `switched_off` | `org.bluez.Reason.Remote` | lien fermé par l'iPhone : Bluetooth coupé volontairement sur l'iPhone | non |
| `computer_suspended` | `org.bluez.Reason.Suspend` | l'ordinateur s'est mis en veille | non |
| `bluetooth_off` | — | le contrôleur de l'ordinateur est éteint ou absent (prime sur le motif BlueZ) | non |
| `local` | `org.bluez.Reason.Local` | lien fermé par l'ordinateur | non |
| `unknown` | aucun, ou autre (`Authentication`…) | BlueZ n'a pas donné de motif exploitable | non |

`lock` :

| Valeur | Sens |
|---|---|
| `none` | l'iPhone est là |
| `off` | parti, verrouillage désactivé (ou activé alors que l'iPhone était déjà parti) |
| `spared` | parti, verrouillage actif, mais le motif n'est pas un éloignement : pas de verrouillage |
| `pending` | parti hors de portée, délai de grâce en cours (`lock_in_seconds`) |
| `locked` | le moteur a demandé le verrouillage et il a été accepté |
| `failed` | le verrouillage a été demandé et refusé (pas de nouvel essai pour cette absence) |

Quand les messages partent :

- `here` : une fois, dès qu'un lien BR/EDR, LE ou une session OBEX atteint l'iPhone ;
- `away` : une fois, quand **tous** les liens sont tombés depuis 3 s (`PRESENCE_CONFIRM_SECONDS` : un lien
  qui retombe et remonte aussitôt n'est pas une absence, la coupure BR/EDR que le moteur fait lui-même
  pour le mains-libres non plus) ; puis de nouveau à chaque changement de `reason` (un motif arrivé en
  retard) ou de `lock` (`pending` → `locked`) ;
- une absence typique avec verrouillage donne donc trois messages : `away`/`pending`, `away`/`locked`,
  puis `here` au retour.

Délais réels à attendre (essai antérieur) : BlueZ ne constate la perte de signal que **20 à 35 s** après
le départ (délai de supervision du lien) ; `away` suit 3 s plus tard, le verrouillage `grace_seconds`
après la perte constatée. Avec le défaut de 30 s : verrouillage 50 à 65 s après le départ.

### Verrouillage : dans le moteur

Le moteur verrouille lui-même, par logind (`org.freedesktop.login1.Session.Lock` sur la session de
l'utilisateur) ou par la commande `lock_command` de `bluetooth.json` (`src/core/src/session_lock.cpp`).
Il n'est pas délégué à l'extension, pour trois raisons : une mesure de sécurité ne doit pas dépendre
d'un client connecté (extension désactivée, Shell redémarré) ; `Lock` de logind est exactement ce que
GNOME attend et ne demande aucun droit ; et le moteur a déjà le motif et l'horloge. L'extension reçoit
l'événement et n'a qu'à l'afficher.

Règles :

- seulement si `lock_on_away` est actif **au moment du départ** : activer le réglage pendant une absence
  ne verrouille pas, il vaut pour la suivante ; le désactiver pendant le délai annule ;
- seulement sur `out_of_range`, après `lock_away_seconds` sans aucun lien ;
- une fois par absence ; une session déjà verrouillée est laissée telle quelle (`LockedHint`) ;
- un iPhone jamais vu depuis le démarrage du moteur ne verrouille rien (`state:"unknown"`).

### Retour : jamais de déverrouillage

Le moteur **ne déverrouille jamais** la session, et ne contient aucun appel qui le ferait.
`org.gnome.ScreenSaver.SetActive(false)` est volontairement écarté : sous GNOME il lève l'écran de
verrouillage. `wake_on_return` (désactivé par défaut) ne fait que deux choses au retour de l'iPhone après
un verrouillage fait par le moteur :

- le `bt_presence` `here` porte `wake:true` — à l'extension d'allumer l'écran de connexion si elle tourne
  sur l'écran verrouillé (mode de session `unlock-dialog`) ;
- si `wake_command` est renseigné dans `bluetooth.json` (vide par défaut), le moteur lance cette
  commande. Aucune commande n'est fournie d'origine : il n'existe pas d'appel D-Bus public qui allume
  l'écran de GNOME sans toucher au verrou.

Le retour n'est signalé `wake:true` que si c'est le moteur qui a verrouillé pendant cette absence
(`lock:"locked"`) : revenir avant la fin du délai, ou rallumer le Bluetooth de l'iPhone, ne réveille rien.

## 8. Appels (mains-libres)

Le démon n'implémente pas le profil mains-libres. Il pilote celui de BlueZ (`org.bluez.Telephony1`, bus
système) ou celui de PipeWire (`org.pipewire.Telephony`, bus de session), le premier qui sert l'iPhone
(`src/core/src/bluetooth/telephony.cpp:135-152`). Sur cette machine c'est PipeWire.

| Message | Sens | Paramètres | Forme | Où |
|---|---|---|---|---|
| `bt_set_calls` | requête | `enabled` | `OK`, puis `bt_status` (`calls_enabled`) ; désactivé par défaut | `src/core/src/net.cpp:1531` |
| `bt_calls` | événement | — | `{calls:[…]}` à chaque changement de la liste des appels en cours | `src/daemon/main.cpp:771-778`, `connection.cpp:1347-1362` |
| `bt_list_calls` | requête | — | réponse directe `bt_calls` | `src/core/src/net.cpp:1609`, `connection.cpp:1678` |
| `bt_call_history` | événement | — | `{calls:[{number, name, type, timestamp}]}` | `src/daemon/main.cpp:780-785`, `connection.cpp:1154-1205` |
| `bt_list_call_history` | requête | — | réponse directe `bt_call_history` | `src/core/src/net.cpp:1617` |
| `bt_call_dial` | requête | `number`, `audio` (facultatif : `here` ou `phone`) | `bt_call_result` `{action:"dial", success, message?}`, en réponse directe **et** en diffusion | `src/core/src/net.cpp:1625`, `telephony.cpp:241-259` |
| `bt_call_action` | requête | `action`, `path` (facultatif), `audio` (facultatif, avec `answer` : `here` ou `phone`) | `bt_call_result` `{action, success, message?}`, idem | `src/core/src/net.cpp:1637`, `telephony.cpp:261-302` |
| `bt_call_tones` | requête | `tones` (chiffres, `*`, `#`, `A`-`D`, 32 au plus) | `bt_call_result` `{action:"tones", …}`, idem | `src/core/src/net.cpp:1651`, `telephony.cpp:304-322` |

Un client abonné reçoit donc `bt_call_result` deux fois (réponse + diffusion).

Un appel (`src/core/src/bluetooth/telephony.cpp:60-73`) :

```json
{"path":"/org/pipewire/Telephony/ag0/call1","number":"+41791234567","withheld":false,"name":"…",
 "incoming_line":"","state":"incoming","multiparty":false,"ringing":true,"outgoing":false,"connected":false}
```

- `state` : `incoming`, `waiting`, `dialing`, `alerting`, `active`, `held`. `ringing`, `outgoing` et
  `connected` en sont déduits. Un appel terminé disparaît de la liste.
- `name` est complété depuis les contacts dans `bt_list_calls` (`connection.cpp:1685-1691`) mais **pas**
  dans l'événement `bt_calls` diffusé : l'extension doit résoudre le nom elle-même ou relire la liste.
- `number` est normalisé par `bt_call_dial` : chiffres, `+` initial, séparateurs usuels retirés
  (`telephony.cpp:33-58`).

Actions de `bt_call_action` :

| `action` | Effet | `path` |
|---|---|---|
| `answer` | répondre | l'appel visé ; facultatif s'il n'y en a qu'un |
| `hangup` | raccrocher | idem |
| `hangup_all` | tout raccrocher | ignoré |
| `swap`, `hold_and_answer`, `release_and_answer`, `release_and_swap`, `multiparty` | gestion de deux appels | ignoré |
| `audio_here` | faire passer le son de l'appel sur l'ordinateur, **pour cet appel**, avant de décrocher comme en cours d'appel | ignoré ; PipeWire seulement |
| `audio_phone` | le renvoyer sur l'iPhone, pour cet appel ; **en cours d'appel, ferme le lien voix déjà ouvert** (voir section 9, « Rendre le son à l'iPhone en cours d'appel ») et ne répond `success:true` que si le lien s'est réellement fermé | ignoré ; PipeWire seulement |

Répondre et raccrocher fonctionnent quel que soit le réglage du son. Le paramètre `audio` de
`bt_call_dial` et de `answer` fixe où ira le son de **cet** appel, avant de le passer ou de le prendre ;
absent, rien n'est touché et le réglage `call_audio` s'applique (section 9).

État de la téléphonie, dans `bt_connection_changed.calls`
(`src/core/src/bluetooth/telephony.cpp:82-94`, `:158-175`) : `available`, `path`, `state`, `operator`,
`signal`, `battery`, `service`, `roaming`, `inband_ringtone`, `address`, `calls` (nombre), `backend`
(`bluez`, `pipewire` ou vide), `indicators`, `wifi_calling` (section 3), `audio` (état brut du transport
PipeWire : `active` quand le son de l'appel est ici ; vide sans transport), `audio_switchable` (bool : cet ordinateur peut porter le
son des appels), `audio_route` (`here` si le son de l'appel est ici, sinon `phone` : **lu sur le
transport de PipeWire à chaque lecture, jamais déduit de ce qui a été demandé**), `audio_reject` (bool
ou `null` : le prochain lien voix offert par l'iPhone sera refusé), `audio_parked` (bool : la carte
PipeWire de l'iPhone est laissée sur `off` jusqu'à la fin de l'appel, voir section 9), `reason`. Avec PipeWire, `indicators`
est faux :
opérateur, signal et batterie restent à zéro, et `available` vaut toujours vrai dès que la passerelle
existe (`src/core/src/bluetooth/telephony_pipewire.cpp:193`).

Limites actuelles, détaillées dans `moteur-reconnexion.md` (section 6) :

- la liste des appels est **interrogée chaque seconde** par le démon, sur le fil qui fait aussi tout le
  reste ; elle l'est maintenant avant les lectures longues (carnet, messages) et la connexion BR/EDR ne
  bloque plus ce fil, mais l'ouverture d'une session OBEX peut encore le retenir : un appel bref à ce
  moment-là n'est pas vu ;
- le démon n'affiche aucune bulle pour un appel entrant ; il écrit en revanche une ligne de journal à
  chaque changement d'état (voir ci-dessous) ;
- le journal d'appels ne se rafraîchit qu'à la fin d'un appel que le démon a vu, ou à une nouvelle session
  de contacts.

### Journal des appels

Chaque changement vu dans la liste des appels écrit une ligne `INFO` dans le journal du démon
(`describe_call_changes()`, `src/core/src/bluetooth/telephony.cpp`, appelé depuis `sync_calls()`). Le numéro
est masqué, sauf ses deux derniers chiffres ; un numéro caché donne `withheld number`.

```
[INFO] bluetooth: call: incoming from ***67 (incoming)
[INFO] bluetooth: call: answered, ***67 (incoming -> active)
[INFO] bluetooth: call: ended, ***67 (was active)
```

Autres formes : `call: outgoing to ***67 (dialing)`, `call: ***67 active -> held`,
`call: in progress with ***67 (active)` (appel déjà en cours à la première lecture). Un appel manqué laisse
deux lignes : `incoming from …` puis `ended, … (was incoming)`. Ces lignes n'existent que si le contrôle
d'appel est activé (`bt_set_calls`).

## 9. Audio : le son de l'iPhone

Par défaut, **le son de l'iPhone reste sur l'iPhone** : musique comme appels. L'ordinateur ne le reçoit
que si l'utilisateur le demande, par un réglage qui dure ou par une bascule ponctuelle. Le pourquoi et le
comment sont dans `moteur-quotidien.md` ; ici, le contrat.

| Message | Sens | Paramètres | Forme |
|---|---|---|---|
| `bt_audio` | requête | — | réponse directe : l'état ci-dessous |
| `bt_audio` | événement | — | le même objet, diffusé quand il change |
| `bt_set_phone_audio` | requête | `route` (`phone`, `here`, `auto`), `persist` (bool, vrai par défaut), `rule` (bool, facultatif) | `OK`, puis `bt_phone_audio_result`, et si `persist` : `bt_status` et `bt_audio` |
| `bt_set_call_audio` | requête | `route` (`phone`, `here`), `persist` (bool, vrai par défaut) | `OK`, puis `bt_call_audio_result`, `bt_status`, `bt_audio` |
| `bt_call_action` `audio_here` / `audio_phone` | requête | — | bascule pour l'appel en cours, section 8 |

Les deux commandes acceptent encore l'ancien `enabled` (vrai = `here`, faux = `phone`) à la place de
`route`. Une commande sans route lisible ne change rien et répond `success:false`,
`message:"invalid_route"`.

### État : `bt_audio`

```json
{"command":"bt_audio","phone_audio":"phone","phone_audio_route":"phone",
 "phone_audio_rule":false,"phone_audio_rule_installed":false,
 "phone_audio_rule_path":"/home/…/.config/wireplumber/wireplumber.conf.d/zz-landline-phone-audio.conf",
 "call_audio":"phone","call_audio_route":"phone","call_audio_switchable":true,"call_audio_reject":true}
```

| Champ | Type | Sens |
|---|---|---|
| `phone_audio` | chaîne | **réglage** pour la musique et les sons : `phone` (défaut), `here`, `auto` (le moteur ne touche à rien ; avec un WirePlumber d'usine le son vient ici) |
| `phone_audio_route` | chaîne | **fait** : `here` (un flux A2DP de l'iPhone est ouvert vers cet ordinateur), `phone`, `unavailable` (pas de lien BR/EDR, ou l'ordinateur n'offre pas le rôle d'enceinte) |
| `phone_audio_rule` | bool | réglage : la règle WirePlumber par appareil est demandée (faux par défaut) |
| `phone_audio_rule_installed` | bool | fait : le fichier de règle existe |
| `phone_audio_rule_path` | chaîne | chemin de ce fichier |
| `call_audio` | chaîne | **réglage** pour le son des appels : `phone` (défaut) ou `here` |
| `call_audio_route` | chaîne | **fait** : `here` pendant qu'un appel sonne dans l'ordinateur, sinon `phone` |
| `call_audio_switchable` | bool | l'ordinateur peut porter le son des appels (mains-libres tenu par PipeWire). Faux : mains-libres tenu par BlueZ, pas encore monté, ou contrôle d'appel désactivé |
| `call_audio_reject` | bool ou `null` | le prochain lien voix offert par l'iPhone sera refusé (`RejectSCO` de PipeWire) ; `null` si non commutable |

Diffusé à chaque changement : flux ouvert ou fermé, réglage modifié, appel dont le son arrive ou repart.
`phone_audio`, `phone_audio_route`, `phone_audio_rule` et `call_audio` figurent aussi dans `bt_status`.

### Musique : `bt_set_phone_audio`

| Commande | Effet | Écrit sur le disque |
|---|---|---|
| `{"route":"here"}` | réglage `here` : à chaque connexion, le flux est ouvert au bout de 5 s si l'iPhone ne l'a pas fait ; appliqué tout de suite | `bluetooth.json` |
| `{"route":"phone"}` | réglage `phone` : à chaque connexion, le flux que l'iPhone ouvre est refermé ; appliqué tout de suite | `bluetooth.json` |
| `{"route":"auto"}` | le moteur ne touche plus au flux | `bluetooth.json` |
| `{"route":"here","persist":false}` | bascule ponctuelle : le son vient ici **jusqu'à la prochaine déconnexion** ; le réglage ne change pas | rien |
| `{"route":"phone","persist":false}` | bascule ponctuelle dans l'autre sens | rien |
| `{"route":…,"rule":true}` | en plus : écrit la règle WirePlumber (voir « Ce qui est écrit sur le disque ») | `bluetooth.json` et le fichier de règle |
| `{"route":…,"rule":false}` | en plus : retire la règle | `bluetooth.json`, fichier de règle supprimé |

Résultat : `{"command":"bt_phone_audio_result","success":true,"route":"here","persist":false}`. Avec
`persist:false`, `success:false` et `message:"not_connected"` si l'iPhone n'est pas relié. `auto` avec
`persist:false` est refusé (`invalid_route`). Le déplacement réel du son se lit ensuite dans `bt_audio`
(`phone_audio_route`), une à quelques secondes plus tard.

Comportement du réglage `phone` (`plan_phone_audio()`, `PhoneAudioFollower`,
`src/core/src/bluetooth/phone_audio.cpp`) : l'iPhone ouvre lui-même le flux environ une seconde après
chaque connexion. Le moteur attend 3 s que l'ouverture soit finie, puis le referme
(`Device1.DisconnectProfile` sur le profil A2DP source de l'iPhone) ; le lien Bluetooth, les messages, les
notifications et le mains-libres ne sont pas touchés. S'il se rouvre, il est refermé de nouveau, **trois
fois au plus, dans les 30 s** qui suivent la connexion. Passé ce délai, un flux qui s'ouvre est un choix
fait sur l'iPhone (Centre de contrôle) : il est laissé, `phone_audio_route` passe à `here`, et l'extension
peut proposer « renvoyer sur l'iPhone » (`route:"phone"`, `persist:false`). Une bascule ponctuelle arrête
ce suivi jusqu'à la prochaine connexion. Fermer le flux met en pause ce que l'iPhone jouait, comme un
casque qu'on débranche.

### Appels : `bt_set_call_audio`

| Commande | Effet |
|---|---|
| `{"route":"phone"}` | réglage `phone` : le lien voix offert par l'iPhone est refusé ; le son des appels reste sur l'iPhone |
| `{"route":"here"}` | réglage `here` : le son des appels vient sur l'ordinateur |
| `{"route":…,"persist":false}` | pour l'appel en cours, ou pour le prochain s'il n'y en a pas ; le réglage ne change pas |

Résultat : `{"command":"bt_call_audio_result","success":true,"route":"here","persist":true,
"applied_now":true}`. `applied_now:false` avec `message` quand l'ordinateur ne porte pas le son des appels
à cet instant (`call_audio_switchable:false`) : avec `persist:true` le réglage est tout de même
enregistré (`success:true`) et s'appliquera quand le mains-libres montera.

Comportement (`CallAudioPolicy`, `telephony.hpp` ; `TelephonyClient::poll_calls()`) : le moteur tient
l'interrupteur `RejectSCO` du mains-libres de PipeWire sur le réglage. Il l'écrit quand le mains-libres
de l'iPhone apparaît (PipeWire le crée ouvert à chaque connexion), quand le réglage change, et **quand le
dernier appel se termine** : un choix fait pour un appel (`audio_here`, `audio_phone`, `audio` sur
`answer` ou `bt_call_dial`, `persist:false`) dure le temps de cet appel puis le réglage reprend. Jamais
pendant un appel. Le contrôle d'appel doit être activé (`bt_set_calls`) : sans lui le moteur ne parle pas
au mains-libres et ne tient pas cet interrupteur.

#### Rendre le son à l'iPhone en cours d'appel

`audio_phone` (ou `bt_set_call_audio` avec `route:"phone"`) pendant un appel dont le son est ici
(`src/core/src/bluetooth/call_audio_handover.cpp`, `TelephonyClient::route_audio()`) :

1. `RejectSCO` passe à vrai : l'iPhone ne peut pas rouvrir le lien voix ;
2. la carte PipeWire de l'iPhone (`bluez_card.<adresse>`, trouvée dans `pw-dump`) passe sur le profil
   `off` (`pw-cli set-param <id> Profile '{ index: 0, save: false }'`, à défaut `wpctl set-profile`) :
   PipeWire retire les nœuds de l'appel, relâche le transport et **ferme le lien voix (SCO)**. Le lien
   de commande du mains-libres (RFCOMM) ne dépend pas du profil de la carte : l'appel n'est pas touché
   et reste pilotable (raccrocher, touches) ;
3. le moteur attend jusqu'à 2 s que le transport ne soit plus `active`, puis **remet la carte sur son
   profil** (`audio-gateway`) dans tous les cas ;
4. si le lien voix se rouvre dans la demi-seconde qui suit, la carte est remise sur `off` et y reste
   jusqu'à la fin de l'appel (`audio_parked:true`) ; elle est remise à la fin du dernier appel, ou dès
   qu'on redemande le son ici.

Résultat : `success:true` seulement si le transport n'est plus `active` ; sinon `success:false`,
`message:"The call audio is still on this computer."` (dans `bt_call_result`, ou `applied_now:false`
dans `bt_call_audio_result`). Le journal dit `telephony: call audio handed back to the iPhone
(released, voice link closed in N ms)` ou `telephony: the call audio is still on this computer (…)`.
Rien n'est enregistré comme choix de l'utilisateur (`save: false`), rien n'est écrit sur le disque,
aucun autre appareil audio n'est nommé.

`audio_here` en cours d'appel : `RejectSCO` à faux, la carte est remise sur son profil si elle était
sur `off`, puis `Activate()` (PipeWire envoie `AT+BCC`, l'iPhone ouvre le lien voix). Le résultat ne
dit que si la demande est partie : l'arrivée du son se lit dans `calls.audio_route`, une à trois
secondes plus tard.

Limites connues :

- la bascule en cours d'appel bloque la boucle d'événements le temps de la vérification : jusqu'à
  2,5 s, moins d'une seconde quand le lien se ferme aussitôt ;
- sans carte PipeWire pour l'iPhone, ou sans `pw-dump`/`pw-cli`/`wpctl`, le lien voix ne peut pas être
  fermé d'ici : `success:false`, et il reste à choisir l'iPhone sur l'iPhone ;
- ces commandes s'exécutent sur la boucle d'événements et attendent PipeWire jusqu'à 5 s ;
- avec le mains-libres tenu par BlueZ, le son des appels ne peut pas venir sur l'ordinateur.

### Ce qui est écrit sur le disque

| Fichier | Quand | Contenu | Comment il est retiré |
|---|---|---|---|
| `~/.config/landline/bluetooth.json` | à tout réglage | `phone_audio` (`phone`, `here`, `auto`), `phone_audio_rule` (bool), `call_audio` (`phone`, `here`), `lock_on_away`, `lock_away_seconds`, `wake_on_return`, `wake_command`, `config_version: 3` | fichier du moteur ; supprimé avec la configuration |
| `~/.config/wireplumber/wireplumber.conf.d/zz-landline-phone-audio.conf` | **seulement** après `bt_set_phone_audio` avec `rule:true`, un iPhone choisi et une route autre que `auto` | une règle `monitor.bluez.rules` qui met `bluez5.auto-connect = [ ]` pour **la seule adresse de l'iPhone** : WirePlumber ne compose plus lui-même le flux vers cet appareil. Aucun autre appareil n'est nommé | `bt_set_phone_audio` avec `rule:false`, ou avec `route:"auto"`, ou `bt_unpair` de l'iPhone ; à la main : supprimer le fichier puis `systemctl --user restart wireplumber` |

L'écriture **et** le retrait du fichier de règle redémarrent WirePlumber (`systemctl --user try-restart
wireplumber`) : le son de l'ordinateur se coupe une seconde et les casques Bluetooth se reconnectent.
C'est pour cela que la règle n'est plus écrite par défaut. Le moteur n'écrit **jamais**
`51-no-phone-audio.conf` (fichier de la documentation amont, qui change `bluez5.roles` pour tous les
appareils), ne modifie pas `bluez5.roles`, et n'appelle pas `pactl`.

Changement de version : un `phone_audio: "auto"` écrit par une version antérieure (`config_version`
inférieur à 3) était l'ancienne valeur par défaut, pas un choix ; il est lu comme `phone`. Pour retrouver
l'ancien comportement : `bt_set_phone_audio` avec `route:"auto"`.

Le moteur ne gère pas AMS : pas de titre en cours ni de commandes lecture/pause vers l'iPhone.

Les commandes `bt_airpods*` concernent des AirPods reliés à l'ordinateur (batterie, mode d'écoute,
passage de relais) et sortent du périmètre de la pastille iPhone. C'est le seul code du moteur qui
appelle `pactl` (`src/core/src/audio.cpp`) ; il est inactif (`airpods_enabled:false`) et chaque appel est
sans effet quand `pactl` est absent.

## 10. Récapitulatif pour l'extension

Événements à écouter, par besoin :

| Besoin | Événement |
|---|---|
| Icône et état de la pastille, batterie | `bt_connection_changed` |
| Batterie faible (20 %, 10 %) | `bt_battery_low` |
| iPhone parti / revenu, verrouillage | `bt_presence` |
| Où va le son de l'iPhone (musique, appels) | `bt_audio` ; commandes `bt_set_phone_audio`, `bt_set_call_audio` |
| Interrupteur de l'iPhone à activer | `map_error` / `pbap_error` à `forbidden` ; `bt_retry_profiles` quand l'utilisateur agit |
| Moteur coupé du bus de session | `map_error` / `pbap_error` à `bus_closed` (le moteur redémarre seul) |
| Appairage à refaire | `needs_repair` et `bond_state` dans `bt_connection_changed` et `bt_status` ; réparation par `bt_pair` avec `fresh:true` (voir `contrat-appairage.md`) |
| Prérequis système manquants, réglages | `bt_status` |
| Repérer l'iPhone avant appairage | `bt_devices` |
| Étapes d'appairage | `bt_pair_progress`, `bt_pair_confirm_request`, `bt_pair_result` |
| Notification arrivée / retirée | `bt_notification`, `bt_notification_removed` |
| Message arrivé ou envoyé | `bt_message`, `bt_send_result`, `bt_message_read`, `bt_threads` |
| Appel entrant, en cours, terminé | `bt_calls`, `bt_call_result`, `bt_call_history` |
| Code à usage unique | `otp_available` |

Rien de tout cela ne demande d'interroger périodiquement. Les seules lectures à la demande sont les listes
(`bt_list_threads`, `bt_list_messages`, `bt_list_contacts`, `bt_list_notifications`, `bt_list_calls`,
`bt_list_call_history`) à l'ouverture d'un menu.

Ce qui manque au protocole pour un client propre, à prévoir quand on fera évoluer le moteur :

1. un identifiant de requête repris dans chaque réponse, et la fin de l'accusé `OK` non JSON ;
2. un événement à l'appel entrant qui ne dépende pas d'une interrogation (section 8) ;
3. ~~la confirmation d'appairage proposée d'abord aux clients abonnés~~ — fait (section 6) ;
4. ~~les bulles du démon désactivées par défaut~~ — fait (section 2) ;
5. le drapeau « historique » sur `bt_message`, et le nom du contact dans `bt_calls`.

## 11. Réglages de démarrage

### Bluetooth seul

Par défaut le démon ne démarre que le Bluetooth et le socket Unix local. Il **n'ouvre pas** le port TCP
5134, **ne crée pas** `cert.pem` / `key.pem` dans `~/.config/landline/`, **ne se publie pas** en mDNS et
**n'enregistre pas** le service GATT « LAN info ». Le journal le dit au démarrage :
`Network side (TCP/TLS, mDNS, LAN info): disabled`.

| Réglage | Où | Défaut | Effet |
|---|---|---|---|
| `network_enabled` | `~/.config/landline/bluetooth.json` | `false` | `true` rétablit TCP/TLS, mDNS et le service GATT |
| `LANDLINE_NETWORK` | environnement du démon | absent | `1` force la partie réseau, `0` la coupe, quel que soit le fichier |
| `desktop_popups_enabled` | `bluetooth.json`, ou commande `set_desktop_popups` | `false` | bulles libnotify du démon |
| `pair_dialog_enabled` | `bluetooth.json` | `false` | fenêtre `tether-dialog` avant le client pour le code d'appairage |

La décision est prise une fois, au démarrage (`src/daemon/main.cpp`, `resolve_network_enabled()` dans
`src/core/src/net.cpp`) : changer `network_enabled` demande de relancer le démon. Le code réseau n'est pas
supprimé, seulement non démarré ; l'objet `TcpServer` existe toujours parce que `UnixServer` en garde une
référence (voir `moteur-inventaire.md`, 4.1).

Ce qui reste présent partie réseau coupée : le presse-papiers Wayland (inerte sous GNOME) et le
gestionnaire de fichiers reçus (jamais sollicité). `state_snapshot` reste envoyé à l'abonnement, avec des
listes vides et `network_enabled:false`.

### Réglages de reconnexion (environnement du démon)

Soupapes pour les corrections de `moteur-reconnexion.md`, à n'utiliser que pour comparer ou revenir en
arrière. Lus une fois au démarrage.

| Variable | Défaut | Effet |
|---|---|---|
| `LANDLINE_SYNC_CLASSIC_CONNECT=1` | absent | reprend l'ancienne connexion BR/EDR bloquante |
| `LANDLINE_HFP_ABSENT_SECONDS=<n>` | 8 | délai avant le cycle mains-libres, de 3 à 120 s (20 = ancienne valeur) |
| `LANDLINE_ADVERT_INTERVALS=0` | absent | annonce LE sans `MinInterval` / `MaxInterval`, comme avant |
| `LANDLINE_ADVERT_FAST_SECONDS=<n>` | 180 | durée de l'annonce à 20 ms après la perte du lien LE, de 0 à 3600 s (30 = ancienne valeur) |
| `LANDLINE_HFP_CONNECT_PROFILE=0` | absent | ne demande plus le mains-libres sur le lien en place avant le cycle |
| `LANDLINE_CONTACTS_ALWAYS_PULL=1` | absent | relit le carnet à chaque nouvelle session contacts, comme avant |
| `LANDLINE_ANCS_FIRST_SECONDS=<n>` | 12 | attente maximale des lectures longues derrière les notifications ; 0 la coupe, ainsi que le pas de 1 s d'ANCS |
| `LANDLINE_OBEX_ABSENT_BACKOFF=0` | absent | tentatives MAP/PBAP toutes les 15 s même sans lien, comme avant |
| `LANDLINE_BUS_WATCH=0` | absent | le démon ne sort plus quand son bus de session ou système est fermé, comme avant |
| `LANDLINE_FORBIDDEN_FAST_SECONDS=<n>` | 300 | durée des essais toutes les 2 s d'un profil refusé (`forbidden`), de 0 à 3600 s ; 0 les coupe, comme avant |
| `LANDLINE_ANCS_STOP_NOTIFY=0` | absent | plus aucun `StopNotify` vers BlueZ, même quand les notifications sont coupées (le moteur n'en envoie déjà plus à la reconstruction d'une session) |
| `LANDLINE_LE_FIRST=0` | absent | coupe les quatre mesures « LE d'abord » (retenue, recyclage, préférence rendue, connexion LE sortante retardée), comme avant |
| `LANDLINE_LE_FIRST_HOLD_SECONDS=<n>` | 10 | sans aucun lien, durée pendant laquelle rien n'est demandé en BR/EDR pour laisser l'iPhone rouvrir le LE, de 0 à 600 s ; 0 la coupe |
| `LANDLINE_LE_RECYCLE_SECONDS=<n>` | 12 | BR/EDR monté et annonce en ondes sans LE depuis ce temps : tous les liens sont coupés une fois, de 0 à 600 s ; 0 le coupe |
| `LANDLINE_MESSAGE_POLL_SECONDS=<n>` | 15 | intervalle de relève des messages, de 5 à 3600 s |
| `LANDLINE_LONG_READ_ATTEMPTS=0` | absent | plus d'essai de profil ni de travail de notifications pendant une lecture longue (carnet, messages), comme avant |

Pendant le cycle mains-libres (coupure volontaire de BR/EDR, quelques secondes) et pendant le recyclage
« LE d'abord » (coupure volontaire de tous les liens, une fois), `map_open` et `pbap_open`
restent publiés à vrai alors que les sessions se rouvrent ; `classic_connected` et `le_connected` passent
bien à faux. Pendant la retenue (10 s au plus sans aucun lien), `link_reason` dit que le moteur attend que
l'iPhone rouvre d'abord le lien des notifications. Quand bluetoothd redémarre, `device_present` passe à
faux le temps de son absence, puis tout se rétablit sans commande. Une
commande de message envoyée à ce moment-là peut échouer.

## 12. À vérifier avec l'iPhone

Ce que l'extension fait de ces messages (passe 4) est décrit dans `extension.md`, « Le quotidien » :
`bt_audio` et `bt_presence` sont demandés à chaque connexion ; un moteur qui ne les connaît pas répond
`OK` et l'extension ne propose alors rien de nouveau. Deux hypothèses de l'extension restent à confirmer
ici : `calls.audio_route` passe bien à `here` dans les secondes qui suivent `bt_set_call_audio` avec
`persist:false` pendant un appel (sinon la carte annonce à tort que le son n'a pas bougé, au bout de
6 s), et `bt_audio` est rediffusé quand le contrôle d'appel s'active (`call_audio_switchable`), sans
quoi la carte d'appel entrant garde un seul « Answer ».

Les trois ajouts de la branche `moteur-quotidien` sont compilés et couverts par des tests unitaires
(règles pures), mais **aucun n'a tourné face à l'iPhone**. La liste des essais réels, avec ce qu'on doit
observer, est dans `moteur-quotidien.md`, section 5.
