# Moteur : lecture du code de reconnexion

Lecture de `engine/` (Tether, commit `9ca3c46`). Chemins relatifs à `engine/src/core/` sauf mention
contraire ; numéros de ligne valables pour ce commit.

Convention : **lu** = constaté dans le code ; **mesuré** = relevé pendant l'essai du 16 novembre 2026
(`iphone-link-study/bench/chrono-reconnexion.log` et journal du démon) ; **déduit** = raisonnement à
confirmer par une capture `btmon`. Aucun essai de reconnexion n'a été refait pour écrire ce document.

**Mise à jour (branche `moteur-reconnexion-2`).** Deux essais réels ont été faits le 16 novembre sur la
première série de corrections : relevés en section 1 ter. Ils ont montré quatre défauts restants et un
réglage trop court ; la seconde série de corrections est en section 7 et résumée en section 1 bis. Elle est
compilée et testée, **pas essayée avec l'iPhone**.

**Mise à jour (branche `moteur-reactif`).** Deux défauts vus en essai réel le 16 novembre — moteur resté
sur un bus de session mort, interrupteurs de l'iPhone vus avec jusqu'à 20 s de retard — sont traités en
section 9 et résumés en section 1 bis. Compilé et testé, **pas essayé avec l'iPhone**.

**Mise à jour (branche `moteur-robuste`).** Le 16 novembre en fin de journée, le moteur a fait planter
bluetoothd, et le lien LE n'est pas revenu après trois relances alors que BR/EDR était monté. Cause du
plantage, correction, stratégie « LE d'abord », survie au redémarrage de bluetoothd et revue des cadences :
section 10, résumée en section 1 bis. Compilé et testé, **pas essayé avec l'iPhone**.

**Mise à jour (branche `moteur-bluetooth-seul`).** Les corrections 5.1, 5.2, 5.4, 5.5 et 5.6 sont écrites
et couvertes par des tests unitaires ; leur état est en section 1 bis et rappelé en tête de chaque
correction. Les sections 1 à 4 décrivent le comportement **d'avant**, tel que mesuré : elles restent la
référence de comparaison. Le protocole d'essai est dans `essai-reconnexion.md`. Les numéros de ligne restent
ceux du commit amont.

## 1. Résumé

- Le délai de 30 s est la constante `CONNECT_TIMEOUT_MS = 30000` (`src/bluetooth/connection.cpp:302`),
  appliquée à un appel D-Bus **synchrone** vers BlueZ pour rétablir le lien BR/EDR
  (`connection.cpp:411-438`, appel en `:578-602`). « Timeout was reached » est le texte de GLib quand ce
  délai expire sans réponse de BlueZ.
- Cet appel bloque **le seul fil** qui fait tout le reste : sessions messages et contacts, abonnement aux
  notifications, lecture des appels, publication de l'état (`connection.cpp:1394-1449`). Pendant 30 s, le
  moteur est sourd et muet alors que le lien LE est déjà revenu.
- Dès que le fil est libéré, l'ouverture de la session messages rétablit le lien BR/EDR en moins d'une
  seconde : l'iPhone était joignable depuis le début. Les 30 s sont perdues pour rien.
- Les notifications n'arrivent qu'à +47 s parce que le même tour de boucle relit d'abord tous les contacts
  et les messages avant de s'occuper d'ANCS.
- La téléphonie n'arrive qu'à +70 s parce que le lien BR/EDR est revenu par la session messages, sans le
  profil mains-libres ; le moteur attend 20 s (`HFP_ABSENT_SECONDS`), coupe lui-même le lien BR/EDR, et
  c'est la reconnexion suivante qui apporte la téléphonie. Cette coupure volontaire n'écrit rien dans le
  journal.

Les trois temps mesurés (+32 s, +47 s, +70 s) s'expliquent entièrement par l'enchaînement de ces trois
attentes. Les corrections de la section 5 visent à les ramener à quelques secondes.

## 1 bis. État des corrections

| Correction | État | Où | Retour arrière |
|---|---|---|---|
| 5.1 Connexion BR/EDR asynchrone | fait | `BluezBearerOps::connect_classic()` dans `src/bluetooth/connection.cpp`, `include/tether/bluetooth/pending_connect.hpp`, `BearerOps::classic_connect_outstanding()` | `LANDLINE_SYNC_CLASSIC_CONNECT=1` |
| 5.2 ANCS et appels avant les lectures longues | fait | `run_supervision_tick()` dans `include/tether/bluetooth/supervision_tick.hpp`, appelé par `ConnectionState::run()` | aucun (simple réordonnancement) |
| 5.3 Retenir OBEX pendant la connexion | **non fait**, gain non démontré (7.1) | — | — |
| 5.4 (a) Journaliser la coupure volontaire | fait | `src/bluetooth/bearer_supervisor.cpp` | — |
| 5.4 (b) `HFP_ABSENT_SECONDS` 20 s → 8 s | fait ; essai 1 : mains-libres venu seul avant 8 s ; essai 2 : cycle déclenché à 8 s | `include/tether/bluetooth/bearer_supervisor.hpp` | `LANDLINE_HFP_ABSENT_SECONDS=20` |
| 5.5 Délai D-Bus expiré ≠ refus | fait, à la source | `classify_classic_reply()` dans `pending_connect.hpp` | avec 5.1 |
| 5.6 Intervalle de l'annonce LE | fait ; accepté par BlueZ (journal des essais) ; intervalle en ondes **à valider par `btmon`** | `src/bluetooth/advert.cpp`, `include/tether/bluetooth/advert.hpp`, `src/bluetooth/pairing.cpp` | `LANDLINE_ADVERT_INTERVALS=0` ; repli automatique si BlueZ refuse |
| 5.7 Sortir OBEX du fil de supervision | non fait | — | — |
| 5.8 Petits gains | pas de nouvelle tentative ANCS : fait (7.2) ; réveil sur événement : non fait | — | — |
| 6.1 Journal des transitions d'appel | fait | `describe_call_changes()` dans `src/bluetooth/telephony.cpp` | — |
| 7.1 Mains-libres demandé sur le lien en place | fait, **effet inconnu** | `request_hands_free()`, `src/bluetooth/bearer_supervisor.cpp` | `LANDLINE_HFP_CONNECT_PROFILE=0` |
| 7.1 Refus périmé oublié au retour du LE ; journal des demandes BR/EDR | fait | `forget_classic_refusal()`, `src/bluetooth/connection.cpp` | — |
| 7.1 Cycle sans chute de l'état publié | fait | `mask_planned_drop()` dans `include/tether/bluetooth/reconnect_policy.hpp` | — |
| 7.1 Carnet gardé en mémoire | fait, `GetSize` **à vérifier sur l'iPhone** | `plan_contacts()`, `ConnectionState::sync_contacts()`, `PbapSession::phonebook_size()` | `LANDLINE_CONTACTS_ALWAYS_PULL=1` |
| 7.2 Lectures longues retenues pour ANCS ; pas de 1 s | fait | `NotificationPriority`, `run_supervision_tick()`, `ancs_retry_seconds()` | `LANDLINE_ANCS_FIRST_SECONDS=0` |
| 9.1 Bus fermé : sortie avec le code 75, motif `bus_closed` | fait, **relance par systemd à vérifier** | `BusWatch` (`include/tether/bus_watch.hpp`), `leave_for_closed_bus()` dans `src/daemon/main.cpp`, `ProfileSupervisor::bus_lost()` | `LANDLINE_BUS_WATCH=0` |
| 9.2 Profil refusé réessayé toutes les 2 s pendant 5 min, lien BR/EDR monté | fait, **coût pour l'iPhone à vérifier** | `ProfileSupervisor::tick()`, `PROFILE_FORBIDDEN_POLL_SECONDS`, `PROFILE_FORBIDDEN_FAST_SECONDS` | `LANDLINE_FORBIDDEN_FAST_SECONDS=0` |
| 9.2 Essais et notifications pendant une lecture longue | fait (réponse partielle à 5.7) | `TransferIdleScope` dans `obex_transfer.hpp`, `Tick::during_long_read()` dans `src/bluetooth/connection.cpp` | `LANDLINE_LONG_READ_ATTEMPTS=0` |
| 9.2 Session publiée dès l'ouverture ; commande `bt_retry_profiles` | fait | `ProfileSupervisor::set_opened_handler()`, `retry_now()`, `ConnectionManager::retry_profiles()` | — |
| 9.3 ANCS : découverte toutes les 2 s pendant 5 min, et dès que le service apparaît | fait | `ancs_retry_seconds()`, `AncsClient::retry_now()`, `ConnectionState::sync_ancs()` | `LANDLINE_ANCS_FIRST_SECONDS=0` |
| 10.2 ANCS : plus de `StopNotify` à la reconstruction ; abonné = `Notifying` lu ; pas de `StartNotify` dans les 35 s après un `StopNotify` | fait ; séquence fautive **prouvée par le source de BlueZ** | `ancs/subscription_guard.hpp`, `AncsClientState::release()`, `subscribe()` | `LANDLINE_ANCS_STOP_NOTIFY=0` (plus aucun `StopNotify`) ; pas de retour à l'ancien comportement |
| 10.3 LE d'abord : retenue de BR/EDR 10 s, recyclage à 12 s, préférence rendue à `le`, connexion LE sortante à 90 s | fait, **hypothèse non vérifiée** | `le_first.hpp`, `BearerSupervisor::supervise()`, `disconnect_all()` | `LANDLINE_LE_FIRST=0` ; `LANDLINE_LE_FIRST_HOLD_SECONDS`, `LANDLINE_LE_RECYCLE_SECONDS` |
| 10.4 Redémarrage de bluetoothd : nom suivi, annonce réenregistrée, sessions reconstruites | fait | `BluezMonitor::bluez_generation()`, `ConnectionState::bluez_restarted()`, `forget_ancs_solicitation()` | — |
| 10.5 Journal recréé si son dossier disparaît | fait | `log_file.hpp`, `keep_log_file()` dans `src/daemon/main.cpp` | — |
| 10.6 Sonde ANCS sans réponse : 5 s pendant 5 min, puis 60 s ; relève des messages réglable | fait | `unanswered_seconds()`, `message_poll_seconds()` | `LANDLINE_MESSAGE_POLL_SECONDS` |
| 7.3 Annonce : 20 ms pendant 180 s, puis 152,5 ms sans limite ; renouvellement avant expiration | fait | `advert_interval_for()`, `advert_renewal_due()`, `supervise_ancs_solicitation()` | `LANDLINE_ADVERT_FAST_SECONDS=30` |
| 7.4 Tentatives OBEX espacées sans lien | fait | `ProfileSupervisor::tick()` | `LANDLINE_OBEX_ABSENT_BACKOFF=0` |
| 7.5 État publié à jour dans le tour | fait | `run_supervision_tick()`, `run_quick_steps()` | — |

Tests unitaires ajoutés : `test/test_pending_connect.cpp`, `test/test_supervision_tick.cpp`,
`test/test_advert.cpp`, `test/test_reconnect_policy.cpp` (seconde série), et des cas dans
`test/test_bluetooth_supervisor.cpp` et `test/test_telephony.cpp`. 707 tests passent (670 avant la seconde
série).

Ce que les tests **ne couvrent pas**, faute de BlueZ et d'iPhone : l'émission réelle des appels D-Bus
asynchrones et leur rappel sur le fil GLib ; l'acceptation par BlueZ de `MinInterval` / `MaxInterval` et
l'intervalle réellement mis en ondes ; le réenregistrement de l'annonce à 30 s ; l'effet des 8 s sur un
profil mains-libres simplement lent ; tous les temps de reconnexion.

Temps de la première série, **mesurés** le 16 novembre (détail en 1 ter) :

| Depuis le retour du LE | Avant | Essai 1 (coupure côté PC) | Essai 2 (Bluetooth de l'iPhone coupé 55 s) |
|---|---|---|---|
| Messages et contacts | — | +2,2 s | +6,5 s |
| BR/EDR vu par le client | +31,3 s | +7,4 s | +11,4 s |
| Notifications actives | +46 s | +9,7 s | +2,4 s |
| Appels disponibles | +69 s | +7,4 s, sans cycle | +29,6 s, après un cycle |

Temps attendus après la seconde série (**déduits**, à mesurer) : notifications vers +3 à +5 s dans les deux
cas ; messages et contacts vers +1 à +2 s ; appels vers +7 à +9 s si le mains-libres vient sans cycle,
+18 à +20 s sinon, sans chute de l'état publié ni relecture du carnet.


## 1 ter. Les deux essais du 16 novembre, après la première série

Deux essais réels ont été faits sur `moteur-bluetooth-seul` avec l'iPhone. Temps comptés **depuis le retour
du lien LE** ; heures du journal du démon (`~/.local/state/landline/landlined.log`). **Mesuré**, sauf mention.

### Essai 1 — coupure depuis l'ordinateur (`bluetoothctl disconnect`), iPhone à côté

| Écart | Heure | Fait |
|---|---|---|
| — | 14:49:07,9 | BR/EDR puis LE tombent (`Reason.Local`) |
| ≈ 0 | 14:49:08,3 | `ancs: … (LE link dropped), rebuilding it` puis `Subscribing…` : première tentative d'abonnement, le LE n'est pas encore revenu, elle échoue |
| +2,2 s | — | messages et contacts ouverts (client abonné) |
| — | 14:49:12,9 | `pulled 767 contacts` : le fil a lu le carnet pendant ≈ 4,6 s |
| — | 14:49:13,6 | `pulled 100 recent calls` |
| +7,4 s | — | liens et appels disponibles, **sans** cycle mains-libres |
| — | 14:49:15,1 | `ancs: notification session 3 started` |
| +9,7 s | 14:49:17,6 | `Notification mirroring is active` |

Contre +32 s / +47 s / +70 s avant correction. Mais le carnet et les appels récents ont été relus **avant**
le retour des notifications.

**Pourquoi la correction 5.2 n'a pas joué.** Elle place ANCS en tête du tour de boucle, et c'est bien ce
qui s'est passé : la ligne `Subscribing…` précède toute lecture. Mais la session ANCS ne se rétablit pas en
un tour. Il en faut plusieurs : une tentative d'abonnement, qui échoue tant que le LE n'est pas revenu ;
une nouvelle tentative `ANCS_RETRY_SECONDS` (5 s) plus tard ; puis la sonde d'autorisation (≈ 2,5 s). Ici
la première tentative est tombée 0,4 s après la coupure, avant le retour du LE. Dans le même tour, les
sessions OBEX se sont rouvertes et le carnet a été lu : 4,6 s de fil occupé. La seconde tentative, due à
+5 s, n'a abouti qu'à 14:49:15. Le retard vient donc de deux choses que 5.2 ne touchait pas : le pas de
5 s entre deux tentatives, et le fait qu'aucune règle n'empêchait une lecture longue de démarrer **entre**
deux étapes d'ANCS.

L'essai 2 y a échappé pour une raison de calendrier : le LE est revenu seul, bien avant BR/EDR ; ANCS a eu
six secondes de fil libre avant que les sessions OBEX ne s'ouvrent.

### Essai 2 — Bluetooth coupé 55 s sur l'iPhone, puis rallumé

| Écart | Heure | Fait |
|---|---|---|
| — | 14:54:09,9 | LE puis BR/EDR tombent (`Reason.Remote`) |
| — | 14:54:12 à 14:54:58 | quatre paires de tentatives MAP/PBAP (`no_record`), toutes les ≈ 14 s, 2,5 à 3,9 s de fil chacune |
| — | 14:54:13,7 | annonce LE à 20 ms — 3,8 s après la perte, derrière la première paire de tentatives |
| — | 14:54:40,9 | `BR/EDR connect unanswered after 30s; BlueZ is still working on it` |
| — | 14:54:44,4 | annonce réenregistrée à 152-153 ms (fin de la fenêtre rapide) |
| 0 | ≈ 14:55:06,7 | LE revenu, `notification session 6 started` |
| +2,4 s | 14:55:09,2 | notifications actives |
| +6,5 s | ≈ 14:55:13,3 | MAP et PBAP ouverts : c'est l'échéance ordinaire des tentatives (15 s après celle de 14:54:57) |
| — | 14:55:15,9 | carnet lu (767), puis appels récents à 14:55:16,6 |
| +11,4 s | ≈ 14:55:18,2 | le client abonné voit `classic_connected=oui` |
| +19,5 s | 14:55:26,2 | `BR/EDR up for 8s without hands-free; dropping the BR/EDR link on purpose, once` |
| — | 14:55:28,9 | BR/EDR tombé (`Reason.Local`) : l'appel `Disconnect` a retenu le fil 2,6 s |
| — | 14:55:29,1 | `ListMessages failed … MAP session is not answering; reopening it` |
| — | 14:55:34,3 | carnet **relu en entier** (767), appels récents à 14:55:35,0 |
| +29,6 s | ≈ 14:55:36,4 | appels disponibles |

Lecture (**déduit** du code et des heures) :

- **`classic_connected` publié avec 5 s de retard.** L'état publié est construit à partir de la lecture des
  liens faite en début de tour. Le tour de 14:55:12 a lu les liens (BR/EDR absent), puis ouvert MAP et PBAP
  — obexd monte alors le lien BR/EDR —, publié l'état avec l'ancienne lecture, et lu le carnet et les
  appels récents. Le lien n'a été vu, et publié, qu'au tour suivant.
- **Le compte à rebours mains-libres en a hérité** : il part du tour où le superviseur *voit* BR/EDR, donc
  à +11,4 s et non à +6,5 s. D'où la coupure à +19,5 s.
- **Le lien BR/EDR est revenu par obexd, pas par une connexion demandée à BlueZ.** Rien dans le journal ne
  dit ce qu'est devenue la demande de connexion du moteur entre le retour du LE et +6,5 s : aucune ligne
  n'était écrite pour une demande émise ou une réponse « déjà en cours ». Deux explications compatibles
  avec le code, non départagées : la demande restée sans réponse à 14:54:40 était toujours en cours dans
  BlueZ, qui répondait `InProgress` aux suivantes ; ou un refus mémorisé pendant l'absence a été rendu à la
  première tentative après le retour, qui s'est alors mise en attente 5 s sans rien émettre.
- **Après la coupure volontaire**, la reconnexion émise par le moteur et la réouverture des sessions par
  obexd sont parties ensemble, et le mains-libres est venu en 7,5 s — comme dans l'essai 1, où les deux
  étaient aussi parties ensemble. C'est l'indice le plus net : le mains-libres vient quand BlueZ a reçu une
  vraie demande de connexion au moment où le lien monte, et ne vient pas quand obexd a monté le lien seul.

## 2. Les pièces en jeu

| Pièce | Rôle | Où |
|---|---|---|
| `ConnectionState::run()` | boucle de supervision, un tour par seconde, un seul fil | `src/bluetooth/connection.cpp:1394-1449` |
| `BearerSupervisor::tick()` | maintient les deux moitiés du lien (BR/EDR et LE) | `src/bluetooth/bearer_supervisor.cpp:117-282` |
| `BluezBearerOps` | les appels BlueZ correspondants | `src/bluetooth/connection.cpp:354-607` |
| `ProfileSupervisor::tick()` | ouvre les sessions messages (MAP) et contacts (PBAP) par obexd | `src/bluetooth/profile_supervisor.cpp:210-251` |
| `sync_ancs()` + `AncsClient::tick()` | abonnement aux notifications, traitement des paquets reçus | `connection.cpp:1208-1270`, `src/bluetooth/ancs/client.cpp:671-747` |
| `supervise_ancs_solicitation()` | met ou retire l'annonce LE qui fait revenir l'iPhone | `src/bluetooth/pairing.cpp:458-481`, `src/bluetooth/advert.cpp` |
| `sync_calls()` | lit la liste des appels en cours | `connection.cpp:1347-1362` |
| Constantes de temps | toutes regroupées | `include/tether/bluetooth/bearer_supervisor.hpp:8-46` |

**Ordre actuel d'un tour** (`run_supervision_tick()`) : liens → ANCS → appels → publication → sessions
OBEX → liens de nouveau si une session a bougé → verrouillage et annonce LE → publication → puis, si les
notifications n'ont pas la priorité (7.2) : carnet → journal d'appels → messages, les quatre étapes rapides
du début repassant après chaque lecture qui a retenu le fil (7.5). L'étape « liens » ne bloque plus (5.1).

Un tour de boucle **avant correction**, dans l'ordre (`connection.cpp:1398-1448`) :

1. `apply_ancs_preference()`, `apply_calls_preference()`
2. `bearers->tick()` — peut appeler `connect_classic()` : **bloquant jusqu'à 30 s, voire 60 s**
3. remise à zéro si le lien BR/EDR vient de tomber (`:1418-1422`)
4. `profiles->tick()` — `CreateSession` MAP puis PBAP : **bloquant jusqu'à 30 s chacun** (`:622-653`)
5. verrouillage à l'éloignement, puis gestion de l'annonce LE
6. `refresh_status()` + `publish()` — c'est ici seulement que les clients apprennent quelque chose
7. `sync_contacts()` — relecture complète du carnet à chaque nouvelle session PBAP : **bloquant**
8. `sync_call_history()` — **bloquant**
9. `sync_messages()` — liste des messages puis jusqu'à 20 corps de message : **bloquant**
10. `sync_ancs()` — abonnement et paquets de notification
11. `sync_calls()` — appels en cours
12. attente d'une seconde

Les paquets ANCS arrivent sur un autre fil (celui de GLib) mais y sont seulement mis en file ; ils ne sont
traités qu'à l'étape 10 (`ancs/client.cpp:714-734`). Toute attente aux étapes 2, 4, 7, 8 ou 9 retarde donc
aussi les notifications, même en régime établi.

## 3. D'où viennent les 30 s

`BearerSupervisor::tick()` voit le lien BR/EDR tombé et appelle `connect_classic()`
(`bearer_supervisor.cpp:170-172`). Celle-ci (`connection.cpp:411-438`) :

1. si l'appareil est encore « connecté » (le LE est revenu) et expose `Bearer.BREDR1`, appelle
   `Bearer.BREDR1.Connect`, **en synchrone, 30 s** ;
2. si cet appel échoue autrement que par `InProgress` / `AlreadyConnected`, enchaîne sur `Device1.Connect`,
   **en synchrone, 30 s de plus** ;
3. rend l'erreur du premier appel si le second échoue aussi (`:435-436`).

Les deux passent par `call()` (`:578-602`), dont le délai par défaut est `CONNECT_TIMEOUT_MS`.

Quand BlueZ ne répond pas dans le délai, GLib rend « Timeout was reached ». Le superviseur classe alors
l'erreur (`bearer_supervisor.cpp:19-70`) : ce texte n'est ni dans la liste des erreurs passagères
(`is_transient`, `:19`), ni dans « déjà en cours » (`is_already_running`, `:38`), ni dans « téléphone
absent » (`is_absent`, `:52` — qui cherche « Connection timed out », pas « Timeout was reached »). Il tombe
donc dans le cas général : `grow_backoff(0)` = 5 s (`:12-16`), d'où la ligne
« BR/EDR connect failed (Timeout was reached), retrying in 5s » (`:188-189`), et un échec de plus au
compteur `classic_failures_`.

Deux remarques **lues** dans le code :

- Le fichier connaît le problème et l'a résolu pour l'autre moitié du lien. La connexion LE est émise en
  asynchrone, avec ce commentaire : un appel synchrone qui expire laisse BlueZ continuer à se connecter, et
  la tentative suivante se heurte à la précédente (`connection.cpp:303-307`, `:478-527`). La connexion
  BR/EDR fait exactement ce que ce commentaire déconseille.
- Le même délai de 30 s sert à `CreateSession` (`:639`). Quand l'iPhone est réellement absent, chaque
  tentative d'ouverture de session attend l'échec de la connexion d'obexd, pour MAP puis pour PBAP, toutes
  les 15 s (`PROFILE_STEADY_POLL_SECONDS`, `include/tether/bluetooth/profile_supervisor.hpp:10`). Le fil de
  supervision passe alors une bonne partie de son temps bloqué, et réagit d'autant plus tard au retour du
  téléphone.

## 4. La séquence du 16 novembre, reconstituée

Relevé `bench/chrono-reconnexion.log` (**mesuré**), t0 = 13:15:06,9 :

| Heure | Écart | Fait mesuré |
|---|---|---|
| 13:15:06,927 | t0 | tout tombe |
| 13:15:07,563 | +0,6 s | LE revenu |
| 13:15:38,825 | +31,9 s | BR/EDR revenu |
| — | +47 s | notifications actives (journal) |
| 13:16:13,816 | +66,9 s | BR/EDR retombe |
| 13:16:15,240 | +68,3 s | BR/EDR revenu |
| — | +70 s | téléphonie active (journal) |

Lecture proposée, tour de boucle par tour de boucle :

| Écart | Ce que fait le fil de supervision | Statut |
|---|---|---|
| +0,6 s | L'iPhone rouvre le lien LE de lui-même, en réponse à l'annonce. Rien à corriger ici | mesuré |
| ≈ +1 s | Tour A, étape 2 : BR/EDR vu tombé, `connect_classic()` part et **bloque** | lu |
| ≈ +31 s | Expiration : « Timeout was reached, retrying in 5s » | mesuré |
| ≈ +31 s | Tour A, étape 3 : le lien était prêt, il ne l'est plus : `profiles->reset()`, donc nouvelle tentative immédiate | lu |
| +31,9 s | Tour A, étape 4 : `CreateSession` MAP. obexd ouvre lui-même la liaison : **BR/EDR revient en moins d'une seconde** | déduit (la coïncidence à 0,9 s près le rend très probable) |
| +32 à +47 s | Tour A, étapes 7 à 9 : relecture du carnet, du journal d'appels, des messages | déduit |
| ≈ +47 s | Tour A, étape 10 : la coupure LE a invalidé l'abonnement (`connection.cpp:1264-1268`) ; `AncsClient::tick()` se réabonne enfin | lu + mesuré |
| ≈ +47 s | Tour B, étape 2 : premier tour où le superviseur **voit** BR/EDR actif sans téléphonie ; le compte à rebours de 20 s démarre (`bearer_supervisor.cpp:217-221`) | lu |
| +66,9 s | 20 s plus tard : `Bearer.BREDR1.Disconnect` par le moteur lui-même (`:221-224`). **Aucune ligne de journal** quand l'appel réussit | lu ; l'écart de 20 s exactement est mesuré |
| +68,3 s | Reconnexion fraîche, qui cette fois apporte le profil mains-libres | mesuré |
| ≈ +70 s | Téléphonie active | mesuré |

Tout concorde, y compris l'écart de 20 s entre +47 et +67. Deux points restent **déduits** et demandent une
capture :

- **Pourquoi BlueZ n'a pas répondu pendant 30 s.** Tether a mesuré un `Device1.Connect` à froid en 1 à 2 s
  (`docs/BLUETOOTH.md`, 22 août 2026). Pistes : la politique de reconnexion propre à BlueZ
  (`[Policy] ReconnectUUIDs`, nouvelles tentatives à 1, 2, 4… s après une perte de signal) qui se
  télescope avec notre appel ; ou BlueZ qui n'envoie sa réponse qu'une fois tous les profils connectés et
  reste suspendu à l'un d'eux (mains-libres ou audio côté PipeWire).
- **Lequel des deux appels a expiré.** Selon que le premier tour a eu lieu avant ou après le retour du LE
  (0,6 s), c'est `Bearer.BREDR1.Connect` ou `Device1.Connect`. Dans le second cas, le code écrit aussi
  `PreferredBearer=bredr` juste avant (`connection.cpp:430-431`), au moment précis où l'iPhone rouvre le
  LE.

Capture à faire pour trancher (aucune modification du système) : `btmon -w`, `busctl --system monitor
org.bluez` et le journal du démon en parallèle, puis couper et rétablir le lien.

## 5. Corrections proposées

Classées par rapport gain/risque. Les gains chiffrés sont **déduits** de la reconstitution ci-dessus.

### 5.1 Rendre la connexion BR/EDR asynchrone — gain fort, risque faible

**État : fait.** `connect_classic()` enregistre la demande (`PendingConnect::begin()`), émet l'appel sur le
fil GLib du moniteur et rend la main ; le rappel `on_classic_connected()` classe la réponse
(`classify_classic_reply()`) et enchaîne lui-même sur `Device1.Connect` si `Bearer.BREDR1.Connect` est
refusé. Tant que la réponse n'est pas arrivée, le superviseur ne retente pas et ne fait pas grossir son
délai (`classic_connect_outstanding()`). Un refus est relevé au tour suivant ; le délai de nouvelle
tentative et le compteur de refus continuent de croître d'une tentative à l'autre (ils ne sont plus remis à
zéro par une demande simplement émise). Si le lien revient par un autre chemin, l'erreur de la tentative
abandonnée est oubliée. `PreferredBearer=bredr` n'est toujours jamais écrit pendant une connexion LE en
cours. Non vérifié sans matériel : le comportement réel de BlueZ quand la demande asynchrone croise une
connexion ouverte par obexd.

**Quoi.** Émettre `Bearer.BREDR1.Connect` / `Device1.Connect` avec `g_dbus_connection_call` (asynchrone),
comme le fait déjà `connect_le()` : drapeau « en cours », rendre `ConnectResult::Busy` tant que la réponse
n'est pas arrivée, relever l'erreur au tour suivant. Le modèle est sous les yeux
(`connection.cpp:478-560`, structure `LeConnect`).

**Où.** `connection.cpp:411-438` ; rien à changer dans `BearerSupervisor`, qui gère déjà `Busy`
(`bearer_supervisor.cpp:178-183`).

**Gain.** Supprime les 30 s d'aveuglement. À lui seul : BR/EDR vers +1 à +2 s (par la session messages),
notifications vers +15 s, téléphonie vers +40 s.

**Risque.** Faible : le comportement en cas de `Busy` existe et est testé
(`test/test_bluetooth_supervisor.cpp`). Point d'attention : ne jamais écrire `PreferredBearer` tant qu'une
connexion est en cours (le code le fait déjà pour le LE, `:429-431`).

### 5.2 Servir ANCS et les appels avant les lectures longues — gain fort, risque faible

**État : fait.** L'ordre est fixé par `run_supervision_tick()` et vérifié par
`test/test_supervision_tick.cpp`. Une publication de l'état a été ajoutée juste après ANCS et les appels :
les clients apprennent le retour des notifications sans attendre l'ouverture des sessions OBEX. Effet de
bord voulu : `le_down_since` et `ancs_absent_since` sont désormais à jour du tour courant quand l'annonce
LE est décidée (ils dataient du tour précédent).

**Quoi.** Dans `run()`, déplacer `sync_ancs()` et `sync_calls()` juste après `bearers->tick()`, avant
`profiles->tick()` et avant `sync_contacts()` / `sync_messages()`.

**Où.** `connection.cpp:1404-1444`.

**Gain.** Avec 5.1, notifications actives en 2 à 6 s après le retour du LE au lieu de +47 s. Supprime aussi
le retard des notifications pendant une relecture de carnet en régime établi.

**Risque.** Faible : simple réordonnancement. Vérifier que `sync_ancs()` ne dépend de rien que
`profiles->tick()` calcule (lu : il ne lit que l'état des liens et le moniteur BlueZ).

### 5.3 Rétablir BR/EDR par une vraie connexion avant de laisser obexd le faire — gain fort, risque moyen

**État : non fait.** Les essais du 16 novembre montrent que le mains-libres vient sans retenir OBEX quand
la demande de connexion et l'ouverture des sessions partent ensemble (essai 1, et après le cycle de
l'essai 2). La seconde série mise là-dessus (7.1) ; retenir OBEX reste en réserve.

**Quoi.** Aujourd'hui le lien revient par la session messages, donc sans mains-libres, ce qui coûte 20 s
d'attente plus une coupure. Après une perte de lien, émettre la connexion (asynchrone, 5.1) et **retenir
l'ouverture des sessions OBEX** tant qu'elle est en cours, avec un plafond court (3 à 5 s). Une connexion
fraîche demandée par l'ordinateur apporte tous les profils d'un coup (`docs/BLUETOOTH.md`, 6 septembre
2026).

**Où.** `connection.cpp:1427` (condition d'appel de `profiles->tick()`), avec un nouvel état « connexion
BR/EDR en cours » exposé par `BluezBearerOps`.

**Gain.** Téléphonie en quelques secondes, sans la coupure volontaire ni la perte des sessions messages et
contacts qu'elle entraîne.

**Risque.** Moyen. Tether a volontairement cessé de conditionner OBEX à l'état du lien (23 août 2026) : sur
une machine sans profil audio local, `Device1.Connect` ne peut pas réussir. D'où le plafond court, à
garder impérativement. Dépend aussi de la réponse à « pourquoi BlueZ n'a pas répondu pendant 30 s » : si la
connexion fraîche reste suspendue sur cette machine, cette correction n'apporte rien et il faut se rabattre
sur 5.4.

### 5.4 Raccourcir et journaliser le cycle mains-libres — gain moyen, risque faible à moyen

**État : fait.** Depuis la seconde série, une demande directe du profil précède la coupure, et la coupure
ne fait plus retomber l'état publié ni relire le carnet (7.1). (a) La coupure écrit maintenant, au niveau INFO :
`bluetooth: BR/EDR up for 8s without hands-free; dropping the BR/EDR link on purpose, once, so the
reconnect brings call control`. (b) `HFP_ABSENT_SECONDS` vaut 8. La valeur n'a pas été réglée sur mesure :
si l'essai montre un profil mains-libres qui arrive entre 8 et 20 s après le lien, elle coupe pour rien
(une fois par panne) et il faut la remonter — `LANDLINE_HFP_ABSENT_SECONDS=<n>` (3 à 120) le permet sans
recompiler.

**Quoi.** (a) Écrire une ligne de journal quand le moteur coupe lui-même le lien BR/EDR : aujourd'hui seul
l'échec est journalisé (`bearer_supervisor.cpp:221-224`), ce qui rend la coupure indiscernable d'une panne.
(b) Ramener `HFP_ABSENT_SECONDS` de 20 s à 6-8 s.

**Où.** `include/tether/bluetooth/bearer_supervisor.hpp:37`, `src/bluetooth/bearer_supervisor.cpp:217-229`.

**Gain.** (a) diagnostic immédiat, risque nul. (b) 12 à 14 s de moins avant la téléphonie quand le cycle
reste nécessaire.

**Risque.** (b) moyen : un délai trop court coupe un lien dont le profil mains-libres arrivait simplement
en retard, au prix des sessions messages et contacts. Le cycle n'a lieu qu'une fois par panne
(`hfp_cycle_spent_`), ce qui borne le dégât. À régler sur mesure, pas à l'estime.

### 5.5 Classer « Timeout was reached » comme « en cours » — gain faible, risque faible

**État : fait**, à la source : le rappel asynchrone reconnaît `G_IO_ERROR_TIMED_OUT`, ne rapporte aucun
refus et écrit `BR/EDR connect unanswered after 30s; BlueZ is still working on it`. La tentative suivante
reçoit `InProgress` de BlueZ, déjà classé « en cours ».

**Quoi.** Un délai D-Bus expiré ne dit pas que l'iPhone refuse : BlueZ travaille encore. Le traiter comme
`Busy` et non comme un refus.

**Où.** `bearer_supervisor.cpp:38-43` (`is_already_running`), ou mieux, à la source dans `call()` avec
`g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)` comme le fait déjà `advert.cpp:170`.

**Gain.** Évite de compter des refus fictifs : au bout de six, le moteur affiche « l'iPhone refuse la
connexion, éteignez puis rallumez son Bluetooth » (`bearer_supervisor.cpp:203-212`), conseil faux ici.
Devient sans objet si 5.1 est fait.

### 5.6 Régler l'intervalle de l'annonce LE — gain moyen (iPhone en veille), risque faible

**État : fait, à valider par `btmon`.** L'objet `LEAdvertisement1` déclare `MinInterval` et `MaxInterval`
(type `u`, millisecondes). Rapide = 20/20 ms pendant les 30 s qui suivent la perte du lien LE
(`advert_interval_for()`, à partir de `le_down_since`, donc aussi pendant les 30 premières secondes après
le démarrage du démon si le LE est tombé) ; ensuite 152/153 ms. BlueZ n'accepte que des millisecondes
entières et les convertit par troncature en pas de 0,625 ms : 152 ms donne 243 pas (151,875 ms), 153 ms
donne 244 pas (152,5 ms exactement) ; le contrôleur choisit entre les deux. Au passage de rapide à normal,
`supervise_ancs_solicitation()` réenregistre l'annonce. L'annonce posée par l'appairage ou par `bt_solicit`
part directement à 152/153 ms.

Deux protections : si BlueZ refuse l'annonce avec intervalle, elle est aussitôt réenregistrée sans, le
moteur le retient pour la suite et écrit `BlueZ refused the advertising interval (…); soliciting without
one` ; et `LANDLINE_ADVERT_INTERVALS=0` rend l'annonce d'origine (les deux propriétés ne sont alors même plus
déclarées). Non vérifié sans matériel : que le noyau applique bien l'intervalle (hors mode expérimental,
BlueZ ignore ces propriétés en silence) ; que le réenregistrement à 30 s ne laisse ni trou notable ni
enregistrement fantôme. Seconde série (7.3) : la phase rapide dure 180 s et non plus 30, et le point « À
corriger en même temps » ci-dessous (expiration à 180 s) est traité par un renouvellement anticipé.

**Constat (lu).** L'annonce est enregistrée sans aucun paramètre d'intervalle : options vides à
l'enregistrement (`src/bluetooth/advert.cpp:154-166`), et ni `MinInterval` ni `MaxInterval` parmi les
propriétés déclarées (`advert.cpp:28-40`, `:66-104`). BlueZ applique donc le défaut du noyau, 1,28 s.
Apple recommande 20 ms pendant les 30 premières secondes, puis 152,5 ms (ou 211,25, 318,75, 417,5, 546,25,
760, 852,5, 1022,5, 1285 ms).

**Quoi.** Ajouter les propriétés `MinInterval` et `MaxInterval` (en millisecondes) à l'objet
`LEAdvertisement1` : 20 ms à la mise en ondes, puis réenregistrer l'annonce à 152,5 ms au bout de 30 s
(BlueZ ne relit les propriétés qu'à l'enregistrement). Ces deux propriétés sont marquées expérimentales
dans BlueZ ; `bluetoothd --experimental` est déjà un prérequis, et l'adaptateur de cette machine annonce
`CanSetTxPower`, signe que le noyau accepte les paramètres d'annonce étendus.

**Où.** `advert.cpp:28-40` (déclaration), `:66-104` (valeurs), et `pairing.cpp:361-385` /
`:458-481` pour le passage de 20 ms à 152,5 ms.

**Gain.** Aucun sur l'essai du 16 novembre : le LE est revenu en 0,6 s, iPhone éveillé. Le gain attendu est
ailleurs : iPhone verrouillé ou en veille, où il n'écoute que par courtes fenêtres et où une annonce toutes
les 1,28 s peut être manquée plusieurs fois de suite. C'est la mesure que personne n'a publiée (voir
`iphone-link-study/02-protocoles-reconnexion.md`, §3) : à faire avant et après.

**Risque.** Faible. Consommation radio plus élevée pendant 30 s seulement. Trois points à vérifier par
`btmon` (« LE Set Extended Advertising Parameters ») : que l'intervalle demandé est bien appliqué ; que
152,5 ms, qui n'est pas un nombre entier de millisecondes, est arrondi de façon acceptable (sinon, régler
`MinAdvertisementInterval` / `MaxAdvertisementInterval` dans la section `[LE]` de
`/etc/bluetooth/main.conf`, en unités de 0,625 ms, soit 244 — réglage global, droits administrateur) ; que
le réenregistrement ne laisse pas d'enregistrement fantôme (`AlreadyExists`, déjà géré en
`advert.cpp:173-177`).

**À corriger en même temps.** L'annonce expire au bout de 180 s (`ANCS_ADVERT_TIMEOUT_SECONDS`,
`include/tether/bluetooth/advert.hpp:11`) et n'est remise en ondes qu'au tour suivant du superviseur ; si
celui-ci est bloqué (section 3) ou si l'enregistrement échoue, elle reste absente jusqu'à 30 s
(`SOLICIT_RETRY_SECONDS`, `pairing.cpp:38`). Pendant une absence longue, c'est la seule chose qui fait
revenir l'iPhone.

### 5.7 Sortir les lectures longues du fil de supervision — gain fort, risque élevé

**Quoi.** Donner à OBEX (ouverture de session, carnet, messages) son propre fil, et ne laisser au
superviseur que des décisions rapides. C'est la vraie réparation ; 5.1 et 5.2 en sont la version économe.

**Risque.** Élevé : l'état partagé (`ConnectionState`) est conçu pour un seul écrivain, les commentaires
du code le répètent (`connection.cpp:813-816`). À faire après les corrections simples, avec les tests du
superviseur comme filet.

### 5.8 Petits gains

- Réveiller la boucle sur événement BlueZ au lieu d'attendre le tour suivant (jusqu'à 1 s) :
  `connection.cpp:1446-1447`, le moniteur reçoit déjà les changements de propriétés.
- Resserrer `ANCS_RETRY_SECONDS` (5 s, `include/tether/bluetooth/ancs/client.hpp:16`) pendant les 30 s qui
  suivent un retour de lien : découverte, abonnement et sonde d'autorisation avancent aujourd'hui par pas
  de 5 s. **Fait** pour la découverte et l'abonnement (1 s pendant 20 s, voir 7.2) ; la sonde
  d'autorisation reste à 5 s.

### Ordre conseillé

1. 5.4 (a) — une ligne de journal, pour voir ce que fait le moteur. *Fait.*
2. Capture `btmon` d'une coupure brève, pour trancher les deux points déduits. *Reste à faire : c'est le
   premier pas de `essai-reconnexion.md`.*
3. 5.1 puis 5.2, mesurer. *Écrits ; mesure à faire.*
4. 5.6, avec la mesure iPhone éveillé / en veille. *Écrit ; mesure à faire.*
5. 5.3 ou 5.4 (b) selon ce que montre la capture. *5.4 (b) fait d'avance à 8 s, réglable ; 5.3 en attente.*

## 6. Les deux anomalies d'appel

### 6.1 Un appel entrant sonne sur l'ordinateur sans trace ni dans le journal ni dans la liste

**Pourquoi l'ordinateur sonne quand même.** La sonnerie ne vient pas du moteur. PipeWire tient le profil
mains-libres (rôle `hfp_hf` par défaut de WirePlumber) et joue lui-même le son que l'iPhone lui envoie. Le
moteur ne fait qu'observer `org.pipewire.Telephony` sur le bus de session.

**Depuis la branche `moteur-bluetooth-seul`** : chaque transition d'appel écrit une ligne INFO, numéro
masqué (`call: incoming from ***67 (incoming)`, `call: answered, …`, `call: ended, …`), et la liste des
appels est lue avant les lectures longues. Si un appel sonne de nouveau sans ligne dans le journal, les
causes 1 et 3 ci-dessous restent seules en lice (contrôle d'appel désactivé, source PipeWire en sommeil),
avec une fenêtre résiduelle pour la cause 2 pendant l'ouverture d'une session OBEX. Le texte qui suit
décrit l'état d'avant.

**Pourquoi il n'y a rien dans le journal (lu).** Le moteur n'écrit **aucune** ligne pour un appel qui
arrive, change d'état ou se termine. `sync_calls()` compare et publie sans journaliser
(`connection.cpp:1347-1362`) ; le gestionnaire du démon diffuse `bt_calls` sans journaliser
(`engine/src/daemon/main.cpp:771-778`) ; les seules lignes « telephony: » du code concernent des échecs
(`src/bluetooth/telephony.cpp:209`, `:237`). La téléphonie ne déclenche pas non plus de bulle : seule
l'interface GTK montre l'appel. La seule bulle possible est celle de la notification ANCS « appel entrant »,
si les notifications sont actives à ce moment-là. L'absence de trace dans le journal ne prouve donc pas que
l'appel n'a pas été vu.

**Pourquoi il peut manquer dans la liste (lu).** Il y a deux listes :

- les appels en cours (`bt_list_calls`) : lue à la demande dans PipeWire ; vide dès que l'appel est fini ;
- le journal d'appels (`bt_list_call_history`) : relu sur l'iPhone par PBAP seulement dans deux cas — une
  nouvelle session contacts (`connection.cpp:1120`), ou quand `sync_calls()` **constate lui-même** qu'un
  appel a disparu de la liste (`connection.cpp:1357-1358`).

La détection repose donc sur une interrogation, une fois par tour de boucle, à la dernière étape. Causes
possibles, de la plus probable à la moins probable :

1. **Le contrôle d'appel n'était pas activé à ce moment-là.** Il est désactivé par défaut
   (`include/tether/bluetooth/config.hpp`, `calls_enabled = false`). Sans lui, aucun client de téléphonie
   n'est créé (`connection.cpp:1328-1345`) et `sync_calls()` ne fait rien — alors que PipeWire, lui, sonne
   toujours. À vérifier en premier : `calls_enabled` dans `bt_status` à l'heure de l'appel.
2. **Le fil de supervision était occupé pendant toute la sonnerie.** Un appel non décroché dure 15 à 30 s ;
   un seul appel bloquant de 30 s (section 3) ou une relecture de carnet suffit à ne faire aucun tour
   pendant ce temps. L'appel naît et meurt entre deux lectures : pas d'événement `bt_calls`, pas de drapeau
   « journal périmé », donc pas de relecture du journal d'appels. C'est le cas si l'appel est arrivé dans la
   minute qui suit une reconnexion.
3. **La source PipeWire était en sommeil.** Si le service `org.pipewire.Telephony` a répondu « inconnu » une
   fois, le moteur ne le réinterroge plus pendant 30 s (`DORMANT_SECONDS`,
   `src/bluetooth/telephony_pipewire.cpp:25`, `:151-154`).
4. **Le journal d'appels est refusé par l'iPhone.** Si « Synchroniser les appels récents » est désactivé
   sur l'iPhone, la relecture échoue — mais cela écrit une ligne « call history unavailable »
   (`connection.cpp:1166-1168`), donc ce n'est pas le cas observé.

**Corrections.**

- Journaliser chaque transition d'appel (arrivée, décroché, fin) dans `sync_calls()`. Risque nul ; c'est ce
  qui manque pour trancher entre les causes ci-dessus. **Fait.**
- S'abonner aux signaux `InterfacesAdded` / `InterfacesRemoved` de `org.pipewire.Telephony` au lieu
  d'interroger : l'événement arrive alors même si le fil de supervision est occupé. Le moniteur BlueZ le
  fait déjà pour la téléphonie de BlueZ (`src/bluetooth/monitor.cpp:385-407`).
- Marquer le journal d'appels « périmé » sur d'autres indices que la seule liste : notification ANCS de
  catégorie 1 (appel entrant) ou 2 (appel manqué), qui arrive par le LE indépendamment de la téléphonie.
- Faire diffuser un événement d'appel entrant par le démon dès la notification ANCS de catégorie 1, pour
  que la pastille puisse proposer « répondre / refuser » même si la téléphonie n'est pas encore revenue.
- Avec 5.2 (appels lus avant les lectures longues), la cause n° 2 disparaît presque entièrement. **Fait.**

### 6.2 Composer depuis l'ordinateur échoue et fait tomber BR/EDR

**Chemin de la commande (lu).** `bt_call_dial` (`src/net.cpp:1625-1636`) → `ConnectionManager::dial`
(`connection.cpp:1646-1653`) → `TelephonyClient::dial` (`src/bluetooth/telephony.cpp:241-259`) : le numéro
est normalisé (`:33-58`), puis passé **tel quel, sans préfixe `tel:`** (`dial_argument`, `:127-129`) à
`org.pipewire.Telephony.AudioGateway1.Dial` par un appel D-Bus synchrone de 10 s (`:179-214`). C'est tout :
une quinzaine de lignes, sans état.

`org.pipewire.Telephony.Error.Failed` est donc la réponse de PipeWire, pas une erreur du moteur : PipeWire a
envoyé la commande de numérotation à l'iPhone et a reçu un refus, ou n'a pas pu l'envoyer. Le message
« Dial failed: … » est seulement le relais de la ligne de commande.

**Ce que le code du moteur permet d'établir.**

- Le moteur déclare la téléphonie PipeWire « prête » dès que l'objet passerelle existe : l'état est forcé à
  `connected` (`src/bluetooth/telephony_pipewire.cpp:193`), et `dial()` ne vérifie de toute façon pas
  `available()`. Si la liaison mains-libres n'avait pas fini de s'établir — par exemple dans les secondes
  qui suivent le cycle décrit en 5.4 — la numérotation part trop tôt.
- **La chute de BR/EDR peut venir du moteur lui-même.** Si l'échec fait disparaître la passerelle de
  PipeWire, le superviseur voit « BR/EDR actif sans téléphonie », attend 20 s et coupe le lien
  (`bearer_supervisor.cpp:217-224`), **sans rien écrire dans le journal**. Le relevé du banc montre une
  chute de BR/EDR à 13:36:46,9 avec le LE resté actif, ce qui est compatible avec cette coupure ciblée
  (elle n'emploie que `Bearer.BREDR1.Disconnect`) sans la prouver. À confirmer en comparant l'heure de la commande à celle
  de la chute : 20 s d'écart désigneraient le moteur ; une chute immédiate désignerait l'iPhone ou PipeWire.
- La commande s'exécute sur la boucle d'événements du démon : pendant les 10 s d'attente, aucun client
  n'est servi.

**Ce que le code ne permet pas d'établir.** La cause du refus lui-même. Elle se trouve dans l'échange entre
PipeWire et l'iPhone, hors du moteur.

**Pour trancher, sans toucher au moteur.**

1. Reproduire avec `btmon` et le journal de WirePlumber (catégorie `spa.bluez5.native`) : on y lit la
   commande envoyée et la réponse de l'iPhone.
2. Appeler PipeWire directement, moteur arrêté :
   `busctl --user call org.pipewire.Telephony <chemin de la passerelle>
   org.pipewire.Telephony.AudioGateway1 Dial s "+41…"`. Même échec : le moteur est hors de cause.
3. Essayer l'autre pile : retirer `hfp_hf` des rôles WirePlumber donne la téléphonie à BlueZ
   (`org.bluez.Telephony1`). Tether la décrit comme fiable pour le contrôle d'appel, au prix du son, qui
   reste sur l'iPhone (`docs/BLUETOOTH.md`, « Calls »).

**Corrections côté moteur, quelle que soit la cause.**

- Journaliser la coupure volontaire de BR/EDR (5.4 a). **Fait.** Attention : avec le délai ramené à 8 s,
  une passerelle PipeWire qui disparaît après un échec de numérotation fait couper le lien plus vite
  qu'avant ; la ligne de journal permet maintenant de le voir.
- Ne pas déclencher le cycle mains-libres dans les secondes qui suivent une commande d'appel.
- Lire l'état réel de la passerelle PipeWire au lieu de le forcer à `connected`, et refuser de composer
  tant qu'elle n'est pas prête, avec un message clair.
- Exécuter les commandes d'appel hors de la boucle d'événements.

## 7. Deuxième série de corrections (branche `moteur-reconnexion-2`)

Écrites d'après les deux essais de la section 1 ter. **Compilées et couvertes par des tests unitaires ;
aucune n'a été essayée avec l'iPhone.** Tout temps annoncé ici est déduit.

### 7.1 Appels : le cycle « attendre 8 s, couper, tout relire »

**Peut-on demander le mains-libres sans couper le lien ?** Ce qui est établi :

- Tether l'a mesuré le 6 septembre 2026 (`engine/docs/BLUETOOTH.md`, « Hands-free after a phone-initiated
  reconnect »), iPhone reconnecté de lui-même, lien déjà monté : `Device1.Connect()` rend un succès et ne
  fait rien ; `ConnectProfile("0000111e…")` échoue (`No more profiles to connect to` : BlueZ compare à la
  liste de l'iPhone, qui annonce `111f`) ; `ConnectProfile("0000111f…")` rend un succès et ne fait rien.
  Même constat dans la section sur PipeWire. Seule la coupure de `Bearer.BREDR1` rétablissait le profil.
- `org.pipewire.Telephony` n'offre aucune méthode de connexion : la passerelle (`AudioGateway1`) n'existe
  qu'une fois la liaison mains-libres établie. Il n'y a rien à appeler de ce côté.
- Sur cette machine le profil mains-libres appartient à PipeWire. **De mémoire du code de PipeWire, non
  vérifié ici** : il enregistre son profil auprès de BlueZ sans connexion automatique, la liaison étant
  ouverte par le téléphone. Cela expliquerait à la fois que `Device1.Connect()` ne fasse rien sur un lien
  déjà monté et que le profil vienne « tout seul » ≈ 6 s après une connexion demandée par l'ordinateur :
  c'est l'iPhone qui l'ouvre, quand il voit l'ordinateur se connecter comme appareil audio.

Conclusion : pas de moyen sûr de demander le profil sur un lien déjà monté. Ce qui est fait :

1. **Une demande directe, quand même, avant de couper** (`BearerOps::request_hands_free()`). Cinq secondes
   après que BR/EDR est vu actif sans mains-libres, le moteur émet `Device1.ConnectProfile` avec l'UUID de
   l'iPhone (`0000111f-…`), en asynchrone. La réponse de BlueZ est journalisée (`BlueZ accepted / declined
   the hands-free request`). Si le profil arrive, le cycle n'a pas lieu ; sinon la coupure suit à 8 s, à
   l'heure habituelle. Coût si elle ne fait rien, comme l'a mesuré Tether : nul. À 5 s et non plus tôt,
   pour ne pas croiser le profil que l'iPhone ouvre de lui-même vers 6 s dans le bon cas.
   Retour arrière : `LANDLINE_HFP_CONNECT_PROFILE=0`.
2. **Ne plus arriver dans cet état.** Au retour du lien LE, le refus mémorisé d'une tentative faite pendant
   l'absence est oublié (`forget_classic_refusal()`), pour que la première tentative atteigne BlueZ ; et
   les sessions OBEX se rouvrent dans le même tour (7.4), donc en même temps que la demande de connexion,
   comme dans les deux cas où le mains-libres est venu sans cycle. **C'est l'hypothèse centrale de cette
   série, et elle n'est pas vérifiée** : si BlueZ répond encore `InProgress` à ce moment-là, le lien montera
   de nouveau par obexd seul.
3. **Le journal dit maintenant ce qui s'est passé** : `asking BlueZ for the BR/EDR link (Bearer.BREDR1.Connect
   | Device1.Connect)`, `BlueZ answered the BR/EDR connect: …`, `BR/EDR connect already running in BlueZ
   (…), standing back Ns`. Le prochain essai départagera les deux explications de la section 1 ter.
4. **Si la coupure a lieu, elle coûte moins.**
   - Elle part plus tôt : le superviseur voit le lien dès que les sessions sont ouvertes (7.5), soit ≈ 5 s
     de gagné sur l'essai 2.
   - L'état publié ne retombe plus : pendant 20 s au plus (`HFP_CYCLE_MASK_SECONDS`), ou jusqu'au retour
     des sessions, `map_open` et `pbap_open` restent à vrai (`mask_planned_drop()`). `classic_connected`
     dit toujours la vérité. Une commande envoyée pendant ces quelques secondes peut échouer.
   - Le carnet n'est plus relu (`plan_contacts()`). À l'ouverture d'une session PBAP, le moteur demande à
     l'iPhone son nombre de fiches (`GetSize`, un échange d'en-têtes) : carnet lu depuis moins de 10 min et
     nombre identique, il est gardé ; nombre différent, il est relu aussitôt ; carnet plus ancien, il est
     relu 5 min plus tard, une fois la reconnexion passée ; nombre inconnu, relu 5 min plus tard aussi.
     Limite assumée : une fiche modifiée sans changement du nombre, dans les 10 minutes, n'est vue qu'à la
     session suivante. Les appels récents (0,7 s) sont toujours relus à chaque session.
     Retour arrière : `LANDLINE_CONTACTS_ALWAYS_PULL=1`.

**Non fait** : retenir l'ouverture d'OBEX le temps d'une vraie connexion (5.3). L'essai 1 montre que le
mains-libres vient même quand obexd ouvre ses sessions au même moment ; retenir OBEX coûterait 2 à 4 s sur
les messages pour un gain non démontré. À reconsidérer si le prochain essai montre encore un lien monté par
obexd seul.

Temps attendus sur l'essai 2 (**déduits**) : si l'hypothèse du point 2 tient, appels vers +7 à +9 s comme
dans l'essai 1. Sinon, coupure vers +10 s et appels vers +18 à +20 s, sans chute de l'état publié et sans
relecture du carnet.

### 7.2 Ordre : ANCS ne passe plus derrière une lecture du carnet

- **Les lectures longues attendent ANCS** (`NotificationPriority`, `long_reads_allowed()` dans
  `run_supervision_tick()`). Dès qu'une session ANCS en service est perdue, et de nouveau au retour du lien
  LE, carnet, appels récents et messages ne sont plus lus tant que les notifications ne sont pas prêtes,
  12 s au plus. L'ouverture des sessions OBEX, elle, n'attend pas : c'est elle qui ramène BR/EDR.
  Un iPhone dont les notifications ne deviennent jamais prêtes (partage désactivé) n'obtient qu'une attente,
  pas une par battement du lien LE : sans cela les messages ne seraient plus relevés.
- **Le pas de nouvelle tentative passe de 5 s à 1 s** pendant les 20 s qui suivent une perte
  (`ancs_retry_seconds()`), pour la découverte et l'abonnement. C'était le premier petit gain de 5.8.
- Vaut pour les deux chemins : dans l'essai 1 les lectures auraient attendu ; dans l'essai 2 ANCS était
  prêt avant l'ouverture des sessions, l'attente ne joue pas.
- Retour arrière : `LANDLINE_ANCS_FIRST_SECONDS=0` (coupe l'attente et le pas rapide) ; 1 à 120 règle l'attente.

Temps attendu sur l'essai 1 (**déduit**) : notifications vers +3 à +5 s au lieu de +9,7 s.

### 7.3 Annonce LE : politique et renouvellement

**Ce que recommande Apple** (QA1931 et Accessory Design Guidelines, relevé dans
`iphone-link-study/02-protocoles-reconnexion.md`, §3) : 20 ms pendant **au moins** 30 s, puis exactement
l'une de ces valeurs : 152,5 / 211,25 / 318,75 / 417,5 / 546,25 / 760 / 852,5 / 1022,5 / 1285 ms. Trente
secondes est un plancher, pas une consigne.

**Politique retenue.**

| Phase | Intervalle | Durée |
|---|---|---|
| Après la perte du lien LE | 20 ms | 180 s (`ADVERT_FAST_SECONDS`, 30 s avant) |
| Ensuite | 152,5 ms | tant que l'iPhone est absent, sans limite |
| BR/EDR atteint l'iPhone alors que le LE manque | 20 ms | 30 s (`ADVERT_SEEN_FAST_SECONDS`) |

Trois minutes couvrent le Bluetooth coupé puis rallumé (55 s dans l'essai 2) et un aller-retour dans une
autre pièce. Au-delà, personne ne sait quand l'iPhone revient : c'est le palier le plus court de la liste
d'Apple qui tient, en continu. Le troisième cas est le seul indice de présence dont le moteur dispose sans
le LE. `LANDLINE_ADVERT_FAST_SECONDS` règle la première durée (0 à 3600 ; 30 rend le comportement précédent).

**Ce que coûte 152,5 ms en continu** (**estimé, non mesuré**). L'annonce occupe les 31 octets utiles : avec
l'en-tête, 47 octets, soit 376 µs par canal à 1 Mbit/s ; trois canaux, plus l'écoute qui suit chaque
émission d'une annonce connectable : de l'ordre de 2 ms de radio active par annonce.

| Intervalle | Radio active | Puissance moyenne (radio à 100–200 mW en émission) | Part d'un portable au repos (≈ 5 W) |
|---|---|---|---|
| 152,5 ms | ≈ 1,3 % | 1,5 à 3 mW | ≈ 0,05 % — une vingtaine de secondes sur 10 h d'autonomie |
| 20 ms | ≈ 10 % | 10 à 20 mW | ≈ 0,3 % |
| 1285 ms | ≈ 0,15 % | < 0,5 mW | négligeable |

À 152,5 ms la consommation n'est pas le sujet. Deux réserves : le contrôleur Bluetooth ne peut pas être mis
en veille USB tant qu'une annonce est en ondes (c'est déjà le cas dès qu'un lien est monté) ; et à 20 ms, sur
une carte Wi-Fi/Bluetooth à antenne partagée, 10 % du temps d'antenne en 2,4 GHz peut se voir sur le débit
Wi-Fi — raison de ne pas laisser 20 ms en continu. À mesurer avec `powertop` si le doute persiste. En
veille de l'ordinateur, rien n'est annoncé.

**Renouvellement à l'expiration des 180 s.** BlueZ retire l'annonce au bout de 180 s
(`ANCS_ADVERT_TIMEOUT_SECONDS`) et appelle `Release`. Avant : le moteur ne la remettait qu'au tour suivant
qui passait par là — jusqu'à 1 s, plus le temps que le fil passait dans une tentative OBEX (jusqu'à 3,9 s
relevés pendant l'absence de l'essai 2 ; l'annonce initiale est partie avec 3,8 s de retard pour la même
raison). Maintenant : l'annonce est réenregistrée 20 s **avant** l'expiration (`advert_renewal_due()`), ce
qui réduit le trou au temps de deux appels D-Bus (retrait, enregistrement). Il n'est pas nul : faire
chevaucher deux annonces demanderait deux instances, et BlueZ en annonce parfois zéro de libre sur cette
carte quand la nôtre est en ondes. Avec 7.4, le fil n'est plus retenu par OBEX pendant l'absence. Si BlueZ
refuse le réenregistrement, nouvelle tentative 30 s plus tard, comme avant.

Non vérifié sans matériel : la durée réelle du trou (`btmon`, `LE Set Extended Advertising Enable`) ; qu'un
réenregistrement toutes les 160 s pendant des heures ne laisse pas d'enregistrement fantôme.

### 7.4 Pendant l'absence : tentatives MAP/PBAP espacées

Sans lien BR/EDR ni LE, une ouverture de session ne peut pas aboutir, et chaque paire de tentatives retient
le fil 2,5 à 3,9 s (essai 2 : toutes les 14 s). `ProfileSupervisor::tick()` reçoit maintenant
`phone_reachable` :

- aucun lien : première tentative 15 s après la perte (plus aucune dans la seconde qui suit), puis 30 s,
  60 s, 120 s, et toutes les 2 min ensuite (`PROFILE_ABSENT_POLL_MAX_SECONDS`) ;
- un lien revient (BR/EDR ou LE) : ouverture **dans le tour**, sans attendre l'échéance — dans l'essai 2,
  MAP et PBAP auraient été ouverts vers +1 à +2 s au lieu de +6,5 s ;
- les tentatives ne s'arrêtent jamais tout à fait : sur une machine où BlueZ ne rapporte jamais le lien
  (pas de profil audio local, cas décrit par Tether le 23 août 2026), c'est la seule façon de retrouver
  l'iPhone. Pour la même raison, la toute première tentative du démon n'est pas retardée, une session
  ouverte vaut un lien, et une machine sans moitié LE garde le rythme d'avant.

Retour arrière : `LANDLINE_OBEX_ABSENT_BACKOFF=0`.

### 7.5 État publié : chaque changement dès qu'il est connu

- Les liens sont relus **juste après** le traitement des sessions OBEX, avant la publication qui suit :
  un lien monté par obexd est publié dans le tour, pas après le carnet (les 5 s de retard de l'essai 2).
- Après chaque étape qui a retenu le fil plus de 250 ms (carnet, appels récents, messages), les étapes
  rapides repassent : liens, notifications, appels, publication (`run_quick_steps()`). L'horloge du tour
  est relue à chaque passage.
- Un tour où rien n'a retenu le fil fait chaque chose une fois, comme avant.

Reste vrai : une lecture en cours (carnet : ≈ 5 s pour 767 fiches) ne s'interrompt pas, et rien n'est publié
pendant ce temps. 7.1 et 7.2 la rendent rare et la sortent de la fenêtre de reconnexion ; la sortir du fil
reste la correction 5.7.

### Ce qui reste hypothétique sans iPhone

1. Que le mains-libres vienne sans cycle dans le cas de l'essai 2 (7.1, point 2).
2. Que `ConnectProfile(111f)` serve à quelque chose : Tether a mesuré que non ; le journal le dira.
3. Que `GetSize` soit servi par l'iPhone et rende le même nombre d'une session à l'autre. S'il échoue, le
   carnet est relu 5 min après chaque nouvelle session — jamais pendant la reconnexion.
4. Que le pas d'une seconde suffise à ramener ANCS vers +3 à +5 s dans l'essai 1.
5. La durée du trou d'annonce au renouvellement, et l'effet réel de 180 s à 20 ms sur un iPhone en veille.
6. Qu'aucune machine ne dépende des tentatives OBEX à 15 s pendant l'absence (7.4 garde un filet à 2 min).

## 8. Incident du 16 novembre : appairage partiel et boucle d'authentification (branche `moteur-appairage-sain`)

Compilé et testé (755 tests), **pas essayé avec l'iPhone**. Le contrat pour l'extension est dans
`contrat-appairage.md` ; les champs sont décrits dans `moteur-api.md`, sections 3 et 6.

### 8.1 Ce qui a été observé (journal du démon)

1. Au démarrage : « A device is bonded, but the bond covers BR/EDR only -- no LE keys were derived… ».
   Un appairage existait déjà dans BlueZ, créé hors du moteur — vraisemblablement une demande venue de
   l'iPhone et acceptée par un autre agent Bluetooth. Il ne portait que les clés BR/EDR.
2. L'extension envoie `bt_pair`. Le moteur répond `already_paired`, `settling`, `soliciting`, puis
   « supervising 30:D5:… (ANCS enabled) » : succès, sans code.
3. Ensuite, 25 minutes de boucle : 186 fois « org.bluez.Bearer.LE1 disconnected …
   org.bluez.Reason.Authentication », « ancs: notification session 170 started », « map session failed
   (other): The connection is closed » toutes les 5 s, des coupures volontaires de BR/EDR, « bond was
   pinned to BR/EDR; preference handed back to LE ». L'utilisateur voyait « connecté » ; rien ne marchait.

### 8.2 Pourquoi (lu dans le code)

- `pair_device()` ne regardait que `Device1.Paired` : vrai, donc `already_paired` et `success:true`
  (`pairing.cpp`). `dual_bond:false` était bien dans le résultat, mais rien n'en faisait un échec.
- Le succès enregistrait l'adresse et lançait la supervision avec ANCS : l'annonce LE partait.
- **Déduit** : l'annonce appelle l'iPhone sur un lien LE ; faute de clés LE, le chiffrement échoue et BlueZ
  coupe avec `Reason.Authentication`. L'annonce étant toujours en ondes, l'iPhone revient : une coupure
  toutes les 8 s environ (186 en 25 min).
- Chaque coupure LE faisait repartir le reste : session ANCS reconstruite, tentatives MAP, cycle
  mains-libres, préférence de lien réécrite. Aucun compteur ne distinguait « l'iPhone refuse de
  s'authentifier » d'une coupure ordinaire, et l'état publié ne disait rien de l'appairage.

### 8.3 Corrections

| # | Correction | Où |
|---|---|---|
| 1 | Santé de l'appairage lue dans BlueZ (`Bearer.LE1.Bonded`, `Bearer.BREDR1.Bonded`) : `assess_bond()` | `bluetooth/bond_health.hpp`, `objects.cpp` (`bond_facts()`, `report_bond()`) |
| 2 | `bt_pair` sur un appairage partiel ou cassé : étape et statut `stale_bond`, échec, rien n'est touché | `plan_existing_bond()`, `pair_device()` |
| 3 | Option `fresh` de `bt_pair` : `RemoveDevice`, redécouverte, appairage neuf avec code | `pair_device()`, `run_bt_pair()` |
| 4 | Compteur des coupures `Reason.Authentication` par appareil | `monitor.cpp` (`auth_failure_count()`) |
| 5 | Détection de la boucle : 5 coupures en 120 s, ou 2 refus « key-missing » → `broken` | `AuthFailureTracker`, `BearerSupervisor::assess()` |
| 6 | État `broken` : plus de connexion, de composition LE, de cycle mains-libres, d'annonce, d'ouverture MAP/PBAP ; fenêtre d'essai de 30 s toutes les 10 min | `BearerSupervisor::supervise()`, `should_solicit_ancs()`, `should_open_profiles()` |
| 7 | `bond_state`, `bond_detail`, `needs_repair` dans `bt_connection_changed`, `bt_status` (avec `bond_address`), `bt_pair_result`, `bt_devices` | `connection.cpp`, `net.cpp` |
| 8 | `bt_unpair` efface aussi la sélection, arrête la supervision, retire l'annonce | `finish_unpair()`, `run_bt_unpair()` |

Rejoué sur l'incident : au démarrage, `bt_status` porte `bond_state:"partial"`, `bond_detail:"bredr_only"`,
`needs_repair:true`, `bond_address` = l'iPhone, avant tout `bt_pair`. Le `bt_pair` reçoit `stale_bond`.
L'extension propose alors l'appairage neuf (`fresh:true`). Et si un appairage se dégrade en marche, la
boucle s'arrête à la cinquième coupure — une quarantaine de secondes au rythme observé — avec une seule
ligne au journal, au lieu de 186.

### 8.4 Choix à connaître

- **Seul `needs_repair` condamne.** Sur une machine qui ne peut pas porter le LE (`capability.mode` autre
  que `full`), un appairage BR/EDR seul est le mieux qu'on aura : `partial`, `needs_repair:false`,
  `bt_pair` répond `already_paired`. Sans cela, `fresh` tournerait en rond.
- **`le_only` est signalé, pas condamné** : `Bearer.BREDR1` n'est qu'un marqueur vide sur certaines
  versions de BlueZ.
- **Un abonnement ANCS actif vaut preuve** de la moitié LE, quoi que dise `Bearer.LE1.Bonded`.
- **Les refus de connexion `AuthenticationFailed` ne sont pas comptés**, seulement les coupures
  `Reason.Authentication` et les refus « key-missing ». Le code amont dit d'un iPhone qui refuse à
  répétition la connexion BR/EDR que « re-pairing is not the fix » (Bluetooth de l'iPhone coincé) : les
  compter ferait redemander un appairage à tort.
- **`broken` ne retombe pas tout seul sur un simple retour de BR/EDR** : dans l'incident BR/EDR montait
  pendant que LE échouait. Il faut un lien LE qui tient 60 s, ou un appairage refait.
- **Les compteurs repartent de zéro** au démarrage de chaque supervision et quand l'appareil disparaît de
  BlueZ : ce qui a été compté avant appartient à un autre appairage.

### 8.5 Ce qui reste hypothétique sans iPhone

1. Que la cause de la boucle soit bien l'annonce LE sur un appairage sans clés LE (8.2, déduit). Si la
   boucle continue annonce coupée, c'est l'iPhone qui rappelle seul : le moteur n'y peut rien, mais l'état
   publié reste `broken` et les tentatives du moteur restent arrêtées.
2. Que `Bearer.LE1` soit renseigné avec `Bonded:false` sur un tel appairage (le message de démarrage le
   suggère : il repose sur la même lecture).
3. Qu'une même coupure ne soit pas signalée deux fois sur `Bearer.LE1` : le compteur prend le plus grand
   des deux comptes (interface de lien, `Device1`), pas leur somme, mais un doublon sur la même interface
   diviserait le seuil par deux.
4. Le déroulé de `fresh` : délai entre `RemoveDevice` et la disparition de l'objet (5 s d'attente au
   plus), redécouverte en 30 s, et le comportement de l'iPhone s'il n'a pas oublié l'ordinateur.
5. Le texte exact de l'erreur « key-missing » renvoyée par BlueZ (`br-connection-key-missing` attendu).
6. Les seuils (5 en 120 s, 60 s de tenue, 10 min entre deux fenêtres) : choisis d'après un seul incident.

## 9. Bus de session mort, interrupteurs vus trop tard (branche `moteur-reactif`)

Compilé et testé (781 tests), **pas essayé avec l'iPhone**. Les champs et la commande sont décrits dans
`moteur-api.md`, sections 3 et 11.

### 9.1 Bus de session mort

**Observé (journal du démon, 16 novembre).** À une reconnexion de session GNOME, le moteur a démarré 2 s
avant le nouveau bus de session et est resté branché sur l'ancien. Toutes les 5 s, sans fin :
« map session failed (other): The connection is closed », « pbap session failed (other): The connection is
closed ». obexd n'était jamais activé, l'ordinateur n'annonçait ni MAP ni PBAP, et l'iPhone ne proposait
pas les deux interrupteurs. L'unité systemd est depuis liée à `dbus.service` ; le moteur se défend
maintenant seul.

**Pourquoi (lu).**

- `g_bus_get_sync()` rend la même connexion à tout le processus, une par bus. Fermée, elle reste fermée et
  reste partagée tant que quelqu'un la tient (ici : obexd, les bulles, MPRIS, la téléphonie PipeWire). Un
  nouvel appel rend la même connexion morte : **rien ne se répare dans le processus**.
- GDBus sait faire sortir un programme dont le bus se ferme (`exit-on-close`, par `SIGTERM`), mais il le fait
  depuis la boucle GLib du fil qui a demandé le bus en premier. Quand c'est le fil de supervision, qui n'a
  pas de boucle GLib, le signal n'arrive jamais : c'est le cas observé (déduit). Quand c'est un fil qui en a une, le
  démon reçoit `SIGTERM` et sort **avec le code 0**, que `Restart=on-failure` ne relance pas.

**Corrections.**

- `BusWatch` (`include/tether/bus_watch.hpp`, `src/bus_watch.cpp`) : un fil qui regarde chaque seconde si la
  connexion au bus de session et celle au bus système (BlueZ) sont fermées
  (`g_dbus_connection_is_closed()`), sans dépendre d'aucune boucle. Il coupe `exit-on-close` sur ces deux
  connexions pour que la sortie soit toujours la même.
- À la fermeture, `src/daemon/main.cpp` écrit une ligne `[ERR]` — « The session bus connection of this
  process is closed; … Leaving with status 75 so the service manager starts a fresh engine. » — attend 2 s
  (`BUS_CLOSED_EXIT_GRACE_MS`, le temps de publier la cause), arrête le démon proprement et **sort avec le
  code 75** (`BUS_CLOSED_EXIT_STATUS`). Si l'arrêt propre n'aboutit pas en 8 s, sortie forcée, même code.
  systemd relance (`Restart=on-failure`) ; le nouveau processus prend le bus qui existe à ce moment-là.
- Tant que le processus vit encore, MAP et PBAP publient le motif **`bus_closed`** au lieu de `other`
  (`ObexError::BusClosed`) : l'erreur « The connection is closed » est reconnue par
  `classify_obex_error()`, et le superviseur ferme tout de suite les deux sessions dans l'état publié
  (`ProfileSupervisor::bus_lost()`), sans attendre le prochain essai ni rien retirer par un bus qui ne
  répond plus. `profile_reason` dit que le moteur a perdu le bus et redémarre seul.
- Bus de session **absent** au démarrage (pas fermé : introuvable) : même motif `bus_closed`, mais le moteur
  **ne sort pas**. Il redemande le bus à chaque essai et le prend dès qu'il existe ; sortir ferait boucler
  systemd jusqu'à sa limite de relances (5 en 60 s) pendant que le bus n'est pas là.

Retour arrière : `LANDLINE_BUS_WATCH=0` (le démon reste en vie sur un bus fermé, comme avant ; le motif
`bus_closed` reste publié).

### 9.2 Un interrupteur activé sur l'iPhone, vu en 2 s

**Observé.** Entre « Afficher les notifications » ou « Synchroniser les contacts » activé dans Réglages ›
Bluetooth › ⓘ et sa prise en compte : jusqu'à 20 s. Le moteur ne l'apprend qu'en rouvrant une session, et
l'essai se faisait toutes les 15 s dès qu'un des deux profils était ouvert (5 s avant), **plus** le temps
où le fil était pris par la lecture du carnet (767 fiches, ≈ 11 s).

**Corrections.**

1. **Essais rapprochés d'un profil refusé** (`ProfileSupervisor::tick()`). Un profil dont le dernier essai
   a rendu `forbidden` est réessayé **seul, toutes les 2 s** (`PROFILE_FORBIDDEN_POLL_SECONDS`) pendant
   **5 min** (`PROFILE_FORBIDDEN_FAST_SECONDS`). La fenêtre part du premier refus vu depuis le démarrage de
   la supervision (donc d'un appairage neuf, qui la relance) ; elle repart quand l'autre profil s'ouvre
   (l'utilisateur est sur l'écran des interrupteurs) et sur `bt_retry_profiles`. Elle **ne repart pas** à
   une perte de lien : un iPhone qui va et vient toute la journée avec un interrupteur coupé n'est pas
   sollicité 5 min à chaque retour. Passé la fenêtre : l'essai commun toutes les 15 s, comme avant.
2. **Seulement lien BR/EDR monté.** `tick()` reçoit `classic_up` (`classic_connected` du superviseur de
   liens). Sans lien, obexd ferait appeler l'iPhone (paging) à chaque essai : les essais rapprochés ne
   partent pas, ni pendant la coupure volontaire du cycle mains-libres. Les autres motifs (`busy`,
   `no_record`, `unavailable`, `other`) gardent le rythme commun : aucun n'est un interrupteur qu'on vient
   d'activer.
3. **Chaque profil a sa propre échéance.** L'essai d'un profil refusé n'attend plus l'échéance commune ni
   l'autre profil. Le second essai « session périmée » (retirer puis rouvrir) n'a lieu qu'au premier refus,
   comme avant : un essai rapproché, c'est un seul `CreateSession`.
4. **Pendant une lecture longue** (`TransferIdleScope`, `include/tether/bluetooth/obex_transfer.hpp`).
   L'attente d'un transfert OBEX (carnet, appels récents, corps des messages) interroge obexd toutes les
   50 ms ; le fil de supervision y fait désormais, une fois par seconde, les essais de profil dus et le
   travail du client de notifications (`Tick::during_long_read()` dans `connection.cpp`). L'essai de l'autre
   profil ne passe plus derrière le carnet. Ce qui n'est **pas** fait à cet endroit : relire les liens,
   relâcher un profil (verrouillage, inactivité) — cela pourrait retirer la session en cours de lecture.
   C'est une réponse partielle à la correction 5.7 : la lecture reste sur le fil, mais ne le monopolise plus.
5. **Publié dès l'ouverture.** `ProfileSupervisor::set_opened_handler()` : dès qu'une session s'ouvre,
   `bt_connection_changed` part, avant l'essai de l'autre profil (qui peut attendre obexd) et avant toute
   lecture.
6. **`bt_retry_profiles`** (`ConnectionManager::retry_profiles()`) : réveille le fil de supervision, qui
   essaie tout de suite chaque profil fermé (même iPhone absent : un essai, pas une série), relance la
   fenêtre de 5 min et la découverte ANCS. Pour l'extension : à l'ouverture du menu ou de l'écran d'aide aux
   interrupteurs.
7. **Journal.** Un même refus répété n'est écrit qu'une fois toutes les 15 s (les autres en `TRACE`) : les
   essais à 2 s ne remplissent pas le journal.

**Coût pour l'iPhone (déduit, à confirmer par `btmon`).** Sur un lien déjà monté, un essai refusé, c'est :
une requête SDP, l'ouverture du canal du profil, un `CONNECT` OBEX, la réponse `Forbidden` (0x43) et la
fermeture — quelques trames, aucune recherche radio, rien à l'écran de l'iPhone. 150 essais au plus par
profil et par fenêtre.

Retours arrière : `LANDLINE_FORBIDDEN_FAST_SECONDS=0` (plus d'essais rapprochés ; de 1 à 3600 : durée de la
fenêtre) ; `LANDLINE_LONG_READ_ATTEMPTS=0` (plus rien pendant une lecture longue).

### 9.3 ANCS : « Partager les notifications système »

**Lu.** Deux essais périodiques, pas d'événement :

- interrupteur coupé, l'iPhone n'expose pas le service de notifications. Le client le cherche
  (`AncsClientState::discover()`, une lecture de ce que BlueZ sait déjà, sans trafic radio) toutes les 1 s
  pendant les 20 s qui suivent la montée du lien, puis **toutes les 5 s** ; abonnement au tour suivant ;
- service exposé mais accès non accordé : une sonde (écriture GATT) toutes les 5 s.

Délai avant de voir l'interrupteur : jusqu'à 5 s + 1 s, plus le temps d'une lecture longue en cours (le
client ne tournait pas pendant le carnet).

**Corrections.**

- Découverte et abonnement toutes les **2 s** pendant **5 min** après la montée du lien
  (`ANCS_WATCH_RETRY_SECONDS`, `ANCS_WATCH_SECONDS`, dans `ancs_retry_seconds()`), 5 s ensuite.
- **Sur événement** : `sync_ancs()` voit dans l'instantané BlueZ que l'iPhone se met à offrir le service
  (`supports_ancs()`) et relance la découverte dans le tour (`AncsClient::retry_now()`), quelle que soit
  l'échéance. Délai attendu : 1 s.
- Le client tourne pendant les lectures longues (9.2, point 4) et `bt_retry_profiles` le relance aussi.
- La **sonde reste à 5 s** : c'est une écriture vers l'iPhone, et une sonde sans réponse occupe la file du
  séquenceur 15 s.

Retour arrière : `LANDLINE_ANCS_FIRST_SECONDS=0` coupe déjà les pas rapprochés d'ANCS (1 s et 2 s).

### 9.4 Ce qui reste hypothétique sans iPhone

1. Que la fermeture du bus soit bien vue comme « fermée » dans le cas observé (ancien bus qui meurt). Le
   test le reproduit avec une connexion D-Bus sur une paire de sockets dont on ferme l'autre bout, pas avec
   un vrai bus de session. À vérifier : la ligne `[ERR]`, le code 75 (`systemctl --user status`), la relance.
2. Que systemd relance bien sur le code 75 avec l'unité actuelle, et que `PartOf=dbus.service` ne fasse pas
   double emploi au point de compter deux relances dans la limite de 5 en 60 s.
3. Que l'iPhone rende `Forbidden` aussi vite au 150ᵉ essai qu'au premier, sans ralentir ni fermer le lien,
   et sans effet sur la veille du lien (sniff) ou la batterie. Si un essai devait durer plus d'une seconde,
   le fil serait retenu d'autant toutes les 2 s : à lire dans le journal (`LANDLINE_FORBIDDEN_FAST_SECONDS=0`
   en cas de doute).
4. Qu'un interrupteur coupé se traduise toujours par `forbidden`. S'il arrive que ce soit `no_record`
   (obexd rend ce motif quand sa requête SDP est refusée), ce profil reste au rythme de 15 s.
5. Qu'ouvrir une session MAP pendant un transfert PBAP en cours (et l'inverse) ne gêne ni obexd ni
   l'iPhone. Les deux profils ont chacun leur canal ; c'est ce que fait un autoradio.
6. Que l'iPhone expose bien le service ANCS au moment où l'interrupteur est activé (et que BlueZ le voie
   sans reconnexion), ce qui est le cas couvert par la découverte à 2 s et par l'événement. Si le service
   était déjà exposé et seulement muet, c'est la sonde à 5 s qui décide.
7. Les durées (2 s, 5 min) : choisies, pas mesurées.

## 10. Plantage de bluetoothd, LE qui ne revient pas, redémarrage de bluetoothd (branche `moteur-robuste`)

Compilé et testé (889 tests), **pas essayé avec l'iPhone**. Sources lues : notre code, le source de BlueZ
5.87 (`src/shared/gatt-client.c`, `src/gatt-client.c`, `src/shared/att.c`, `src/device.c`,
`profiles/battery/battery.c`) et sa branche principale, le journal de `bluetooth.service`, le journal du
moteur à partir de 18:52, `engine/docs/BLUETOOTH.md`. Les variables d'environnement sont dans
`moteur-api.md`, section 11 ; les essais à refaire dans `essai-reconnexion.md`, scénarios I à L.

### 10.1 Le plantage de 18:47

**Observé (journal système).**

```
18:47:02  rap_accept() RAP unable to attach                      un lien GATT vient de monter
18:47:03  reconnect_timeout() Reconnecting services failed: Operation already in progress
18:47:03  (client abonné) ancs_ready=non
18:47:15  (client abonné) le_connected=non
18:47:33  batt_io_ccc_written_cb() Battery Level: notifications not enabled
          Request attribute has encountered an unlikely error
18:47:33  src/shared/gatt-client.c:2178: disable_ccc_callback:
          Assertion `notify_data->chrc->ccc_write_id' failed.
18:47:35  bluetooth.service: Main process exited, code=dumped, status=6/ABRT
```

Le journal du moteur pour cette minute n'existe plus : le dossier `~/.local/state/landline` a été supprimé
pendant que le moteur tournait (10.5). Le rapport de plantage (`/var/crash`) n'est lisible que par root.

**Ce que dit le source de BlueZ (lu, 5.87 ; identique sur la branche principale en novembre 2026).**

Pour chaque caractéristique, BlueZ tient un compte d'abonnés (`notify_count`) et **un seul** identifiant
d'écriture en cours sur le descripteur CCC (`ccc_write_id`). Les deux rappels de fin d'écriture
(`enable_ccc_callback`, `disable_ccc_callback`) commencent par `assert(ccc_write_id)` puis le remettent à
zéro. Rien n'empêche deux écritures « couper » d'être en cours ensemble :

| Appel D-Bus du client | Dans `gatt-client.c` | `ccc_write_id` |
|---|---|---|
| (abonnement en service) | compte = 1 | 0 |
| `StopNotify` | compte → 0 : écriture CCC=0, **D1** | D1 |
| `StartNotify` | compte = 1 ; une écriture est en cours : mis en file (`reg_notify_queue`), pas de réponse D-Bus | D1 |
| `StopNotify` | retiré de la file ; compte → 0 : **seconde** écriture CCC=0, **D2** | D2 (écrase D1) |
| réponse ou échec de D1 | `disable_ccc_callback` : l'assertion passe, remise à zéro | 0 |
| réponse ou échec de D2 | `disable_ccc_callback` : **`assert(ccc_write_id)` échoue → abort()** | — |

Sur un lien sain, l'iPhone répond à D1 en un intervalle de connexion et la fenêtre dure quelques
millisecondes. Sur un lien qui ne répond plus, elle dure tout le délai ATT : **30 s**
(`ATT_TIMEOUT_INTERVAL`), au bout desquelles `timeout_cb` puis `disconnect_cb` font échouer d'affilée
toutes les requêtes en attente, chacune avec le code « unlikely error » — celui que le greffon batterie a
journalisé une ligne avant l'assertion. Un client qui quitte le bus pendant qu'il est en file produit la
même chose que le troisième appel (`notify_client_disconnect`).

Deux détails de BlueZ rendent la séquence facile à produire sans le vouloir :

- un second `StartNotify` du même client, pendant que le premier attend, est **accepté tout de suite**
  (`client->notify_id` est déjà renseigné) : le client se croit abonné ;
- `Notifying` n'est pas remis à faux quand le lien tombe ; il l'est à chaque `StopNotify`.

**Ce que faisait notre moteur (lu, `ancs/client.cpp` d'avant).** `unsubscribe()` envoyait `StopNotify` sur
les deux sources ANCS à **chaque** reconstruction : lien LE signalé tombé, `Notifying` lu à faux, chemin de
l'appareil vidé. `subscribe()` envoyait `StartNotify` en synchrone, 10 s, et tenait un succès pour un
abonnement. Sur un lien muet :

| Heure (déduite) | Moteur | BlueZ |
|---|---|---|
| 18:47:03 | reconstruction : `StopNotify` ×2 (`ancs_ready=non`) | D1 sur chaque source, sans réponse |
| 18:47:03–13 | `StartNotify` (Data Source) : bloque 10 s, expire | abonné mis en file |
| 18:47:15 | état publié (`le_connected=non`) ; le fil était retenu depuis 10 s | |
| ≈ 18:47:14–24 | nouvel essai : Data Source accepté d'emblée ; Notification Source bloque 10 s | second abonné en file |
| ≈ 18:47:25 | nouvel essai : les deux acceptés d'emblée → « abonné » ; la vérification lit `Notifying` à faux → reconstruction : `StopNotify` ×2 | D2 sur chaque source |
| 18:47:33 | | délai ATT de la requête de 18:47:03 : tout échoue, D1 puis D2 → assertion |

**Degré de preuve.** *Prouvé par le source* : la séquence `StopNotify` / `StartNotify` / `StopNotify` avant
la réponse à la première écriture fait échouer exactement cette assertion, à cette ligne ; notre moteur
émettait cette séquence à chaque reconstruction sur un lien muet ; le test
`AncsCrashSequence.TheOldRebuildAbortsBluetoothd` la rejoue contre un modèle de la comptabilité de BlueZ.
*Très probable, non prouvé* : que ce soit ce qui s'est passé à 18:47. Les heures concordent à la seconde
(30 s entre la première reconstruction et l'assertion, 10 s de fil retenu avant `le_connected=non`), le
moteur était le seul abonné D-Bus de ces caractéristiques, mais son journal manque. *Non établi* : pourquoi
le lien ne répondait plus à 18:47:03 (lien à peine remonté, BR/EDR en cours de reconnexion par BlueZ).

**Le service Batterie de BlueZ.** Il n'est pas en cause : sa caractéristique (Battery Level) a son propre
compte et son propre `ccc_write_id`, et il ne fait qu'un abonnement par connexion. Sa ligne d'erreur est le
premier effet visible du délai ATT, pas une cause. Seul, il ne peut pas produire deux écritures « couper ».

**Rapport amont.** Aucun rapport trouvé pour cette assertion (recherche web, liste linux-bluetooth). Des
correctifs voisins existent dans le même code (2016 : assertions CCC retirées parce qu'« une notification
peut être coupée avant la réponse » ; 2023 : `ccc_write_id`). BlueFerry a noté sur BlueZ 5.87 un plantage
frère (`SIGSEGV` dans `register_notify_cb`) pour « `StopNotify` suivi de `StartNotify` pendant une coupure
LE ». Le défaut est à signaler à BlueZ ; il se corrige en n'écrivant pas un second CCC=0 quand une écriture
est déjà en cours.

**La contradiction Tether / BlueFerry, tranchée.** Tether a ajouté `StopNotify` avant chaque reconstruction
le 15 septembre 2026, en notant « not verified on hardware », parce qu'un nouvel abonnement lui semblait
sans effet tant que BlueZ gardait l'ancien abonné. Le source dit autre chose : au retour du lien,
`btd_gatt_client_connected()` réinscrit de lui-même chaque abonné gardé, avec une vraie écriture du CCC.
`StopNotify` n'est donc pas nécessaire, et il est dangereux.

### 10.2 Correction : les règles de `subscription_guard.hpp`

| Règle | Où |
|---|---|
| Une reconstruction n'envoie **jamais** `StopNotify` ; elle n'oublie la session que de notre côté | `AncsClientState::release(Release::Rebuild)` |
| `StopNotify` ne reste que pour les notifications coupées (client détruit), et seulement si le lien répond **et** si la caractéristique lit `Notifying` (donc aucune écriture en attente pour nous) | `stop_notify_verdict()` |
| Jamais deux `StopNotify` à moins de 35 s ; **aucun `StartNotify` dans les 35 s** qui suivent un `StopNotify` (le délai ATT est de 30 s) : rien n'est jamais mis en file derrière D1. L'heure est commune à tout le processus | `start_notify_allowed()`, `g_last_stop_at` |
| Abonné = `StartNotify` accepté **et** `Notifying` lu à vrai. Accepté sans `Notifying`, sans réponse, ou `InProgress` : « en cours », on attend, on ne coupe rien | `classify_start()` |
| `StartNotify` n'attend plus que 4 s (10 s avant) : le fil n'est plus retenu par un abonné en file | `START_NOTIFY_TIMEOUT_MS` |
| Abonné mais iPhone muet : `StartNotify` repasse toutes les 10 s (immédiat si BlueZ a gardé l'abonné, vraie inscription s'il l'a perdu) | `RESUBSCRIBE_SECONDS` |

Retour arrière : `LANDLINE_ANCS_STOP_NOTIFY=0` supprime le dernier `StopNotify` qui reste. Il n'y a pas
d'interrupteur pour retrouver l'ancien comportement : il fait planter bluetoothd.

**Ce qui reste possible, hors de notre portée.** Deux relances du moteur à moins de 30 s d'intervalle sur
un lien muet : la sortie du premier processus fait écrire D1 par BlueZ, le second se met en file, sa sortie
fait écrire D2. Aucun client ne peut l'éviter ; seul un correctif de BlueZ le peut.

### 10.3 Le LE qui ne revient pas

**Observé le 16 novembre.**

| Cas | Situation | LE |
|---|---|---|
| (a) 18:03 | relance du moteur, BR/EDR resté monté | absent des minutes, annonce en ondes |
| (b) 18:47 | plantage puis relance de bluetoothd | absent > 5 min ; plus de carte audio dans PipeWire |
| (c) 18:52 | relance du moteur, BR/EDR monté | absent jusqu'à 19:17 |
| 19:17–19:18 | Bluetooth de l'iPhone coupé puis rallumé : **tout** tombe | LE à 19:18:03,2 ; MAP +2,7 s ; PBAP et BR/EDR +4,5 s ; appels +5,6 s ; notifications +7,9 s ; carte audio revenue |
| 14:49 (essai 1), 13:15 | coupure complète depuis l'ordinateur | LE en moins d'une seconde |

**Lu dans le journal du moteur, cas (c).** `18:52:48,2 soliciting ANCS … 20-20 ms`, puis plus rien avant
`18:53:37,6 soliciting ANCS … 20-20 ms` : l'annonce a été **retirée 3 s après le démarrage et remise 45 s
plus tard**. C'est la connexion LE sortante (`connect_le()`, une par vie du démon, 3 s après BR/EDR) : tant
qu'elle est en cours, `should_solicit_ancs()` retire l'annonce, et elle n'aboutit jamais (45 s =
`LE_CONNECT_TIMEOUT_MS`). Tether a mesuré que cette connexion sortante n'a jamais ouvert un lien portant
ANCS, alors que l'annonce en ouvre un en 1,3 s (22 août 2026). Ensuite l'annonce est restée en ondes 24 min
sans que l'iPhone vienne, BR/EDR monté.

**Ce que disent les sources.**

- Le lien LE est ouvert **par l'iPhone**, en réponse à l'annonce (Tether, 22 août : `Role: Peripheral`).
- `PreferredBearer=bredr` fait refuser ce lien entrant : mesure A/B de Tether du 25 août (bredr : absent
  180 s ; le : monté en 12 s). Notre `connect_classic()` écrit `bredr` avant `Device1.Connect` ; la remise
  à `le` n'avait lieu qu'une fois BR/EDR monté depuis 3 s. Tant que la connexion échoue (iPhone absent,
  refus), l'appairage restait donc sur `bredr`, y compris pendant les attentes entre deux tentatives.
  Dans BlueZ (lu, `device.c`) la valeur est enregistrée sur disque et survit aux redémarrages.
- Tether a vu le LE revenir avec BR/EDR monté (7 septembre, 5 s, contrôleur MediaTek). Sur notre Intel
  9560, les cinq relevés du jour disent l'inverse. L'hypothèse « l'iPhone ne répond à l'annonce que s'il
  n'a aucun lien avec l'accessoire » est **compatible avec tous nos relevés et non prouvée** ; le mécanisme
  (iPhone, contrôleur, ou BlueZ) n'est pas établi.
- Le son : la carte PipeWire n'existe que si un profil audio (A2DP ou mains-libres) est connecté. Après le
  plantage, BR/EDR a été remonté par obexd seul, donc sans profil audio ; l'iPhone n'en ouvre pas de
  lui-même sur un tel lien (déjà déduit en 1 ter). À 19:18 le lien est remonté par une vraie demande
  (`Bearer.BREDR1.Connect`, accepté en 1 s) et la carte est revenue. Rien de plus n'est nécessaire côté
  son : quand le réglage est « ici », `PhoneAudioFollower` demande déjà `ConnectProfile` (A2DP source) 5 s
  après chaque nouveau lien.

**Stratégie retenue : « LE d'abord » (`le_first.hpp`).** Elle ne dépend pas du mécanisme : elle remet
l'iPhone dans la seule situation d'où on l'a vu rouvrir le LE sur cette machine, à chaque fois.

| # | Mesure | Détail | Réglage |
|---|---|---|---|
| 1 | **Retenue** | Aucun lien (démarrage, retour de bluetoothd, perte complète) : ni connexion BR/EDR ni session MAP/PBAP pendant 10 s au plus ; l'annonce est seule en ondes. Levée dès que le LE est là | `LANDLINE_LE_FIRST_HOLD_SECONDS` (0 = non) |
| 2 | **Recyclage** | BR/EDR monté **et** annonce en ondes depuis 12 s sans LE : `Device1.Disconnect`, une fois ; la retenue prend la suite. Jamais pendant un appel ni quand le son de l'iPhone joue ici, ni connexion en cours ; pas deux fois en 10 min ; pas juste après une retenue restée sans réponse ; réarmé quand le LE est revenu | `LANDLINE_LE_RECYCLE_SECONDS` (0 = non) |
| 3 | **Préférence rendue** | Plus aucune connexion en cours et `PreferredBearer=bredr` : remis à `le` tout de suite, lien monté ou non, et dès le début de la retenue | avec l'interrupteur général |
| 4 | **Connexion LE sortante retardée** | Après 90 s d'annonce sans réponse au lieu de 3 s après BR/EDR : elle ne retire plus l'annonce au moment où l'iPhone devrait y répondre | avec l'interrupteur général |

Interrupteur général : `LANDLINE_LE_FIRST=0` rend le comportement d'avant pour les quatre.

Garde commune : les mesures 1 et 2 ne jouent que si les notifications sont activées, si l'appairage a une
moitié LE, s'il n'est pas « cassé », **et si l'iPhone a déjà offert ANCS sur cet appairage** (UUID dans
`Device1.UUIDs`). Un appairage neuf, ou un iPhone dont le partage des notifications est coupé, n'est ni
retenu ni recyclé.

Pendant un recyclage, l'état publié garde `map_open` / `pbap_open` à vrai 20 s au plus, comme pour le
cycle mains-libres (`planned_drops()`). Le journal dit tout :
`no link to the iPhone; leaving BR/EDR alone for up to 10s…`, `the LE link came first, Ns after every link
was down`, `no LE link after Ns with the solicitation alone on air; connecting over BR/EDR`,
`BR/EDR up for Ns with the solicitation on air and no LE link; dropping every link on purpose, once…`.

Temps attendus (**déduits**) : (b) retour de bluetoothd → LE en 1 à 3 s, le reste en 8 s comme à 19:18 ;
(a), (c) relance du moteur avec BR/EDR monté → recyclage à +13 s environ, LE vers +15 s, tout vers +23 s.
Coût si l'hypothèse est fausse : jusqu'à 10 s de retard sur les messages après une perte complète, et une
coupure de quelques secondes par relance du moteur.

### 10.4 Survivre au redémarrage de bluetoothd

**Avant (lu).** Rien ne suivait le nom `org.bluez`. À la disparition de bluetoothd l'instantané restait tel
quel (liens « montés ») jusqu'aux premiers signaux du nouveau. L'annonce LE restait « enregistrée » aux yeux
du moteur : elle n'était remise qu'au renouvellement suivant, jusqu'à 160 s plus tard. Un moteur démarré
pendant l'absence de bluetoothd restait sans Bluetooth jusqu'à sa propre relance.

**Maintenant.**

- `BluezMonitor` surveille le nom (`g_bus_watch_name_on_connection`) : à la disparition, instantané vidé et
  une ligne `[WARN] bluetoothd left the bus…` ; au retour, `bluez_generation()` avance.
- Le fil de supervision voit le compteur bouger (`ConnectionState::bluez_restarted()`) : l'annonce est
  oubliée sans rien désenregistrer (`forget_ancs_solicitation()`) et réenregistrée dans le tour, à 20 ms ;
  refus de connexion mémorisé, sessions OBEX, compteurs de la retenue et du recyclage repartent de zéro ;
  la session ANCS est reconstruite sans `StopNotify`. La retenue (10.3) s'applique : LE d'abord.
- Agent d'appairage : il n'est enregistré que le temps d'un `bt_pair`, rien à refaire. Application GATT
  (côté réseau, désactivé ici) : `lan_info.cpp` suivait déjà le nom. obexd et PipeWire se réenregistrent
  seuls (vu au journal : `Endpoint registered` à 18:47:35).
- Démarrage sans bluetoothd : si `org.bluez` est un service que le bus sait lancer, le moteur démarre
  quand même et attend ; sinon (machine sans BlueZ) il reste sans Bluetooth, comme avant.

### 10.5 Journal perdu

`~/.local/state/landline` supprimé pendant que le moteur tourne : les écritures continuaient vers un fichier
sans chemin. Toutes les 5 s le moteur compare sa sortie au fichier attendu (`log_file_lost()`) ; s'ils
diffèrent, il recrée le dossier, rouvre le fichier et l'écrit (`[WARN] The log file … was gone`). Un dossier
impossible à créer au démarrage est créé dès que possible. Sans effet avec `LANDLINE_LOG_STDERR=1`.

### 10.6 Ce que le moteur demande à l'iPhone, et à quelle cadence

Revue faite après la baisse de batterie du 16 novembre (80 % à 18:35, 60 % à 19:18, dont un appel de
13 min par l'ordinateur). **Rien n'a été mesuré** ; ce tableau dit ce qui est borné et ce qui ne l'est pas.

| Activité | Cadence | Bornée ? |
|---|---|---|
| Relève des messages (MAP, un échange OBEX sur BR/EDR) | toutes les 15 s, tant que la session est ouverte | **non** : c'est la seule sollicitation sans fin. Réglable : `LANDLINE_MESSAGE_POLL_SECONDS` (5 à 3600). Non modifiée par défaut : c'est le délai d'arrivée d'un message |
| Sonde ANCS (écriture) tant que l'accès n'est pas accordé | 5 s | **oui, depuis cette branche** : 5 min, puis une fois par minute (`unanswered_seconds()`) ; avant, sans fin |
| Essais d'un profil refusé | 2 s | oui : 5 min, lien BR/EDR monté seulement |
| Découverte ANCS | 2 s | oui, et sans trafic radio (lecture de BlueZ) |
| Annonce LE | 20 ms pendant 180 s, puis 152,5 ms | c'est notre radio ; l'iPhone n'est sollicité que s'il se connecte |
| Connexion BR/EDR, iPhone injoignable | 5, 10, 20… jusqu'à 300 s (60 s si l'iPhone ne répond pas à l'appel) | oui. Vu à 19:17 : BlueZ répond « accepté » sans lien, le moteur redemande aussitôt, reçoit `InProgress` et double son attente — un appel D-Bus de trop par cycle, sans effet radio |
| Sessions MAP/PBAP, iPhone injoignable | 15, 30, 60, 120 s | oui |
| Recyclage LE | une fois, pas deux en 10 min | oui |
| Lecture GATT de contrôle du lien LE | à chaque tour (1 s) **si** `Bearer.LE1.Connected` lit faux sur un lien vivant | à vérifier sur cette machine (scénario L) |

Pistes non faites : relever les messages sur événement (une notification ANCS de l'application Messages
arrive par le LE) pour espacer la relève à plusieurs minutes ; mesurer le courant du lien BR/EDR au repos
(mode sniff) avec et sans relève.

### 10.7 Ce qui reste hypothétique sans iPhone

1. Que la séquence de 10.1 soit bien celle de 18:47 (prouvée possible, non prouvée survenue).
2. Que sans `StopNotify` les notifications reviennent après une coupure du seul LE : le source de BlueZ dit
   que l'abonné est réinscrit ; Tether pensait le contraire, sans l'avoir vérifié.
3. Toute la stratégie « LE d'abord » : que l'iPhone rouvre le LE pendant la retenue ; qu'il le rouvre après
   un recyclage ; que rien d'autre (iPhone, politique de BlueZ, WirePlumber) ne remonte BR/EDR pendant la
   retenue ; que 10 s et 12 s soient les bonnes durées, notamment iPhone verrouillé.
4. Que `Device1.UUIDs` garde l'UUID ANCS d'une vie de bluetoothd à l'autre (sinon ni retenue ni recyclage
   après un redémarrage — le journal le dira : pas de ligne `leaving BR/EDR alone`).
5. Que le recyclage n'ait pas lieu à tort au démarrage sur un lien LE vivant que BlueZ lirait « déconnecté »
   (il faudrait que l'abonnement ANCS prenne plus de 12 s).
6. Que la préférence rendue à `le` plus tôt ne gêne pas `Device1.Connect` (elle est réécrite à `bredr`
   juste avant chaque tentative).
7. Le comportement réel au retour de bluetoothd : ordre des signaux, délai avant que l'adaptateur accepte
   l'annonce, retour du son.
8. L'effet sur la batterie de l'iPhone, dans un sens ou dans l'autre.
