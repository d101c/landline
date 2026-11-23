# Mode sobre : écarts par rapport à la conception

Référence : `iphone-link-study/11-classique-a-la-demande.md` (section 5, étapes M1 à M4). Branche
`moteur-sobre`. Ce fichier liste chaque endroit où le moteur réalisé s'écarte du plan : nom prévu → nom réel,
et pourquoi.

**Sur le socket, il n'y a aucun écart de nom** : commandes (`bt_set_link_mode`, `bt_lease`, `bt_call_cancel`),
événements (`bt_send_progress`, `bt_incoming_call`, `bt_call_progress`), champs et valeurs sont ceux de la
conception. Les écarts qui se voient depuis un client sont marqués **[client]**.

## Ce qu'un client peut remarquer

| # | Prévu | Réalisé | Raison |
|---|---|---|---|
| 1 | **[client]** `bt_call_result` : « réponse » | envoyé **au client qui a fait la commande et diffusé aux abonnés** ; un client abonné le reçoit une seule fois | la commande ne s'exécute plus sur la boucle : il fallait rendre la réponse à `landlinectl`, qui n'est pas abonné, sans la doubler pour l'extension |
| 2 | **[client]** `bt_call_dial` sans passerelle, mode complet : non précisé | échec **immédiat**, `reason:"no_hands_free"`, comme avant | « le mode complet répond comme avant » ; rien n'ouvre de lien pour un appel dans ce mode, attendre 35 s n'aurait servi à rien |
| 3 | **[client]** `bt_incoming_call` : condition `calls_enabled` (4.9) | publié seulement si le contrôle d'appel est activé, dans les deux modes | sans lui, ni `answer` ni `decline` ne sont offerts ; la notification ANCS ordinaire reste |
| 4 | **[client]** fin de sonnerie : quatre cas | deux de plus : `ended` aussitôt si la passerelle est là et ne montre plus d'appel au retrait de la notification ; `ended` après 90 s de sonnerie sans aucun signal (`RING_MAX_SECONDS`) | le premier évite 20 s de carte fantôme en mode complet ; le second couvre un lien LE perdu pendant la sonnerie, où le retrait n'arrive jamais |
| 5 | **[client]** `bt_retry_profiles` : inchangé | en mode sobre, tient en plus le lien ouvert 30 s (bail `reading` court) | sur un lien fermé il n'y a rien à réessayer ; un client d'avant la branche garde ainsi un comportement utile |
| 6 | **[client]** `next_sync` : « prochaine relève horaire » | en mode complet : `last_sync` + l'intervalle de fond réel (3 600 s avec notifications, 15 s sans) | le champ reste vrai dans les deux modes |
| 7 | **[client]** `classic_state`, `classic_leases`, `classic_opened_by` en mode complet : non précisé | `open` / `idle` selon le lien, `[]`, `engine` / `""` | la machine à baux ne tourne pas dans ce mode |
| 8 | **[client]** `link_mode_reason:"no_le"` | aussi quand l'appairage n'a pas de moitié LE, pas seulement quand le contrôleur ne peut pas ; jamais dans les 10 premières secondes du démon | sans moitié LE le mode sobre n'ouvrirait jamais rien ; au démarrage BlueZ n'a pas fini d'énumérer et un repli sur le mode complet ferait appeler l'iPhone à chaque lancement |
| 9 | **[client]** `notifications_off` : « coupées ou jamais accordées » | l'accord est mémorisé sur disque (`link.json`) et retiré quand le LE est monté depuis 75 s sans que l'iPhone offre le service | sans mémoire, chaque démarrage commencerait en mode complet, le temps que les notifications soient prêtes |
| 10 | Présence « = LE tombé » | au repos, oui (rien d'autre n'est monté). Un lien classique **ouvert** compte encore comme présence | le LE cligne pendant que le classique monte (étude, 1.1) : sans cela, un appel en cours pouvait être suivi d'un faux « iPhone parti », verrou compris |
| 11 | Une notification de Messages **retirée** → `request_message_sync("ancs_removed")`, et M2 : bail `arrival` sur `request_message_sync` | la relève est demandée, mais **aucun bail** n'est pris pour un retrait ni pour un message qu'on vient d'envoyer : ils sont relevés à l'ouverture suivante | 4.2 ne prévoit le bail `arrival` que pour une notification non préexistante ; ouvrir le lien à chaque message lu sur l'iPhone coûterait une ouverture par lecture |
| 12 | Relances « à +3 s puis +10 s » | 3 s après la première relève, puis 10 s après la deuxième (soit 2, 5 et 15 s après la demande) | lecture retenue de la formule ; E3 dira si c'est trop ou trop peu |

## Ce qui ne se voit que dans le code

| # | Prévu | Réalisé | Raison |
|---|---|---|---|
| 13 | Déclencheurs ANCS (relève, journal d'appels périmé, traqueur d'appel) dans `src/daemon/main.cpp` | dans `connection.cpp`, là où le client ANCS rappelle, par deux classes pures : `AncsSyncTriggers` (`message_sync_policy.hpp`) et `IncomingCallTracker` (`incoming_call.hpp`) | testable sans démon ; `main.cpp` n'a pas à connaître les baux. `request_message_sync()` et `mark_call_history_stale()` existent bien sur `ConnectionManager` |
| 14 | `message_sync_requested_at`, `message_sync_attempts` dans `ConnectionState` | une structure `MessageSyncRequest` (demande, tentatives, motif, expiration à 60 s si aucune session ne vient) ; `MessageSyncCause` en plus de `MessageSyncPlan` | la demande vient d'autres fils : elle est datée par le tic suivant ; le motif sert à la ligne `inbox listed (…)` |
| 15 | `DemandFacts` : neuf champs | deux de plus : `data_refused` (les deux profils essayés et refusés : échec sans attendre 20 s), `held_by_other` (profil réseau de l'iPhone connecté : partage de connexion par NetworkManager, lien jamais fermé) | le second réalise la règle de 4.2 sur le partage de connexion |
| 16 | `ClassicDemand::take(Lease, now)` | `take(lease, now, ttl_seconds = 0, urgent = false)` ; en plus `want_data()`, `keep_data()`, `adopt_engine_link()`, `reset()`, `retry_at()`, `capped()`, `phone_keeps_link(now)` | durée propre aux baux de client ; `urgent` = recyclage immédiat à la sonnerie ; `keep_data` garde les sessions pendant le maintien (réponse en moins d'une seconde) |
| 17 | `LeaseKind` : `Data`, `Voice` ; bail `son` de type « son », `installation` « données + voix » | `audio` et `setup` sont de type `Voice` | deux types suffisent : tous deux veulent une vraie connexion |
| 18 | `BearerSupervisor::set_link_mode()` | `set_link_mode(bool on_demand)` ; `set_demand(action, now)` exécute tout de suite | le superviseur n'a besoin que de savoir s'il tient le lien ; attendre le tic suivant ajoutait une seconde à chaque ouverture |
| 19 | `BluezBearerOps::connect_classic()` « en mode sobre : `Bearer.BREDR1.Connect` seul » | nouvelle méthode `BearerOps::connect_classic_on_demand()` ; `connect_classic()` est intact | le mode complet garde son chemin au caractère près |
| 20 | `PreferredBearer` « plus écrit » | jamais écrit pour ouvrir. Remis **une fois** à `le` s'il est trouvé à `bredr` sans connexion en cours | le mode complet peut l'avoir laissé ainsi, et un appairage épinglé sur BR/EDR refuse le lien LE dont tout dépend |
| 21 | `ProfileSupervisor::set_demand(bool want_data)` | `set_on_demand(bool)` et `set_demand(want_data, keep_open)` | voir 16 |
| 22 | `pairing.cpp`, fin de `pair_device()` : bail `setup` | pris dans `net.cpp`, après `set_device()` | la supervision est redémarrée après l'appairage : un bail pris avant serait perdu |
| 23 | `PhoneAudioFollower` : bail `audio` | `set_lease_handler()` côté suiveur, `ConnectionManager::set_phone_audio_here()` côté superviseur ; le choix permanent est lu au démarrage | le suiveur vit dans la boucle du démon, la machine à baux dans le fil de supervision |
| 24 | `src/sleep_watch.cpp` : abonnement à `PrepareForSleep` | classe `SleepAnnouncer`, qui tient en plus un **inhibiteur de veille de type « delay »** (rendu dès que le lien est fermé, 3 s au plus) | sans lui logind suspend dans la milliseconde et la fermeture n'a pas lieu ; `SleepWatch` existait déjà (`le_after_sleep.hpp`) |
| 25 | « dernier verdict MAP / PBAP sur disque » | `$XDG_STATE_HOME/landline/link.json` : adresse, accord des notifications, verdict et date de chaque profil | un seul petit fichier, sans donnée personnelle |
| 26 | `save_call_history()` / `load_call_history()` | conformes ; en plus le journal d'appels n'est relu à l'ouverture d'une session qu'en mode complet, ou s'il n'a jamais été lu | en mode sobre une session s'ouvre des dizaines de fois par jour |
| 27 | Carnet : `GetSize` seul, relecture si le nombre change ou après 24 h | `plan_contacts_on_demand()` (`reconnect_policy.hpp`), en mode sobre seulement | le mode complet garde `plan_contacts()` |
| 28 | `ConnectionManager::ring(uid)` / `ring_over(uid)` | pas de méthodes de ce nom : le bail `call` est un souhait recalculé à chaque tic (sonnerie, 20 s après, composition, appel en cours) | une seule règle au lieu de prises et de rendus dispersés entre trois fils |
| 29 | `TelephonyClient::decline`, choix ANCS / HFP dans `telephony.cpp` | `plan_call_action()` (pure, `incoming_call.hpp`) et `ConnectionManager::call_action(path, action, options, err, via)` ; tests dans `test_incoming_call.cpp` | `TelephonyClient` ne connaît pas ANCS |
| 30 | `dial_async(number, audio, operation_id)` | deux rappels en plus (progression, résultat) | `net.cpp` publie, le superviseur ne connaît pas le socket |
| 31 | `telephony_pipewire.cpp` : « lire l'état réel » | la passerelle est dite `connecting` tant que son gestionnaire d'objets ne répond pas ; `LANDLINE_GATEWAY_STRICT=0` pour revenir en arrière | PipeWire n'expose pas d'état sur `AudioGateway1` ; « répond » est le seul signe lisible |
| 32 | Plafond de 5 min d'un lien sans bail voix | ne s'applique pas pendant un envoi en cours | ne pas couper un message en train de partir |
| 33 | Interrupteurs prévus : `LANDLINE_LINK_MODE`, `LANDLINE_MESSAGE_POLL_SECONDS`, `LANDLINE_ANCS_CALLS` | deux de plus : `LANDLINE_SLEEP_CLOSE`, `LANDLINE_GATEWAY_STRICT` | chaque comportement risqué a son retour arrière |

## Ce qui n'a pas été réalisé

| Élément de la conception | État | Raison |
|---|---|---|
| 4.5, entrée **provisoire** d'un message créée depuis la notification ANCS, remplacée par le message MAP | non réalisée | absente des tableaux de fichiers de M1 à M4 ; l'extension affiche la notification elle-même et la fusionne (X1) |
| Test `test_net.cpp` « les autres commandes sont servies pendant l'attente d'un `bt_call_dial` » | remplacé | aucun démon ni pile téléphonie en test unitaire : le test vérifie que la réponse revient hors de la boucle, avec son `operation_id` ; la non-rétention de la boucle est garantie par construction (fil à part) |
| Critère M2 « en mode complet, le journal d'une reconnexion rejouée en test est identique » | couvert par les 927 tests d'origine, inchangés et verts | pas de test de rejeu de journal dans le dépôt |
| Catalogue de traduction (`engine/po`) | non régénéré | il était déjà périmé avant la branche (56 entrées d'écart, sans rapport) ; les nouvelles phrases restent en anglais |
