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

**Mise à jour (branche `moteur-le-apres-veille`).** Le 17 novembre, le lien LE n'est pas revenu après trois
sorties de veille de l'ordinateur. Le journal montre qu'il manquait déjà depuis la veille au soir ; il
montre aussi deux défauts du moteur (une retenue qui ne retenait rien, un lien LE à peine revenu mis en
concurrence avec un appel BR/EDR) et une annonce restée lente aux moments utiles. Analyse des trois pistes,
corrections et état publié `le_state` : section 11, résumée en section 1 bis. Compilé et testé, **pas
essayé avec l'iPhone**.

**Mise à jour (branche `moteur-trois-fautes`).** L'étude documentaire `iphone-link-study/12-ios-bluetooth-accessoires.md`
(guide de conception d'Apple R32 Update 1 lu directement, code de Pebble, forums) contredit trois
affirmations de la section 13 et précise le blocage de l'iPhone : iOS compte les liens LE morts **avant
d'avoir servi** et se retire au troisième. Règle des fautes, lien rendu utile plus vite, demande de
paramètres de connexion préparée (désactivée) : section 14. Les passages corrigés de la section 13 sont
marqués « corrigé le 17 novembre ». Compilé et testé, **pas essayé avec l'iPhone**.

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
| 11.2 Retenue sans fuite : sessions d'un lien tombé oubliées avant le superviseur | fait ; course **prouvée par le journal** | `Tick::links()`, `session_outlived_link()` dans `le_first.hpp` | `LANDLINE_LE_FIRST_STRICT=0` |
| 11.3 Lien LE venu en premier : BR/EDR et OBEX attendent 5 s | fait, **hypothèse non vérifiée** | `settle_le_before_classic()`, `BearerStatus::le_settling` | `LANDLINE_LE_SETTLE_SECONDS=0` |
| 11.4 Réveil de l'ordinateur vu (deux horloges) : annonce réenregistrée, retenue et recyclage réarmés | fait, **effet sur l'iPhone inconnu** | `SleepWatch` dans `le_after_sleep.hpp`, `ConnectionState::watch_sleep()`, `BearerSupervisor::resumed()`, `renew_ancs_solicitation()` | `LANDLINE_RESUME_WATCH=0` |
| 11.4 Annonce à 20 ms pendant 60 s quand tous les liens tombent, après un recyclage, au réveil | fait | `advert_interval_for()`, `BearerSupervisor::advert_rearmed_at()` | `LANDLINE_ADVERT_FAST_SECONDS=0` |
| 11.5 Lien LE d'un appareil non appairé à adresse privée résolvable : journalisé, annonce réenregistrée | fait ; hypothèse **peu probable, non exclue** | `BluezMonitor::le_stranger()`, `may_be_unresolved_phone()`, `ConnectionState::watch_strangers()` | `LANDLINE_LE_STRANGER=0` |
| 11.7 État publié `le_state`, `le_cause`, `le_detail` | fait | `le_state_for()`, `ConnectionState::refresh_status()` | — |
| 7.3 Annonce : 20 ms pendant 180 s, puis 152,5 ms sans limite ; renouvellement avant expiration | fait | `advert_interval_for()`, `advert_renewal_due()`, `supervise_ancs_solicitation()` | `LANDLINE_ADVERT_FAST_SECONDS=30` |
| 7.4 Tentatives OBEX espacées sans lien | fait | `ProfileSupervisor::tick()` | `LANDLINE_OBEX_ABSENT_BACKOFF=0` |
| 7.5 État publié à jour dans le tour | fait | `run_supervision_tick()`, `run_quick_steps()` | — |
| 16 obexd vérifié avant `CreateSession`, relancé par systemd ; motif `daemon_down` | fait, **démarrage de session réel à vérifier** | `ProfileSupervisor::daemon_ready()`, `recover_daemon()`, `ObexProfileOps::probe_daemon()` ; complément `system/obex-landline.conf` | aucun (complément : `uninstall-local.sh`) |

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

## 11. Le LE absent après la veille de l'ordinateur (branche `moteur-le-apres-veille`)

Compilé et testé (927 tests), **pas essayé avec l'iPhone**. Sources lues : journal du moteur installé
(`~/.local/state/tether/tetherd.log`, version `ff0efbd-dirty`), `journalctl -u bluetooth` et `journalctl -k`
des 16 et 17 novembre, l'arbre d'objets de BlueZ (lecture seule), le source de BlueZ 5.87
(`plugins/policy.c`, `src/device.c`, `src/advertising.c`, `src/main.conf`), le noyau (branche principale,
`net/bluetooth/hci_event.c`, `hci_sync.c`, lus en ligne et non sur l'étiquette 7.3), les notes d'Apple
(QA1931, forum développeurs). Aucun essai n'a été fait pour écrire cette section ; le protocole est dans
`essai-reconnexion.md`, scénarios M à P.

### 11.1 Ce que le journal prouve

**Le LE n'a pas été perdu à la sortie de veille. Il l'était depuis la veille au soir.**

| Heure | Fait (journal) |
|---|---|
| 16 nov. 22:23:25 | recyclage « LE d'abord » après 760 s de BR/EDR sans LE |
| 22:25:05 – 22:27:11 | le LE revient **cinq fois** (`the LE link came first`), et retombe à chaque fois en moins d'une seconde à quelques secondes (`Reason.Timeout`, `Reason.Unknown`) ; pendant ce temps BlueZ reconnecte BR/EDR de lui-même (`policy.c:reconnect_timeout() … Device or resource busy`, à répétition) |
| 22:27:11 | **dernier lien LE**. Ensuite : BR/EDR, MAP, PBAP montés, annonce en ondes, aucun LE |
| 22:34 → 08:08 | 9 h 30, ordinateur **éveillé** (aucune ligne `PM: suspend` au noyau avant 08:08:23 ; `soliciting ANCS` toutes les 2 min 40 sans trou) : pas de LE |
| 17 nov. 08:08:23–30, 08:08:59–08:18:08, 08:26:35–10:36:12 | trois veilles. À chaque réveil : `Reason.Suspend`, BR/EDR de retour en 17 à 40 s, pas de LE |
| 10:54:11 | un appareil LE inconnu, `4A:1B:2C:3D:4E:5F`, se connecte et repart dans la seconde ; au même instant le noyau écrit `hci0: Opcode 0x200e failed: -16` |
| 12:23:07 | coupure à la main ; BR/EDR revient en 2 s. 12:23:21 recyclage ; BR/EDR revient en 3 s. Pas de LE |

La veille de l'ordinateur n'est donc pas la cause : elle n'a fait que ne rien réparer. Ce qui est établi,
c'est que **depuis 22:27 l'iPhone ne répond plus à l'annonce**, quoi que fasse l'ordinateur (liens tous
coupés ou non, annonce à 20 ms ou à 152,5 ms), et que la veille au soir couper puis rallumer le Bluetooth de
l'iPhone a tout rétabli en moins de 10 s.

**Deux défauts de notre moteur sont prouvés par le même journal**, indépendamment de la cause :

1. *La retenue ne retenait rien* (11.2).
2. *Le lien LE revenu en premier était aussitôt mis en concurrence avec un appel BR/EDR* (11.3) — et c'est
   dans cette séquence, répétée cinq fois en deux minutes, que le LE a été vu pour la dernière fois.

### 11.2 Piste 1 — la course dans « LE d'abord » : prouvée, corrigée

**Observé**, à chaque perte complète avec une session OBEX ouverte (08:08:31, 08:18:09, 10:36:13,
12:23:08, 12:23:24) :

```
10:36:12.821  Device1 disconnected … Reason.Suspend
10:36:13.849  asking BlueZ for the BR/EDR link (Device1.Connect)
10:36:13.853  no link to the iPhone; leaving BR/EDR alone for up to 10s …
10:36:13.862  BlueZ answered the BR/EDR connect: … br-connection-busy
```

**Lu dans le code.** `Tick::links()` calculait `obex_up` *avant* d'oublier les sessions du lien tombé. Le
superviseur recevait donc « une session est ouverte », la prenait pour un lien, ne commençait pas la retenue
et demandait BR/EDR. Les sessions étaient oubliées juste après ; `profiles()` signalait un changement,
`links()` repassait dans le même tour — 4 ms plus tard — et commençait une retenue devenue sans objet. Le
même premier passage laissait aussi `ProfileSupervisor` rouvrir MAP, donc obexd appeler l'iPhone.

**Corrigé, à deux endroits.** `links()` oublie les sessions d'un lien BR/EDR tombé avant d'appeler le
superviseur ; et le superviseur lui-même ignore une session dont le lien vient de tomber
(`session_outlived_link()`), ce qui est testable sans BlueZ. Une machine qui n'offre aucun profil à
l'iPhone, où BlueZ ne lit jamais BR/EDR monté, n'est pas touchée : rien n'y « tombe ». Retour arrière :
`LANDLINE_LE_FIRST_STRICT=0`.

**Ce qui rouvre BR/EDR sans nous, et qu'on ne peut pas empêcher sans root.**

- *La politique de BlueZ* (`plugins/policy.c`, lu). `disconnect_cb()` n'arme la reconnexion que pour deux
  motifs. `MGMT_DEV_DISCONN_TIMEOUT` (perte de portée) : première tentative **1 s** après la perte, puis 2,
  4, 8, 16, 32, 64 s (`ReconnectIntervals`), pour tout appareil qui a un service de `ReconnectUUIDs` —
  l'iPhone en a deux (`111f`, `110a`). `MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND` (veille) : seulement si
  l'appareil a un service A2DP *sink* (`110b`) ; l'iPhone n'en a pas, donc **BlueZ ne reconnecte pas
  l'iPhone au réveil**. Rien dans ce fichier ne regarde `Trusted` : retirer la confiance ne change rien, et
  aucune propriété de `Device1` ne règle cette politique. Seul `main.conf` le peut (`[Policy]
  ReconnectAttempts=0`, ou `ReconnectUUIDs=` sans les UUID de l'iPhone), pour tous les appareils, en root.
  La retenue est donc sans effet après une perte de portée, et ce n'est pas réparable d'ici.
- *L'iPhone lui-même.* Au réveil, BlueZ répond `br-connection-busy` 1 s après la reprise, alors que ni la
  politique ni (désormais) le moteur n'ont rien demandé : `device.c` rend cette erreur quand une connexion
  ou une **découverte de services** est en cours, ce que provoque un lien BR/EDR entrant. L'iPhone rappelle
  ses accessoires connus de son propre chef. **Déduit**, à confirmer au `btmon` (scénario M).
- *obexd* ne se connecte que si le moteur ouvre une session : couvert par la retenue.
- `AutoEnable` ne concerne que la mise sous tension de l'adaptateur.

La retenue garantit donc seulement que *nous* ne demandons rien. Si l'iPhone rouvre BR/EDR dans la seconde,
la retenue est levée (un lien est là) et c'est le recyclage qui prend la suite, 12 s plus tard.

### 11.3 Le lien LE revenu en premier, perdu dans la seconde : prouvé, corrigé par hypothèse

**Observé**, quatre fois sur cinq le 16 au soir :

```
22:25:57.004  the LE link came first, 2s after every link was down; BR/EDR follows
22:25:57.004  asking BlueZ for the BR/EDR link (Bearer.BREDR1.Connect)
22:25:57.007  BlueZ answered the BR/EDR connect: … br-connection-busy
22:25:57.768  Bearer.LE1 disconnected … Reason.Timeout (Connection timeout)
```

Mêmes écarts à 22:25:05 (0,4 s), 22:26:13 (0,2 s), 22:26:57 (0,8 s). La cinquième fois (22:07:55) le lien
a tenu. Le moteur demandait BR/EDR dans le tour même où il voyait le LE ; BlueZ en avait déjà une en cours
(`busy` : la politique, après les `Reason.Timeout`).

**Déduit.** Une seule radio : appeler l'iPhone en BR/EDR (*page*) occupe l'antenne au moment où un lien LE
âgé de quelques intervalles doit tenir ses rendez-vous, et il tombe sur son délai de supervision. Non
prouvé — il faudrait le `btmon` de ces secondes — mais c'est la seule action de notre part dans la fenêtre.

**Corrigé.** Quand le LE monte seul, ni connexion BR/EDR ni session MAP/PBAP pendant 5 s (`le_settling`,
`settle_le_before_classic()`). Coût : messages et appels 5 s plus tard après une perte complète. Retour
arrière : `LANDLINE_LE_SETTLE_SECONDS=0`. La politique de BlueZ, elle, continue d'appeler après une perte
de portée.

### 11.4 Piste 3 — l'annonce après la veille

**BlueZ ne retire pas l'annonce à la mise en veille** (lu : `src/advertising.c` ne contient aucun traitement
de la veille ; une annonce n'est retirée qu'au départ du client, à l'expiration de son `Timeout` ou sur
`UnregisterAdvertisement`). C'est le noyau qui la suspend et la reprend (`hci_suspend_sync` /
`hci_resume_sync`, lus en ligne) ; ce chemin a eu deux correctifs en 2025. Relevé le 17 à 12:40 :
`LEAdvertisingManager1.ActiveInstances = 1` — BlueZ tient l'annonce pour enregistrée. **Qu'elle soit en
ondes après le réveil n'est pas établi** : seul `btmon` le dit (scénario M).

**Notre moteur ne voyait pas la veille.** Il n'écoute pas logind, et son horloge (`steady_clock`,
`CLOCK_MONOTONIC`) s'arrête pendant la veille : au réveil rien n'a changé pour lui, sauf un lien tombé.
Conséquences lues au journal : l'annonce n'était pas réenregistrée ; le recyclage, déjà fait la veille au
soir, restait « dépensé » ; et **l'annonce restait à 152,5 ms pendant toute la retenue**, parce que la
cadence rapide ne dépendait que de l'âge de la perte du LE (ici 12 h). Elle ne repassait à 20 ms que 17 s
plus tard, quand BR/EDR revenait, pour 30 s (`10:36:30 … 20-20 ms`, `10:37:00 … 152-153 ms`). Le moment
où l'iPhone a le plus de chances de répondre — plus aucun lien, annonce seule — se jouait donc à la cadence
lente.

**Corrigé.**

- *Réveil vu par deux horloges* (`SleepWatch`) : l'écart entre `CLOCK_BOOTTIME` et `CLOCK_MONOTONIC` ne
  grandit que pendant une veille. Pas de bus, pas de logind, et une veille pendant laquelle aucun lien
  n'était monté est vue aussi. Seuil : 3 s.
- Au réveil (`ConnectionState::watch_sleep()`) : une ligne `the computer slept for Ns…` ; l'annonce est
  **réenregistrée** (`renew_ancs_solicitation()`) ; retenue et recyclage repartent de zéro
  (`BearerSupervisor::resumed()`) ; refus de connexion mémorisé oublié ; découverte ANCS relancée.
  L'application GATT et l'agent ne sont pas concernés : la première n'existe que partie réseau activée,
  le second que le temps d'un `bt_pair` (10.4).
- *Cadence* : 20 ms pendant 60 s (`ADVERT_REARM_FAST_SECONDS`) chaque fois que tous les liens viennent de
  tomber, après un recyclage, et au réveil, quel que soit l'âge de la perte du LE.

Retour arrière : `LANDLINE_RESUME_WATCH=0` ; `LANDLINE_ADVERT_FAST_SECONDS=0` coupe toute cadence rapide.

### 11.5 Piste 2 — l'adresse privée non résolue : peu probable, non exclue

**Observé.** Un seul événement en 14 h : 10:54:11, `Bearer.LE1 disconnected from …/dev_4A_1B_2C_3D_4E_5F:
Reason.Unknown`, 18 min après le réveil et sans rapport d'heure avec nos annonces. `49` = `01001001` : les
deux bits de poids fort valent `01`, c'est une adresse privée **résolvable**. L'objet a disparu depuis
(appareil temporaire de BlueZ, 30 s). `/var/lib/bluetooth` n'est pas lisible sans root : la présence de la
clé d'identité (IRK) de l'iPhone n'est pas vérifiable d'ici ; BlueZ n'expose pas les clés sur D-Bus.

**Ce qui va contre l'hypothèse.**

- Dans le noyau, la résolution d'une adresse privée à l'arrivée d'une connexion se fait **côté hôte**
  (`le_conn_complete_evt` → `hci_get_irk`), dans les deux rôles, sans dépendre de la liste de résolution du
  contrôleur ni de `ll-privacy`. Une liste de résolution perdue au réveil ne suffit donc pas à produire un
  appareil inconnu : il faudrait que le noyau n'ait plus l'IRK, ou que l'iPhone en ait changé. (Lu en ligne
  sur la branche principale, pas sur 7.3.)
- `Opcode 0x200e` est *LE Create Connection Cancel*, que le noyau n'envoie que quand **l'ordinateur** ouvre
  une connexion LE en tant que central ; `-16` est « commande refusée ». À cet instant l'ordinateur se
  connectait donc à quelqu'un (souris, clavier, manette appairés en LE se reconnectent ainsi), ce qui
  désigne plutôt un autre appareil du voisinage ou une collision entre deux connexions.
- Le LE manquait déjà 11 h avant le premier réveil.
- Recherche : aucun rapport trouvé pour cette combinaison (BlueZ, noyau, ancs4linux). Le plus proche : un
  périphérique BlueZ 5.50 qui ne résolvait plus l'adresse d'un iPhone appairé après redémarrage, guéri en
  relançant bluetoothd.

**Ce qui la laisse ouverte.** Tout appareil peut se connecter à une annonce connectable ; un iPhone utilise
toujours une adresse résolvable ; et l'événement n'a pas été capturé. Un seul `btmon` tranche : rôle de la
connexion (`Role: Peripheral` ou `Central`) et adresse d'identité résolue ou non (scénario N).

**Fait.** Le moniteur compte les liens LE qui se ferment sur un appareil sans appairage
(`BluezMonitor::le_stranger()`). Si l'adresse est privée résolvable, que l'iPhone appairé n'a pas de LE,
qu'il a déjà offert ANCS et que l'annonce est enregistrée (`may_be_unresolved_phone()`), le moteur écrit
`[WARN] an LE device BlueZ holds no pairing for (…) connected and left…`, publie la cause
`unresolved_address` pendant 15 min, et réenregistre l'annonce, pas plus d'une fois par 5 min. Retour
arrière : `LANDLINE_LE_STRANGER=0`.

**Ce qui n'est pas fait, et pourquoi.** Couper puis rallumer l'adaptateur (`Adapter1.Powered`) ferait
recharger les clés au contrôleur, mais coupe le clavier, la souris et le son de l'utilisateur pour une
hypothèse peu probable. Redémarrer bluetoothd demande root. Si le scénario N confirme l'hypothèse, la
réponse sera l'un des deux, proposé à l'utilisateur et non fait d'office.

### 11.6 Cause la plus probable

| Explication | Pour | Contre | Jugement |
|---|---|---|---|
| L'iPhone ne rouvre plus le LE de son côté (« coincé »), depuis la série de liens LE avortés de 22:25–22:27 | guéri la veille en basculant son Bluetooth ; annonce enregistrée ; rien ne change quoi que fasse l'ordinateur ; Apple (forum développeurs) : pour un accessoire double mode, se reconnecter ne rétablit que le côté classique et l'accès à ANCS « n'est pas garanti » | mécanisme interne à iOS, invérifiable | **la plus probable, ≈ 65 %** |
| Notre annonce n'est plus réellement en ondes (ou plus connectable) alors que BlueZ la croit active | expliquerait tout aussi bien ; jamais vérifié au `btmon` depuis 22:27 ; l'annonce est renouvelée toutes les 160 s par retrait puis remise | elle l'était à 22:25–22:27 (l'iPhone y a répondu cinq fois) | ≈ 20 %, **tranché par le scénario M en 2 min** |
| Adresse privée non résolue | l'appareil de 10:54 | 11.5 | ≈ 10 % |
| La veille de l'ordinateur | — | le LE manquait déjà 9 h 30, ordinateur éveillé | écartée comme cause ; aggravante seulement (11.4) |

Les corrections de cette branche ne guérissent pas un iPhone coincé : elles retirent ce qui, de notre côté,
a pu le coincer (11.3), cessent de gâcher les occasions qu'il a de revenir (11.2, 11.4), et disent à
l'utilisateur que les notifications sont en pause et pourquoi (11.7).

### 11.7 État publié : `le_state`, `le_cause`, `le_detail`

`bt_connection_changed` porte trois champs de plus (`le_state_for()`, détail dans `moteur-api.md`,
section 3). `le_state` vaut `connected`, `waiting` (lien perdu depuis moins de 180 s, ou retenue en cours),
`paused` (au-delà), `unavailable` ou `off`. En pause, `le_cause` vaut `after_resume`, `phone_silent`,
`phone_away`, `unresolved_address`, `advert_off_air` ou `phone_backoff` (§13), et `le_detail` est une phrase prête à afficher.
En attente, `le_cause` peut valoir `holding_off` (annonce retirée exprès après un lien mort avant service, §14.2).
Chaque changement écrit une ligne `notification link <état>: <cause>`.

### 11.8 Ce qui reste hypothétique sans iPhone

1. Que l'annonce soit en ondes, connectable, à l'intervalle demandé, après un réveil (M).
2. Qui rouvre BR/EDR une seconde après le réveil (M).
3. Que 5 s d'installation suffisent au lien LE, et que ce soit bien l'appel BR/EDR qui le faisait tomber (O).
4. Que le réenregistrement de l'annonce au réveil change quoi que ce soit pour l'iPhone (M, P).
5. L'identité de l'appareil à adresse privée (N).
6. Que `CLOCK_BOOTTIME` avance bien pendant la veille sur cette machine (M : la ligne `the computer slept`).
7. Qu'un iPhone coincé le reste après un redémarrage de bluetoothd ou de l'ordinateur (P).

## 12. Mode sobre : le LE seul lien permanent, le classique à la demande (branche `moteur-sobre`)

Compilé et testé (1 073 tests), **pas essayé avec l'iPhone**. Conception et preuves :
`iphone-link-study/11-classique-a-la-demande.md`. Socket : `moteur-api.md`, section 13. Écarts :
`moteur-sobre-ecarts.md`. Essais à faire : `essai-reconnexion.md`, série « mode sobre » (E1 à E9).

### 12.1 Pourquoi

Le journal des 16 et 17 novembre (§11) dit deux choses : sur cette machine le LE ne revient pas tant que le
BR/EDR est monté, et c'est BlueZ lui-même qui remonte le BR/EDR au réveil dès qu'un profil audio était
connecté à la veille. Les mécanismes des sections 5 à 11 (retenue, recyclage, cycle mains-libres,
`PreferredBearer`) coordonnent deux liens qui se gênent. Le mode sobre ne les coordonne plus : il n'en tient
qu'un.

### 12.2 Ce qui change dans la supervision

| Mécanisme du mode complet | En mode sobre |
|---|---|
| Connexion BR/EDR permanente, reculs 5…300 s | aucune tentative au repos ; une connexion seulement quand un bail voix la demande, réessayée toutes les 3 s pendant l'ouverture (20 s) |
| Retenue « LE d'abord » (10 s), installation du LE (5 s) | sans objet : rien n'est demandé sur BR/EDR sans LE |
| Recyclage de tous les liens après 12 s | jamais |
| Demande puis cycle mains-libres (5 s, 8 s) | jamais au repos ; une seule variante, dans la machine à baux : bail voix sur un lien de données → demande du profil sur le lien en place, puis fermeture et réouverture par vraie connexion si le mains-libres n'est pas venu en 8 s, une fois (tout de suite pour une sonnerie) |
| Écriture de `PreferredBearer` | jamais pour ouvrir. Remis une fois à `le` s'il est trouvé à `bredr` (écart 20) |
| Sessions MAP et PBAP tenues en permanence, reculs iPhone absent | ouvertes seulement sous bail ; fermées (`RemoveSession`) avant le lien ; le dernier verdict est gardé |
| Relève des messages toutes les 15 s | à la notification, à l'ouverture, une fois par heure (aussi en mode complet, étape M1) |
| Lecture du carnet à chaque session, différée de 5 min | nombre de fiches seul ; relecture s'il change ou une fois par jour |
| Reconnexion par BlueZ au réveil | plus de prise : le lien classique est fermé avant la veille (annonce `PrepareForSleep` de logind, inhibiteur de 3 s au plus) |

Le code du mode complet n'est pas modifié : `BearerSupervisor::supervise()` bifurque vers
`supervise_on_demand()` après la lecture des liens et l'examen de l'appairage, communs aux deux modes ; un
appairage cassé ou partiel est traité de la même façon. Tout ce qui concerne le LE (annonce, intervalle
rapide, détection du réveil, appareil inconnu, garde de l'abonnement ANCS, `le_state`) est inchangé et
devient le seul point à soigner.

### 12.3 La machine à baux (`classic_demand.hpp`)

```
          bail, LE monté               dernier bail rendu
 idle ───────────────► opening ──► open ───────────► lingering ──► closing ──► idle
   ▲       échec          │          ▲   nouveau bail     │ délai
   └──── failed ◄─────────┘          └────────────────────┘
```

Une fonction pure, `ClassicDemand::step(DemandFacts)`, appelée à chaque passage des étapes rapides, juste
après `notifications()` (étape `demand()` de `supervision_tick.hpp`). Elle rend une action : `OpenData`
(obexd ouvre les sessions, ce qui monte un lien sans profil audio), `OpenVoice` (`Bearer.BREDR1.Connect`,
ou demande du mains-libres sur le lien en place), `RecycleForVoice`, `Close` (sessions retirées, puis
`Bearer.BREDR1.Disconnect`). Baux, maintiens et garde-fous : `moteur-api.md`, section 13.

Le lien **ouvert par l'iPhone** (BR/EDR monté sans qu'aucun bail ne l'ait demandé) est adopté : relève si la
dernière a plus de 10 min, mains-libres utilisé s'il vient, fermeture après 120 s sans appel, sans son et
sans opération. Garde-fou : une réouverture par l'iPhone moins de 60 s après une fermeture est comptée ; deux
en 15 min, et le moteur repasse en mode complet pour 30 min (`link_mode_reason:"phone_keeps_link"`), puis
réessaie. Jamais plus de quatre fermetures par heure d'un lien ouvert par l'iPhone.

### 12.4 Ce qu'on lit dans le journal

| Ligne | Sens |
|---|---|
| `bluetooth: link mode lean (LE kept, the classic link on demand)` | mode appliqué au démarrage ou après un changement |
| `bluetooth: link mode full (both links kept), instead of lean: notifications_off` | repli, avec son motif |
| `bluetooth: no LE link for 30s while the classic link is up; closing the classic link once so that the iPhone comes back over LE (its leases wait for it)` | #43 : le LE tombé sous un classique monté (18 novembre, 18:18) ; une fois par 10 min, jamais sous un appel, un son qui joue ou le partage de connexion (`moteur-quotidien.md`, section 1) |
| `bluetooth: the iPhone's sound asked for here; the classic link is offered for 120s` | #43 : le son demandé ici ; le bail `audio` dure 120 s, puis tant que le son joue et 60 s après |
| `bluetooth: sync wanted (the iPhone is back)` / `(notifications waiting on the iPhone)` | bail `sync` pris |
| `bluetooth: message sync asked (ancs)` (DEBUG) | une notification de Messages demande la relève |
| `bluetooth: inbox listed (session)` / `(request: ancs)` / `(retry: ancs)` / `(background)` (DEBUG) | chaque relève et sa cause : au repos, il ne doit plus y en avoir entre deux notifications |
| `bluetooth: classic link opening; opening for data (leases: arrival; opened by engine)` | chaque changement d'état de la machine, avec les baux tenus |
| `bluetooth: asking BlueZ for the BR/EDR link on demand (Bearer.BREDR1.Connect)` | ouverture voix |
| `bluetooth: BR/EDR link closed on purpose (nothing needs it); the LE link stays` | fermeture |
| `bluetooth: the iPhone opened the BR/EDR link itself; using it, then closing it` | lien adopté |
| `bluetooth: a message arrived and the classic link is not opened for it (hourly ceiling reached)` | plafond des 12 ouvertures automatiques |
| `bluetooth: incoming call ringing (answer here: connecting)` puis `(answer here: ready, seen by hands-free)` | étape M3 |
| `bluetooth: dial accepted by the iPhone` / `dial not placed: no_hands_free` | étape M4 |
| `sleep: logind says the computer is about to sleep` | suivi, si le lien est ouvert, de la fermeture |

Les lignes du mode complet (`leaving BR/EDR alone`, `dropping every link on purpose`, `dropping the BR/EDR
link on purpose, once`, `standing back`) **ne doivent plus apparaître** en mode sobre : en trouver une est un
défaut.

### 12.5 Retours arrière

| Pour revenir sur | Faire |
|---|---|
| tout le mode sobre | `bt_set_link_mode full`, ou `LANDLINE_LINK_MODE=full` dans l'environnement du démon |
| la relève déclenchée par ANCS | `LANDLINE_MESSAGE_POLL_SECONDS=15` |
| la fermeture avant la veille | `LANDLINE_SLEEP_CLOSE=0` |
| l'attente d'un LE stable avant d'ouvrir (§13) | `LANDLINE_LE_STABLE_SECONDS=0` |
| l'espacement après une coupure LE imputée au classique (§13) | `LANDLINE_CLASSIC_QUIET_SECONDS=0` |
| l'annonce retirée pendant une rafale de coupures LE (§13) | `LANDLINE_LE_BURST_PAUSE=0` (ne joue plus qu'avec `LANDLINE_LE_STRIKES=0`) |
| la règle des fautes : liens LE morts avant service comptés, annonce rapide 30 s puis 152,5 ms (§14.2, §14.7) | `LANDLINE_LE_STRIKES=0` (l'ancienne règle des rafales reprend) |
| l'annonce **laissée à l'antenne** après un lien mort avant service (#29, §14.7) | `LANDLINE_LE_STRIKE_PAUSE_SECONDS=15 LANDLINE_LE_STRIKE_LONG_PAUSE_SECONDS=30 LANDLINE_LE_STRIKE_SLOW=2 LANDLINE_LE_STRIKE_FAST_SECONDS=0` (retraits et ralenti d'avant le 18 novembre au soir) |
| la passe de supervision déclenchée par le lien LE et par ANCS (§14.3) | `LANDLINE_LE_PROMPT=0` |
| la demande de paramètres de connexion (§14.4) | désactivée par défaut ; `LANDLINE_LE_CONN_PARAMS=1` l'active |
| l'ouverture à la sonnerie et `bt_incoming_call` | `LANDLINE_ANCS_CALLS=0` |
| la passerelle « pas encore prête » | `LANDLINE_GATEWAY_STRICT=0` |

Tant que E1, E2 et E7 ne sont pas passés, l'unité systemd d'essai peut porter `LANDLINE_LINK_MODE=full`.

### 12.6 Ce qui reste hypothétique sans iPhone

1. Que le LE seul tienne des heures sur l'Intel 9560, iPhone verrouillé en poche (E1).
2. Que l'iPhone laisse le BR/EDR fermé quand l'ordinateur l'a fermé, et à quelle fréquence il le rouvre (E1).
   Le garde-fou rend la réponse sans conséquence pour le fonctionnement, pas pour le gain.
3. Que l'ouverture du classique ne fasse pas tomber le LE (E2) : les rafales de coupures LE du journal ont
   toutes eu lieu pendant qu'un BR/EDR montait.
4. Qu'une session ouverte par obexd alors que le LE est monté s'ouvre en 2 à 4 s (E2), et que
   `Bearer.BREDR1.Connect` amène bien le mains-libres en 7 à 9 s (E4, E5) : une seule mesure à ce jour.
5. Que `Bearer.BREDR1.Disconnect` ferme le lien que obexd a ouvert, sans effet sur le LE (E2).
6. Que la notification de Messages précède de peu la présence du message dans MAP (E3) : l'anti-rebond de
   2 s et les relances à +3 s et +10 s en dépendent.
7. Que les actions de la notification d'appel décrochent et refusent (E4), et qu'un mains-libres ouvert
   pendant la sonnerie voie l'appel (E4).
8. Que logind accorde l'inhibiteur de veille au démon et attende sa libération (E7) ; à défaut la fermeture
   avant la veille n'a pas le temps de se faire et BlueZ remonte le lien au réveil, comme avant.
9. Que la passerelle PipeWire réponde bien à `GetManagedObjects` dès que la liaison mains-libres est
   établie (E5) : sinon `calls_available` resterait à `on_demand` et la composition échouerait en
   `no_hands_free` ; `LANDLINE_GATEWAY_STRICT=0` lève le doute.
10. Que le profil réseau de l'iPhone (`org.bluez.Network1.Connected`) soit bien le signe d'un partage de
    connexion en cours sur ce lien.

## 13. Le LE fragile : coupures à l'ouverture du classique, iPhone qui ne répond plus (branche `moteur-le-fragile`)

Compilé et testé (1 128 tests), **pas essayé avec l'iPhone, aucun `btmon`**. Sources : journal réel du 16 et
du 17 novembre (`landlined.log`), source du noyau (`net/bluetooth/l2cap_core.c`, `hci_core.c`, `hci_event.c`)
et de BlueZ (`src/gatt-database.c`, `src/main.conf`) lus en ligne, Accessory Design Guidelines d'Apple (par
citations de seconde main : le PDF n'a pas pu être chargé), forums Apple et Nordic DevZone. Essais à faire :
`essai-reconnexion.md`, série « LE fragile » (R1 à R5).

**Corrigé le 17 novembre (étude 12, section 14).** Le guide d'Apple a depuis été lu directement (R32
Update 1, 30 octobre 2026). Trois affirmations de cette section étaient fausses et sont rectifiées à leur
place : les bornes du délai de supervision (6 à 18 s, et non 2 à 6 s), le « 420 ms » que le noyau
demanderait, et l'effet du réglage `main.conf`. La mesure (d) de 13.4 est remplacée par la règle des fautes
(14.2) ; le réglage root (e) et l'essai R4 sont abandonnés au profit de la demande HCI (14.4).

### 13.1 Ce que le journal dit (17 novembre, mode sobre)

| Heure | Fait |
|---|---|
| 17:43:41 – 17:44:15 | départ de l'utilisateur : le LE tombe et revient **9 fois en 35 s**, **sans aucun lien classique** (`Timeout` 1 à 4 s après chaque retour, 2 `Unknown`) |
| 18:04:26 – 18:05:45 | retour : le LE revient et retombe **une vingtaine de fois en 80 s**, toujours **sans lien classique** (sessions de notification 11 à 25) |
| 18:05:45,5 | LE monté ; 18:05:48,9 notifications actives |
| 18:05:49,743 | bail `sync` → ouverture du classique (obexd, donc *page*) |
| 18:05:50,462 | LE `Timeout`, **0,719 s** après l'ouverture |
| 18:05:54,96 | classique monté (5,2 s d'ouverture) ; 18:05:55,58 LE revenu, classique monté |
| 18:06:02,895 | LE `Timeout`, classique monté et au repos depuis 8 s |
| 18:06:03,6 → 06,4 | fermeture du classique (`Reason.Local`, 2,8 s) |
| ensuite | plus aucun lien LE ; annonce renouvelée toutes les 2 min 40 |

Ouvertures et fermetures du classique en mode sobre le 17 (16:02 → 18:06) et sort du LE :

- ouvertures par le moteur : 16:05:31, 16:06:46, 16:07:13 (voix), 16:31:32, 17:24:28 (données, 4,4 s et
  5,7 s) : **LE intact** ; 18:05:49 : **LE perdu en 0,72 s**. Soit 1 sur 6.
- fermetures : 16:06:20, 16:07:03, 16:08:05, 16:31:49, 17:09:01, 17:19:42, 17:24:45 : **LE intact** (7) ;
  16:02:21 : LE perdu 1,2 s après, mais avec `Reason.Remote` — c'est **l'iPhone** qui a fermé le LE, 0,18 s
  avant de fermer aussi le classique, et non un délai dépassé ; le partage des notifications venait d'être
  activé (session ANCS pas encore établie). Revenu seul en 4 s. 18:06:03 : le LE était déjà tombé.

Le 16 au soir (mode complet), mêmes écarts entre une demande BR/EDR et la coupure LE : 22:25:05 (0,42 s),
22:25:57 (0,76 s), 22:26:13 (0,18 s), 22:26:57 (0,82 s) ; liste complète produite par un script sur le
journal (34 coupures LE dans les 15 s suivant une demande, un recyclage ou une chute de BR/EDR).

### 13.2 Q1 — pourquoi le LE tombe quand on ouvre le classique

- **Établi (sources)** : iOS ouvre ses liens LE avec un intervalle de 30 ms, latence 0 et un **délai de
  supervision de 720 ms** (relevé au renifleur publié sur Nordic DevZone ; fil du forum Apple d'août 2026,
  `lsto: 72`). Un lien LE qui ne réussit aucun échange pendant 0,72 s est donc perdu.
- **Probable** : 0,719 s entre l'ouverture et la coupure à 18:05, 0,76 s et 0,82 s le 16 : à partir du début
  de la recherche de l'iPhone (*page*), **plus aucun rendez-vous LE n'a été servi**. Une seule radio ; le
  contrôleur donne la priorité à la page. Quand l'iPhone répond vite à la page (à côté, 16:31, 17:24), elle
  dure moins de 0,72 s et le LE survit ; quand il répond mal (arrivée, signal faible : 5,2 s d'ouverture à
  18:05), le LE meurt. Aucune source publiée ne décrit ce défaut sur l'Intel 9560 : c'est une déduction,
  à confirmer au `btmon`.
- **Établi (journal)** : la plupart des coupures du 17 n'ont **rien à voir avec le classique** : 17:43 et
  18:04–18:05, trente coupures sans aucun lien classique, en limite de portée. Avec 720 ms de délai, un lien
  LE ne tient pas à la limite de portée, et notre annonce à 20 ms le fait renaître aussitôt.
- **Fermeture** : pas de preuve qu'elle fasse tomber le LE (7 fermetures sans effet ; 16:02 est une
  fermeture par l'iPhone, autre mécanisme).
- **Paramètres plus tolérants** (*corrigé le 17 novembre*) : Apple **ne lit pas** la caractéristique PPCP et
  attend une demande du périphérique. Ses règles, dans la révision en vigueur (R32 Update 1, §57.6) : délai
  de supervision de **6 à 18 s** (la règle « 2 à 6 s » citée ici venait de la révision de 2017), intervalle
  minimal multiple de 15 ms, intervalle maximal × (latence + 1) ≤ 6 s ; la procédure attendue est celle de
  la couche liaison (*Connection Parameters Request*), pas seulement la demande L2CAP. Le noyau n'envoie sa
  demande L2CAP que si l'intervalle sort de la fenêtre de l'adaptateur (30–50 ms) — or iOS choisit 30 ms,
  donc rien n'est demandé. Et s'il la faisait, il y recopierait le délai de supervision **en cours** (720 ms,
  `req.to_multiplier = hcon->le_supv_timeout`), pas celui de l'adaptateur : la phrase « il demanderait
  420 ms » était fausse, 420 ms étant la valeur par défaut des liens que l'ordinateur ouvre lui-même. BlueZ
  tient lui-même le service GAP et refuse qu'une application l'enregistre. **Aucune voie sans privilège**
  (confirmé en 14.4). Le réglage root proposé ici — `/etc/bluetooth/main.conf`, section `[LE]`,
  `ConnectionSupervisionTimeout=600` — **ne ferait pas demander 6 s** : pour un lien entrant, seuls les
  intervalles viennent de ce fichier ; avec un intervalle sorti de la fenêtre, la demande partirait avec
  720 ms, qu'iOS refuserait. Il est abandonné. La voie qui reste est la commande HCI *LE Connection Update*
  (14.4).

### 13.3 Q2 — pourquoi l'iPhone ne répond plus

- **Établi (journal)** : l'annonce est en ondes et connectable (le 16, bascule du Bluetooth de l'iPhone →
  LE en quelques secondes sans que l'ordinateur change rien). Le blocage est côté iPhone.
- **Probable, non prouvé** (*précisé le 17 novembre, 14.1*) : iOS cesse de se reconnecter à un accessoire
  dont trois liens LE de suite se terminent **sans qu'il ait utilisé un service** — ce n'est pas le nombre
  de coupures qui compte (console d'un iPhone publiée sur StackOverflow en 2015, « did not use any built-in
  service, strike #3 » ; phrase du guide d'Apple §57.12.4 ; aucun seuil ni délai documenté par Apple). Les deux épisodes (16 nov. 22:25–22:27,
  17 nov. 18:04–18:06) suivent une rafale ; mais la rafale de 17:43 (9 liens) n'a pas empêché le retour à
  18:04, 20 min après.
- Le 17 à 13:16 le LE est revenu seul, 14 h 50 après le blocage de 22:27 (cause inconnue, à demander à
  l'utilisateur).
- Aucune parade publiée (annonce retirée puis remise, annonce dirigée — déconseillée par Apple —, changement
  d'adresse). La défense est la **prévention** : ne pas laisser l'iPhone enchaîner des liens LE avortés.
- Geste utilisateur, le seul sûr : Réglages › Bluetooth de l'iPhone, couper puis rallumer (pas le centre de
  contrôle, qui ne coupe pas vraiment). Le moteur le dit dans `le_detail`.

### 13.4 Corrections (`le_health.hpp`, `LeHealth`)

Une classe pure, alimentée une fois par tour (`watch_le_health()`, juste après la lecture des liens) par le
compte des signaux `Disconnected` de `Bearer.LE1` — elle voit donc aussi les liens nés et morts entre deux
tours — et par ce que la machine à baux vient de faire au classique.

| # | Mesure | Détail | Retour arrière |
|---|---|---|---|
| a | **Classique seulement sur un LE stable** | Les ouvertures que personne n'a demandées (bail `sync` au retour, `arrival` d'un message, `hourly`, et le bail `audio` permanent) attendent un lien LE monté depuis **45 s sans coupure**, notifications actives. Les liens des rafales du 17 ont vécu 1 à 8 s ; 45 s laissent aussi passer la négociation des paramètres par iOS. Un lien classique déjà là (ouvert par l'iPhone, ou pour un appel) est utilisé tout de suite : il ne coûte pas de *page*. Appel, envoi d'un message, lecture demandée par l'interface : **jamais retenus**. Le retard n'est pas une perte : les messages arrivés sont déjà annoncés par ANCS | `LANDLINE_LE_STABLE_SECONDS=0` (0 à 3600) |
| b | **Cause à effet** | Une coupure LE dans les 10 s qui suivent une ouverture ou une fermeture du classique lui est imputée : `[WARN] the LE link dropped Ns after the classic link was opened; the classic link is not opened again for 300s unless a call or the user asks for it`. 300 s, doublés à chaque récidive dans l'heure, plafond 1 800 s. Avec (a), il n'y a de toute façon pas de deuxième ouverture dans la minute | `LANDLINE_CLASSIC_QUIET_SECONDS=0` (0 à 1800) |
| c | **Fermeture** | Non modifiée : 7 fermetures sur 7 sans effet sur le LE, et celle de 16:02 est une fermeture par l'iPhone. Elle est couverte par (b) : si une fermeture fait tomber le LE, c'est dit au journal et les ouvertures s'espacent | — |
| d | **Rafale** (*remplacée par la règle des fautes, 14.2 ; ne s'applique plus qu'avec `LANDLINE_LE_STRIKES=0`*) | 3 coupures LE en 60 s : l'annonce est **retirée 30 s** (60 puis 120 s si la rafale reprend), `[WARN] the LE link dropped 3 times within 60s; the advertisement is taken off air for 30s…`. Oublié quand un lien a tenu 45 s (`the LE link has held`). Le soir du 17, cela aurait offert à l'iPhone 6 à 8 liens au lieu de 20. **Hypothèse** : que ce soit le nombre de liens avortés qui « coince » l'iPhone | `LANDLINE_LE_BURST_PAUSE=0` (la rafale reste journalisée) |
| d | **État publié** | `le_cause:"unsteady"` (état `waiting`) pendant une rafale ; `le_cause:"phone_backoff"` (état `paused`) quand le LE est perdu depuis 180 s **à la suite d'une rafale** et que le classique a joint l'iPhone depuis — il est donc là. Sans cette preuve de présence la cause reste `phone_away`, avec une phrase qui dit les deux possibilités | — |
| e | **Paramètres LE** | Rien de possible sans privilège (13.2). Le réglage `main.conf` (R4) ne marcherait pas ; demande HCI préparée et désactivée (14.4) | `LANDLINE_LE_CONN_PARAMS` |

Hors mode sobre, (d) s'applique aussi ; (a) et (b) ne concernent que la machine à baux.

### 13.5 Ce qui est prouvé, probable, hypothétique

| Affirmation | Degré |
|---|---|
| iOS ouvre ses liens LE à 30 ms / 720 ms de supervision | établi (sources), non relevé sur cette machine |
| La majorité des coupures LE du 17 ont lieu sans aucun lien classique, au départ et à l'arrivée | **prouvé** (journal) |
| Le LE tombe 0,72 s après le début d'une ouverture du classique quand l'iPhone tarde à répondre | **prouvé** pour les délais (5 cas), **probable** pour le mécanisme (page prioritaire sur l'unique radio) |
| La fermeture du classique fait tomber le LE | **non** : 0 cas sur 7 ; 16:02 est une fermeture par l'iPhone |
| L'annonce est bien en ondes pendant le silence de l'iPhone | prouvé le 16 (bascule du Bluetooth de l'iPhone → LE en secondes) ; **non vérifié le 17** |
| iOS cesse de rouvrir le LE après une rafale de liens avortés | **probable**, témoignages seulement, aucune règle publiée, un contre-exemple partiel (17:43 → 18:04) |
| Retirer l'annonce pendant une rafale évite le blocage | **hypothèse** |
| Ce qui débloque sans toucher à l'iPhone | **inconnu** : le 17 à 13:16 le LE est revenu seul après 14 h 50 ; rien de ce que l'ordinateur a fait entre-temps (recyclages, veilles, relances) n'a eu d'effet immédiat |
| 6 s de supervision via `main.conf` feraient tenir le LE | **faux** (*corrigé le 17 novembre*) : le noyau ne porte pas ce délai dans sa demande (13.2). Par la commande HCI (14.4) : **hypothèse**, iOS peut refuser ou répondre par d'autres valeurs |

## 14. Les « trois fautes », le lien vite utile, les paramètres de connexion (branche `moteur-trois-fautes`)

Compilé et testé (1 174 tests), **pas essayé avec l'iPhone, aucun `btmon`, aucune commande privilégiée**.
Sources : `iphone-link-study/12-ios-bluetooth-accessoires.md` (étude 12) ; source de BlueZ 5.87 (copie
locale : `src/device.c`, `src/gatt-client.c`, `src/shared/gatt-client.c`, `profiles/battery/battery.c`) ;
noyau v7.3 lu en ligne (`net/bluetooth/hci_sock.c`, `l2cap_sock.c`, `mgmt.c`) ; pages de manuel
`org.bluez.*` de la 5.87 installée ; journal réel du 17 novembre. Variables : `moteur-api.md`, section 11.
Essais : `essai-reconnexion.md`, section 0 (P1 à P3).

### 14.1 Ce que dit l'étude 12, et ce qui en est appliqué

L'essentiel, en quatre points :

1. **iOS compte les liens LE qui meurent avant d'avoir servi.** Console d'un iPhone (2015, iOS 8) :
   « *Peripheral … did not use any built-in service, strike #1* », puis #2, #3, après quoi l'iPhone ne
   revient plus sans un geste. Le guide d'Apple dit la même chose à l'endroit (§57.12.4) : l'iPhone garde
   le lien « *as long as it is paired and uses one of the available services* » (Batterie, Heure, ANCS).
   Ancien, recoupé par notre journal, **non vérifié sous iOS 26**.
2. **Notre lien LE vit avec 720 ms de délai de supervision** parce que nous ne demandons rien ; les
   fabricants demandent autre chose quelques secondes après la connexion (Pebble : 150–180 ms, latence 0,
   6 s). Apple admet 6 à 18 s. C'est la cause première des rafales en limite de portée et de la chute du
   LE pendant la recherche de l'iPhone en classique.
3. **Le réglage `main.conf` ne marcherait pas** ; il faut la commande HCI *LE Connection Update*.
4. Annonce, ordre des liens, classique à la demande : conformes.

Les onze changements que l'étude recommande, et leur sort dans cette branche :

| # | Changement recommandé | Ici |
|---|---|---|
| 1 | Demander 6 s de supervision après chaque lien LE | **préparé, désactivé** (14.4) ; l'essai manuel P1 passe d'abord. Rectification : la capacité requise est `CAP_NET_RAW`, pas `CAP_NET_ADMIN` |
| 2 | Compter les liens morts avant l'abonnement, retirer l'annonce dès le premier | **fait** (14.2), puis **le retrait abandonné** après deux essais réels (14.7, #29) : le compte reste |
| 3 | Raccourcir le délai entre le lien et l'usage d'un service | **fait pour ce qui dépend du moteur** (14.3) ; le reste est dans BlueZ et l'iPhone |
| 4 | Preuve côté iPhone (console, « strike ») | protocole écrit (essai P3) ; demande un Mac |
| 5 | Sortir du blocage sans l'interrupteur Bluetooth | non fait ; l'essai R5 reste le protocole |
| 6 | ANCS par le lien classique comme repli | non fait |
| 7 | Intervalle de repos à 150–180 ms | compris dans le n° 1 (valeurs par défaut de la demande) |
| 8 | Alléger la retenue de 45 s avant le classique | non fait : attend que le n° 1 soit validé |
| 9 | Vérifications de conformité au `btmon` (sniff, `AT+XAPL`, annonce étendue) | non fait |
| 10 | Corriger nos documents | fait ici (13.2, 13.3, 13.4, 13.5) et dans `essai-reconnexion.md` (R4) ; l'étude 02 n'a pas été touchée |
| 11 | Annonce : ne rien changer au régime normal | respecté ; après une faute, 20 ms 30 s puis 152,5 ms (14.7) ; l'intervalle lent ne sert plus qu'aux liens servis qui tombent en boucle (15.8) |

### 14.2 La règle des fautes (`le_strikes.hpp`, `LeStrikes`)

**Ce qui n'allait pas.** La mesure (d) de 13.4 retirait l'annonce après 3 coupures LE en 60 s : à la
troisième coupure, la troisième faute est déjà comptée par l'iPhone. Et elle comptait les coupures, alors
qu'iOS compte les liens **morts sans avoir servi** : un lien abonné qui tombe n'est pas une faute.

> **Depuis le 18 novembre au soir (#29), l'annonce n'est plus retirée** : voir 14.7. Le tableau ci-dessous
> décrit la règle d'origine, que les variables d'environnement rétablissent ; le compte, lui, est inchangé.

**La règle.** Une classe pure, alimentée une fois par passe (`watch_le_strikes()`, dans l'étape `links()`,
donc **avant** que le client ANCS apprenne la coupure : son état décrit encore le lien qui vient de finir).

| Fait | Décision |
|---|---|
| Un lien LE se termine alors que le service de notifications **a répondu** dessus (`ancs_ready`, c'est-à-dire une réponse de l'iPhone sur la *Data Source*) | pas une faute |
| Un lien LE se termine **avant** | faute. Plusieurs signaux `Disconnected` dans la même passe : le premier lien est jugé sur l'état du service, les autres, nés et morts entre deux passes, sont des fautes |
| 1re faute | annonce **retirée tout de suite, 15 s** (`supervise_ancs_solicitation(false)` dans la passe même), puis remise à son rythme normal |
| 2e faute sans lien servi entre-temps | retirée **30 s** (120 s avant le 18 novembre), puis remise à **1022,5 ms** (le plus long intervalle de la liste d'Apple) tant qu'aucun lien n'a servi |
| 3e et suivantes | retirée 60 s, puis 120 s (plafond) ; avant le 18 novembre 240, 480, 600 s — l'iPhone s'est relié 1 s après une pause de 4 min (`journal-essais.md`) |
| Notifications actives **10 s de suite** sur un lien | compte remis à zéro, annonce de nouveau normale |
| Lien servi moins de 10 s puis perdu | ni faute, ni remise à zéro |
| 15 min sans nouvelle faute, hors retrait | compte oublié (l'iPhone est parti ; son retour repart à 20 ms) |
| Réveil de l'ordinateur, retour de bluetoothd | compte remis à zéro (ces coupures ne sont pas celles de l'iPhone) |

Pourquoi « le service a répondu » et non « l'abonnement est accepté » : `StartNotify` réussit et `Notifying`
lit vrai sur un lien déjà mort (BlueZ ne remet pas `Notifying` à faux à la coupure ; le journal du 17 montre
des « notification session N started » 10 ms **après** un `Disconnected`). Seule une réponse de l'iPhone
prouve que le lien a servi. Ce critère est plus sévère que celui d'iOS (qui a vu l'écriture de
l'abonnement avant de répondre) : une faute comptée ici peut ne pas en être une pour l'iPhone, jamais
l'inverse. Le coût d'une faute comptée à tort est un retrait de 15 s.

Garde : le compte n'est tenu que si les notifications sont voulues, l'appairage sain, et si l'iPhone a déjà
offert ANCS sur cet appairage (`Device1.UUIDs`). Un appairage en cours, qui coupe et rouvre le lien, n'est
pas concerné ; pendant qu'il tient l'annonce, le retrait ne s'applique pas non plus.

**Ce qui est publié.** Depuis #29, sans retrait : `unsteady` (état `waiting`) tant que le compte n'est pas à
zéro et que le lien LE est perdu depuis moins de 180 s (14.7). Avec les retraits rétablis :
`le_cause:"holding_off"` (état `waiting`) pendant le retrait, avec une phrase dans
`le_detail` ; cette cause passe avant toutes les autres, y compris au-delà de 180 s : le silence est alors
le nôtre, pas celui de l'iPhone. Ensuite `unsteady` tant que le compte n'est pas à zéro. `le_strikes` donne
le compte. Après deux fautes, une perte durable est attribuée au retrait de l'iPhone (`phone_backoff` s'il
est joint par le classique, sinon `phone_away` avec la phrase à deux possibilités), comme après une rafale.

**Sur le journal du 17 novembre.** Arrivée de 18:04 : une vingtaine de liens en 80 s. Avec la règle (test
`LeStrikes.TheArrivalOfTheSeventeenthWithThePausesOfBefore`) : quatre liens offerts dans les deux premières
minutes. Depuis #29 l'iPhone se voit offrir un lien dès qu'il en veut : ce sont les essais du 18 qui ont
montré qu'il ne s'en lasse pas.

**Choix à connaître.**
- Un lien **servi** qui tombe ne retire pas l'annonce : le lien suivant est donc la première occasion de
  faute. L'étude ne demande pas plus ; un retrait bref après toute coupure retarderait de plusieurs secondes
  les reprises banales (dont les coupures de 720 ms).
- Le doublement au-delà de la deuxième faute et l'oubli après 15 min sont des ajouts ; l'étude dit seulement
  « 2 min, puis reprise lente ».
- Coût (règle d'origine) : un iPhone posé en limite de portée attendait ses notifications jusqu'à 2 min
  après être revenu à portée franche, tant qu'aucun lien n'avait servi 10 s. C'est ce coût, mesuré le
  18 novembre, qui a fait abandonner le retrait (14.7).
- L'ancienne règle des rafales reste dans le code (`LeHealth`) : elle journalise encore (`dropped 3 times
  within 60s`) mais ne retire plus l'annonce, sauf avec `LANDLINE_LE_STRIKES=0`.

### 14.3 Du lien à l'usage : ce qui retardait, ce qui est raccourci

**Le « 3,4 s » du 17 novembre.** Le journal ne datait pas la montée du lien LE : les 3,4 s (18:05:45,5 →
18:05:48,9) séparent « notification session 25 started » de « Notification mirroring is active », donc
l'abonnement **déjà accepté** de la première réponse de l'iPhone. Lu dans le code, ce délai était presque
entièrement le nôtre :

| Cause (lue) | Coût | Maintenant |
|---|---|---|
| La première question (`GetAppAttributes`) était mise en file dans la passe de l'abonnement mais **écrite à la passe suivante** | 1 s | écrite dans la même passe |
| La réponse, reçue sur le fil GLib, n'était **lue qu'à la passe suivante** | jusqu'à 1 s | un paquet ANCS déclenche une passe (au plus une toutes les 100 ms) |
| Après une reconstruction de session, la date de la question n'était pas remise à zéro | jusqu'à 5 s (18:05:31,0 → 33,1) | remise à zéro |
| Les demandes d'attributs des notifications que l'iPhone liste à l'abonnement passaient **avant** la question, une par passe, et leur réponse ne valait pas preuve | 1 s par notification en attente | une par aller-retour radio ; toute réponse de la *Data Source* vaut preuve |
| La montée du lien n'était vue qu'après l'anti-rebond de l'instantané (250 ms) puis à la passe suivante | 0,25 à 1,25 s | `Bearer.LE1.Connected` rafraîchit l'instantané et déclenche une passe tout de suite |
| Les dates d'essai (découverte, abonnement) avaient grandi pendant l'absence : 5 s après 5 min | jusqu'à 5 s | remises à zéro à chaque nouveau lien |
| La chute du lien n'était vue qu'à la passe suivante : l'annonce, encore enregistrée, laissait l'iPhone se reconnecter entre-temps | jusqu'à 1 s, un lien de plus | le signal `Disconnected` déclenche une passe |

Rien de cela n'ajoute d'appel à BlueZ : aucune séquence `StopNotify` / `StartNotify`, les règles de
`subscription_guard.hpp` (§10.2) sont intactes. Le délai de 4 s de `StartNotify` est un **délai d'attente
maximal** d'un appel, pas un retard : il n'est pas modifié. Retour arrière : `LANDLINE_LE_PROMPT=0`.

**Ce que l'iPhone voit, lui, ne dépendait déjà presque pas de nous** (lu dans BlueZ 5.87) :
- à l'attachement du canal ATT, quand le cache GATT existe, `gatt_client_init()` appelle
  `device_accept_gatt_profiles()` — le greffon Batterie **lit le niveau de batterie** puis s'y abonne — puis
  `btd_gatt_client_connected()`, qui **réécrit l'abonnement de chaque abonné gardé** (`register_notify`),
  sans attendre `ServicesResolved` ni notre fil ;
- cela vaut tant que BlueZ tient encore notre abonné. Il le retire si l'iPhone refuse l'écriture
  (`register_notify_cb` avec une erreur) ou s'il a redémarré : c'est alors notre `StartNotify` qui abonne,
  et c'est là que la seconde gagnée compte pour l'iPhone.

**Lire un service intégré plus tôt ?** Non : BlueZ lit déjà la batterie au tout premier instant possible, et
une lecture par D-Bus exigerait les mêmes préalables qu'un abonnement (client GATT prêt, lien chiffré) ; le
service Batterie, pris par le greffon, n'est d'ailleurs pas exporté sur D-Bus. **Qu'une lecture compte comme
« usage » pour iOS n'est pas établi** : si elle comptait, la lecture de BlueZ nous protégerait déjà à chaque
lien. C'est la console de l'iPhone qui le dira (essai P3 : un lien coupé juste après la lecture de la
batterie est-il compté ?).

**Mesure.** Trois lignes nouvelles au journal datent chaque lien :
`org.bluez.Bearer.LE1 connected to …`, `notification subscription confirmed Nms after the LE link came up`,
`notification service in use Nms after the LE link came up, Nms after the subscription`. Ce qui reste dans
ce temps : le chiffrement du lien, la relecture de l'arbre par BlueZ, la réponse de l'iPhone.

### 14.4 Paramètres de connexion LE (`le_conn_params.hpp`, `le_conn_update.cpp`)

> **Remplacé par la section 15** (essai réel du 17 novembre 2026, intégration) : la demande est désormais
> **active par défaut** dès que l'auxiliaire est en place, avec **45–60 ms, latence 0, 6 s**, faite **dès que
> le lien est chiffré** et à **chaque** nouveau lien ; relances de Pebble (collisions 2 s × 10, 3 essais à
> 5 s) ; l'auxiliaire est posé par `landline-setup apply`. Ce qui suit est la conception d'avant l'essai,
> gardée pour l'inventaire des voies sans privilège et les risques de la voie B.

**Ce que faisait le moteur avant l'essai.** Sur chaque lien LE, une fois les notifications actives et au
moins 5 s après la montée du lien (`TGAP(conn_pause_peripheral)`), il envoie la commande HCI *LE Connection
Update* : **150–180 ms, latence 0, supervision 6 s** (valeurs de Pebble ; 6 s est la seule valeur admise à
la fois par la règle de 2017 et par celle de 2026). Les valeurs réglées sont vérifiées contre les règles
d'Apple (§57.6) avant tout envoi ; un jeu refusé par ces règles n'est jamais envoyé. Le résultat est lu sur
le même socket (*Command Status*, puis *LE Connection Update Complete*, 8 s au plus), dans un fil à part.
Trois demandes au plus par lien, à 15 s d'intervalle, jamais deux à la fois ; une réponse dont le délai de
supervision est plus court que celui demandé est redemandée. **Jamais de coupure du lien sur un refus.**
Publié : `le_conn_params` (`off`, `no_right`, `waiting`, `asking`, `applied`, `refused`).

**Voies sans privilège : aucune.** Vérifié une par une :

| Voie | Verdict | Source |
|---|---|---|
| Propriété ou méthode D-Bus de BlueZ 5.87 | aucune ne porte intervalle, latence ou délai de supervision | pages de manuel `org.bluez.Device`, `Bearer.LE`, `Adapter` installées |
| Socket L2CAP sur le canal de signalisation LE (CID 5) | refusé : en LE, seul le CID de l'ATT est permis à un socket (« *We only allow ATT user space socket* ») ; un socket L2CAP brut exige `CAP_NET_RAW` et parlerait sur le CID 1 du classique | `l2cap_sock.c`, `l2cap_sock_bind`, `l2cap_sock_create` |
| `setsockopt` sur un socket L2CAP ATT | options existantes : `BT_SECURITY`, `BT_DEFER_SETUP`, `BT_FLUSHABLE`, `BT_POWER`, `BT_CHANNEL_POLICY`, `BT_SNDMTU`, `BT_RCVMTU`, `BT_PHY`, `BT_MODE` — aucune ne touche aux paramètres de connexion | `l2cap_sock.c`, `l2cap_sock_setsockopt` |
| API de gestion (mgmt) sans privilège | un socket non privilégié n'a droit qu'aux lectures (`READ_INDEX_LIST`, `READ_INFO`…) | `mgmt.c`, `mgmt_untrusted_commands[]` |
| API de gestion **avec** `CAP_NET_ADMIN` (`Load Connection Parameters`) | ne met à jour un lien existant que si l'ordinateur en est le **central** (`conn->role == HCI_ROLE_MASTER`) ; nous sommes périphérique | `mgmt.c`, `load_conn_param`, `conn_update_sync` |
| `main.conf` `[LE]` | ne porte pas le délai de supervision d'un lien entrant (13.2) | étude 12 §4.3 |
| Commande HCI *LE Connection Update* sur un socket HCI brut | **marche, avec `CAP_NET_RAW`** : toute commande d'un groupe au-delà de 5 (le LE est le groupe 8) est refusée par `EPERM` sans cette capacité. `CAP_NET_ADMIN` seul n'y suffit pas | `hci_sock.c`, `hci_sock_sendmsg`, `hci_sec_filter` |

Lire la liste des liens du contrôleur (pour trouver le *handle*) ne demande aucun droit (`HCIGETCONNLIST`,
comme `hcitool con`). L'étude 12 parlait de `CAP_NET_ADMIN` : c'est `CAP_NET_RAW`.

**Sans droit.** Le moteur le constate au démarrage (capacités du processus, présence de l'auxiliaire), écrit
**une** ligne `[INFO]` (`LE connection parameters are wanted … but this process may not ask for them`),
publie `le_conn_params:"no_right"` et ne tente rien. Ce n'est pas une erreur.

**Ce que l'assistant système (`landline-setup`) devrait accorder.** Deux façons ; la première est
recommandée.

*A. Un auxiliaire à capacité (recommandé).* `landline-le-params`, compilé avec le moteur
(`engine/build/local/landline-le-params`, 360 ko, lié à la seule libc, sans RUNPATH) : il prend un
contrôleur, une adresse et quatre valeurs, refuse tout jeu hors des règles d'Apple, envoie cette seule
commande sur un lien LE qui existe déjà, n'en coupe jamais. Le démon le lance à chaque demande.

```sh
sudo install -d -o root -g root -m 0755 /usr/local/libexec/landline
sudo install -o root -g root -m 0755 landline-le-params /usr/local/libexec/landline/
sudo setcap cap_net_raw+ep /usr/local/libexec/landline/landline-le-params
getcap /usr/local/libexec/landline/landline-le-params        # cap_net_raw=ep
```

puis, dans l'unité utilisateur du démon, `Environment=LANDLINE_LE_CONN_PARAMS=1`. Retrait :
`sudo rm /usr/local/libexec/landline/landline-le-params`. Risques : tout processus local peut lancer
l'auxiliaire, donc changer les paramètres d'un lien LE existant **dans les bornes d'Apple** (intervalle
15 ms à 2 s, supervision 6 à 18 s) — nuisance faible, et réductible en donnant le fichier à un groupe
(`-m 0750 -g <groupe>`). Un défaut dans l'auxiliaire donnerait `CAP_NET_RAW` à qui l'exploite : ses
arguments sont des nombres de six chiffres au plus et une adresse de 17 caractères, sa sortie est une ligne.
Le système de fichiers ne doit pas être monté `nosuid` (les capacités de fichier y sont ignorées).
La capacité est à reposer après chaque mise à jour du fichier (toute écriture l'efface).

*B. La capacité sur le démon lui-même.* `sudo setcap cap_net_raw+p <chemin>/landlined` (« permise »
suffit : le démon ne la lève que dans le fil éphémère qui écrit la commande). Plus simple, **déconseillé** :
- un binaire à capacité tourne en « mode sécurisé » (`AT_SECURE`) : la glibc ignore `LD_LIBRARY_PATH` et
  consorts, GLib se met en mode privilégié (certaines variables d'environnement ne sont plus lues, dont
  l'adresse du bus de session, retrouvée alors par `XDG_RUNTIME_DIR`) — **effets non essayés** sur un démon
  qui dépend du bus de session (obexd, notifications) ;
- le processus n'est plus « dumpable » : plus de vidage mémoire, plus de `gdb` par l'utilisateur ;
- `CAP_NET_RAW` vaut pour tout le démon : sockets bruts de toute famille (écoute du trafic réseau, paquets
  forgés) et **toute** commande HCI, y compris celles du fabricant. Tout code injecté dans le démon — il
  charge des bibliothèques par un RUNPATH qui, dans la compilation locale, pointe vers le dossier personnel
  — en hériterait ;
- la capacité saute à chaque recompilation ou mise à jour du binaire : mot de passe à chaque fois ; et elle
  est ignorée si `/home` est monté `nosuid`.

*C. `AmbientCapabilities=CAP_NET_RAW` dans l'unité : impossible ici.* Le démon est une unité **utilisateur**.
Le gestionnaire `systemd --user` tourne sans aucune capacité et ne peut en donner aucune : une capacité
ambiante doit déjà être permise et héritable chez celui qui lance le processus. La directive n'a d'effet que
dans une unité **système** (avec `User=`). Faire du démon une unité système le sortirait de la session de
l'utilisateur (bus de session, obexd, PipeWire, `XDG_RUNTIME_DIR`), ce sur quoi tout le reste repose : à
exclure pour un seul appel HCI.

**Risques de la demande elle-même.**
- Une procédure de couche liaison à laquelle l'iPhone ne répond pas ferme le lien au bout de 40 s (délai de
  réponse de la spécification) : d'où l'essai manuel d'abord, et trois demandes au plus par lien.
- iOS peut répondre par d'autres valeurs, ou refuser. Un iPhone qui ne connaît pas la procédure de couche
  liaison renverrait « fonction non prise en charge » (0x1a) ; la demande L2CAP de repli n'est accessible
  qu'au noyau.
- Avec 6 s de supervision, **le départ de l'iPhone est vu 6 s plus tard** (présence, verrouillage à
  l'éloignement) au lieu de 0,7 s.
- À 150–180 ms, chaque question ANCS et sa réponse prennent 0,3 à 0,5 s au lieu de 0,1 s : la liste des
  notifications présentes au retour arrive plus lentement. Pebble redescend à 15 ms le temps d'un
  transfert ; le moteur ne le fait pas. `LANDLINE_LE_CONN_INTERVAL_MS=30-45` garde le débit d'aujourd'hui
  avec 6 s de supervision.
- Si iOS propose ensuite lui-même d'autres paramètres, le noyau refuse tout intervalle maximal au-dessus de
  celui de l'adaptateur (50 ms par défaut) : à chercher dans une capture (*LE Remote Connection Parameter
  Request* suivi d'une réponse négative).
- L'arrêt du démon attend la fin d'une demande en cours (8 s au plus).

### 14.5 Ce qu'on lit dans le journal

| Ligne | Sens |
|---|---|
| `bluetooth: org.bluez.Bearer.LE1 connected to …` | montée du lien LE, datée à la réception du signal |
| `bluetooth: notification subscription confirmed Nms after the LE link came up` | abonnement confirmé par BlueZ |
| `bluetooth: notification service in use Nms after the LE link came up, Nms after the subscription` | première réponse de l'iPhone : à partir de là, une coupure n'est plus une faute |
| `[WARN] bluetooth: the LE link ended before the notification service had answered (unused links in a row: N); the advertisement stays on air, fast for 30s then at 152.5 ms, and the iPhone is called again` | une faute (défaut depuis #29) : l'annonce reste à l'antenne |
| `[WARN] bluetooth: the LE link ended … ; the advertisement is taken off air for Ns…` | une faute et un retrait, seulement si les retraits sont rétablis par l'environnement ; `, then put back at its slow interval` avec `LANDLINE_LE_STRIKE_SLOW` |
| `[WARN] bluetooth: N LE links in a row ended before the notification service had answered, none served in between; the advertisement stays on air at 152.5 ms, with no fast phase, until a link serves (LANDLINE_LE_STRIKE_BRAKE=N)` | frein (#41, §14.7) : une fois par série, plus de phase rapide jusqu'à un lien servi |
| `bluetooth: the notification service has been live on the LE link for 10s; the N unused link(s) before it are forgotten` | compte remis à zéro |
| `bluetooth: no unused LE link for 900s; the N counted before that are forgotten` | oubli après 15 min sans nouvelle faute |
| `bluetooth: notification link waiting: unsteady` | liens qui échouent, nouvel essai en cours (défaut) |
| `bluetooth: notification link waiting: holding_off` | état publié pendant un retrait (retraits rétablis seulement) |
| `bluetooth: soliciting ANCS for 180s, advertising every 20-20 ms` puis `every 152-153 ms` | annonce après une faute : rapide 30 s, puis 152,5 ms |
| `bluetooth: soliciting ANCS for 180s, advertising every 1022-1023 ms` | annonce au ralenti : liens servis qui tombent en boucle (15.8), ou `LANDLINE_LE_STRIKE_SLOW` |
| `bluetooth: LE connection parameters are wanted … but this process may not ask for them…` | demande activée, pas de droit : une fois, au démarrage |
| `bluetooth: asking the iPhone for LE connection parameters 150-180 ms, latency 0, supervision 6000 ms (attempt 1 of 3, by helper)` | demande envoyée (`by capability` si le démon a lui-même le droit) |
| `bluetooth: LE connection parameters now in force: interval 165 ms, latency 0, supervision 6000 ms (handle N)` | ce que l'iPhone a appliqué |
| `[WARN] bluetooth: LE connection parameters not obtained (refused, HCI status 59…), attempt N of 3` | refus (59 = 0x3b, paramètres inacceptables ; 26 = 0x1a, procédure non prise en charge ; 12 = 0x0c, commande refusée par le contrôleur) |

### 14.6 Ce qui reste hypothétique sans iPhone

1. Que la règle des « trois fautes » existe encore sous iOS 26, avec quel seuil et quelle fenêtre (P3).
   **Deux essais réels (18 novembre) disent que non, du moins pas à trois** : l'iPhone est revenu après
   trois liens morts à la suite, les deux fois (14.7).
2. ~~Que retirer l'annonce 15 s puis 30 s suffise à ne pas atteindre la troisième faute~~ : sans objet
   depuis #29, l'annonce n'est plus retirée.
3. Qu'une réponse sur la *Data Source* soit bien postérieure à ce qu'iOS tient pour « usage » (sinon des
   fautes nous échappent) ; qu'une lecture de la batterie compte ou non.
4. Que `Bearer.LE1` émette `PropertiesChanged` pour `Connected` à chaque montée de lien sur cette machine (lu
   dans `src/bearer.c`, pas vu au journal) : sans lui, pas de ligne `connected to`, pas de passe immédiate,
   et le comportement est celui d'avant.
5. Les délais réels après ces changements (les trois lignes de 14.5 les donneront).
6. Tout 14.4 : que le contrôleur Intel 9560 accepte la commande en rôle périphérique, que l'iPhone accepte
   150–180 ms / 6 s, que les coupures cessent, que la recherche en classique ne tue plus le LE (P1).
7. Que le noyau liste le lien LE sous l'adresse d'identité de l'iPhone (sinon l'unique lien LE est pris).
8. Le comportement d'un démon portant une capacité de fichier (voie B) : non essayé, déconseillé.

### 14.7 L'annonce reste à l'antenne après un lien inutilisé (18 novembre, #29)

**Ce qu'ont montré les essais.** Deux allers-retours à pied (#1). Le second, au retour : premier contact
16:17:32, entrée dans l'appartement **16:18:00**, notifications **16:20:58**. Trois liens LE morts avant
service, en 1,4 s, 0,7 s et 0,7 s (16:17:32, 16:17:50, puis **16:19:43, à l'intérieur**), avant même qu'une
supervision de 6 s ait pu être négociée. Avec la règle d'origine, l'annonce a été retirée 15 s, 30 s, puis
60 s, et remise à 1022,5 ms à partir de la deuxième faute. Décomposé après l'arrivée : **~80 s** avant que
l'iPhone réponde à l'annonce lente, puis **60 s** hors antenne après la troisième faute, puis le lien de
16:20:56 qui a tenu. Les deux fois, l'iPhone est revenu **après trois fautes à la suite** : la règle
d'iOS de 2015 ne s'applique pas, ou pas à ce seuil. Les deux attentes étaient les nôtres.

**Ce qui change (défaut).**

| Fait | Avant | Depuis #29 |
|---|---|---|
| un lien LE meurt avant service | annonce retirée 15 s (1re), 30 s doublé jusqu'à 120 s (suivantes) | **annonce laissée à l'antenne** |
| intervalle après une faute | 1re : rythme d'une perte ordinaire (20 ms 180 s) ; dès la 2e : 1022,5 ms | **20 ms pendant 30 s** après chaque faute, **puis 152,5 ms** tant qu'aucun lien n'a servi |
| liens servis qui tombent en boucle (Pebble, 15.8) | 1022,5 ms une minute | inchangé |
| un lien sert (même quelques secondes, sans effacer le compte) puis tombe après une faute | — | **perte ordinaire** : 20 ms pendant 180 s depuis la perte, puis 152,5 ms (#40) ; les fautes restent comptées |
| 8 fautes à la suite ou plus, aucun lien servi entre elles | — | **frein** : plus de phase rapide, 152,5 ms, une ligne de journal (#41) |
| compte des fautes, `le_strikes`, journal, `phone_backoff` après deux fautes | — | inchangés |
| état publié | `holding_off` pendant le retrait, puis `unsteady` | `unsteady` tout de suite (pas de retrait) |

**Pourquoi ces intervalles.** Apple (*Accessory Design Guidelines*, annonce LE) demande 20 ms pendant au
moins les 30 premières secondes, puis l'un des intervalles 152,5 / 211,25 / 318,75 / 417,5 / 546,25 / 760 /
852,5 / 1022,5 / 1285 ms. L'iPhone verrouillé n'écoute que par courtes fenêtres : plus l'intervalle est
long, plus il en rate à la suite. Le 1022,5 ms a coûté ~80 s le 18 ; on garde donc le plus court de la
liste, 152,5 ms, qui est déjà le régime de fond du moteur (environ 2 ms de radio toutes les 152,5 ms).
La phase rapide est **de 30 s** (le minimum d'Apple), relancée à chaque faute, et non les 180 s qui suivent
une perte ordinaire : en limite de portée, 20 ms pendant trois minutes offrirait à l'iPhone un lien voué à
mourir toutes les une ou deux secondes, sans rien gagner sur un iPhone déjà entré (30 s couvrent le pas de
la porte). Les fenêtres rapides « tous liens tombés » et « réveil » ne sont pas données pendant les fautes
(chaque faute est une telle perte, elles maintiendraient les 20 ms) ; celle « classique joint, LE absent »
l'est (`advert_interval_with_strikes()`, `le_strikes.hpp`). Un réveil ou un retour de bluetoothd remettent
le compte à zéro : le rythme ordinaire reprend.

**Ce que le 18 aurait donné.** Test `LeStrikesOnAir.TheReturnHomeOfTheEighteenth` : les trois liens morts
rejoués à la seconde ; l'annonce est à l'antenne à chaque passe, jamais à 1022,5 ms, à 20 ms de 16:17:34 à
16:18:20 et de 16:19:44 à 16:20:13, à 152,5 ms entre les deux ; l'état publié est `waiting` / `unsteady`
sur toute la durée ; le lien qui tient remet le compte à zéro 10 s après la première notification. Combien
de secondes cela fait gagner en vrai : à mesurer au prochain retour (#1). Attendu : le lien de 16:19:43
dès l'entrée (plus d'annonce lente), et pas de minute hors antenne après lui.

**Un lien qui a servi rend le rythme ordinaire (#40).** Un lien sur lequel l'iPhone a répondu, ne
serait-ce qu'un instant, puis qui tombe avant les 10 s qui effacent le compte, n'est pas une faute : c'est
une perte de lien servi. Avant la correction, les fautes d'avant gardaient la main sur le rythme pendant
15 min et l'annonce passait directement à 152,5 ms (sans `le_down_since`). Désormais, dès qu'un lien a
servi après la dernière faute, `sets_rhythm()` est faux : la règle ordinaire reprend (20 ms pendant 180 s
après la perte, puis 152,5 ms). Le compte reste pour le journal, `le_strikes` et `le_state` (`unsteady`),
jusqu'à ce qu'un lien serve 10 s ou que 15 min passent sans faute ; une nouvelle faute rend la main aux
fautes. Test `LeStrikesOnAir.ALinkServedBrieflyAfterAStrikeIsAnOrdinaryLoss` : faute, lien servi 5 s,
perte → 20 ms.

**Un frein sur une boucle sans fin (#41).** Sans retrait ni ralenti, un iPhone qui se relierait et
tomberait indéfiniment sans jamais répondre recevrait 30 s à 20 ms après chaque faute, pour toujours (l'oubli
de 15 min n'arrive jamais tant que les fautes continuent, et le ralenti « Pebble » ne compte que les liens
servis). À partir de **8 fautes à la suite sans aucun lien servi entre elles** (3 lors des retours réels),
l'annonce reste à l'antenne à **152,5 ms sans aucune phase rapide** (ni celle des fautes, ni « classique
joint, LE absent »), et le journal le dit une fois :
`[WARN] bluetooth: 8 LE links in a row ended before the notification service had answered, none served in
between; the advertisement stays on air at 152.5 ms, with no fast phase, until a link serves
(LANDLINE_LE_STRIKE_BRAKE=8)`. Un lien qui sert, même brièvement, lève le frein ; un réveil ou un retour de
bluetoothd aussi (remise à zéro), de même que 15 min sans nouvelle faute. Réglage : `LANDLINE_LE_STRIKE_BRAKE=<n>` (0 à 99, 0 : pas de frein ;
`moteur-api.md`). Tests `LeStrikesOnAir.EightUnusedLinksInARowAndTheFastPhaseStops`,
`TheBrakeCanBeMovedOrTurnedOff`.

**Revenir en arrière.** `LANDLINE_LE_STRIKE_PAUSE_SECONDS=15 LANDLINE_LE_STRIKE_LONG_PAUSE_SECONDS=30
LANDLINE_LE_STRIKE_SLOW=2 LANDLINE_LE_STRIKE_FAST_SECONDS=0` rétablit exactement la règle d'origine
(`moteur-api.md`, variables). Chacune se règle seule : par exemple `LANDLINE_LE_STRIKE_FAST_SECONDS=0` seul
garde l'annonce à l'antenne au rythme d'une perte ordinaire.

**Ce qui reste à vérifier.** Qu'aucun seuil d'iOS ne se manifeste avec plus de liens morts à la suite
(l'annonce laissée à l'antenne en offre davantage en limite de portée qu'avant) : si l'iPhone cessait de
revenir après N fautes, le journal le dirait (`unused links in a row: N` suivi d'un silence), et l'état
passerait à `paused` / `phone_backoff` ou `phone_away` 180 s après la dernière.

## 15. Paramètres de connexion LE : l'essai réel du 17 novembre et l'intégration (branche `tolerance`)

### 15.1 L'essai

Contrôleur Intel 9560 en périphérique, BlueZ 5.87, noyau 7.3, iPhone sous iOS 26. Script
`essais/lecup-suivi.sh` (root, hors dépôt) : il lit `hcitool con` toutes les 200 ms et, une seconde après
chaque nouveau lien LE de l'iPhone, envoie `hcitool lecup <handle> 0x0024 0x0030 0x0000 0x0258`
(45–60 ms, latence 0, supervision 6000 ms). Relevés `essais/lecup-auto.txt.btmon` (20:40) et
`essais/lecup-suivi.txt.btmon` (20:55–21:25), `btmon` en route.

| Constat | Où |
|---|---|
| La puce prend la commande en rôle périphérique (*Command Status* 0x00) et lance la procédure de couche liaison | `lecup-auto` 20:40:14, `lecup-suivi` #59, #111 |
| L'iPhone applique **exactement** la demande : *LE Connection Update Complete* 60 ms, latence 0, 6000 ms | `lecup-suivi` lignes 26–31, 2546–2551 |
| Avant : liens morts en 1 à 2 s (5 coupures en 5 min, *Connection Timeout* 0x08). Après : un lien tenu près d'une heure sans coupure | relevé de l'utilisateur, 20:58 → 21:53 |
| Un lien est tombé **1,05 s** après sa montée (145,436 → 146,469), avant que la demande, partie à +1,2 s, ne s'applique : *Unknown Connection Identifier* (0x02) | `lecup-suivi` 1711–1917 |
| À une reconnexion, l'iPhone renégocie lui-même **avant** le chiffrement : *LE Remote Connection Parameter Request* puis *Update Complete* **45 ms / 720 ms** ; notre demande juste après impose 6 s, sur le **même handle** 3585 | `lecup-suivi` 2370–2394, 2447–2551 |
| Chiffrement 0,92 s après la montée du lien (161,668 → 162,590) | `lecup-suivi` 2167, 2408 |

Deux règles en sortent : **demander le plus tôt possible** ; **redemander à chaque nouveau lien**, même s'il
reprend le même handle.

### 15.2 Ce que fait le moteur

`le_conn_params.hpp` (décisions), `le_conn_update.{hpp,cpp}` (système), `src/le_params/main.cpp`
(auxiliaire).

- **Activé par défaut** quand l'auxiliaire *opérationnel* est là (`/usr/local/libexec/landline/landline-le-params`,
  à root, non inscriptible par d'autres, portant une capacité de fichier) ou que le démon a lui-même
  `CAP_NET_RAW`. Sinon : une ligne `[INFO]` au démarrage, `le_conn_params:"no_right"`, rien d'autre — le
  comportement d'avant. `LANDLINE_LE_CONN_PARAMS=0` éteint tout (`off`).
- **Valeurs** : 45–60 ms (0x24–0x30), latence 0, supervision 6 s — celles de l'essai, celles de Pebble
  (octobre 2026) sans la latence (§14.1, étude 13 §3.3). Réglables (`LANDLINE_LE_CONN_INTERVAL_MS`,
  `…_LATENCY`, `…_TIMEOUT_MS`), toujours vérifiées contre les règles d'Apple ; un jeu refusé n'est pas
  envoyé, les valeurs par défaut le sont.
- **Moment** : dans la passe qui voit le lien monter (réveillée par le signal `Bearer.LE1`), sans attendre
  ANCS ni les 5 s de TGAP. Un fil à part regarde la liste des liens du contrôleur toutes les **50 ms**
  jusqu'à ce que le lien de l'iPhone y soit **et soit chiffré** (5 s au plus), puis fait la demande.
  `LANDLINE_LE_CONN_ENCRYPTED=0` n'attend pas le chiffrement (voir 15.5).
- **À chaque nouveau lien** : un lien est « nouveau » quand le compte des montées vues par le moniteur
  change, quel que soit son handle. Ce qui avait été obtenu part avec l'ancien.
- **Vérification** : l'auxiliaire attend *Command Status* puis *LE Connection Update Complete* (8 s au
  plus) ; une supervision accordée plus courte que celle demandée compte comme un refus.
- **Relances** (Pebble, `gap_le_connect_params.c`) : statut **0x23** (*LMP/LL Error Transaction Collision*)
  ou **0x2A** (*Different Transaction Collision*) → nouvel essai **2 s** plus tard, sans compter d'essai,
  **10 fois** au plus ; tout autre refus, l'absence de réponse ou de lien → **3 essais à 5 s** d'intervalle,
  puis abandon **sans couper le lien** (`refused`).
- **Surveillance** : après sa réponse, l'auxiliaire reste à l'écoute du lien (6 h au plus, lecture seule)
  et rapporte chaque *LE Connection Update Complete* et la fin du lien. Si l'iPhone renégocie une
  supervision **< 6 s** sur le même lien, la demande est refaite, **au plus une fois par minute**, avec un
  nouveau compte d'essais. Au bout des 6 h, une nouvelle demande (qui relance la surveillance).
- **État publié** : `le_conn_params` = `applied` / `pending` / `refused` / `no_right` (et `waiting` sans
  lien LE, `off` éteint) ; `le_conn_values` = `{interval_ms, latency, timeout_ms}` en vigueur sur le lien,
  ou `null`.

### 15.3 Comment le handle est trouvé

Sans privilège : `HCIGETCONNLIST` sur un socket HCI non lié (ce que lit `hcitool con`) rend, pour chaque
lien du contrôleur, handle, adresse, type et mode (dont `HCI_LM_ENCRYPT`). Le moteur s'en sert pour attendre
le chiffrement ; **l'auxiliaire refait lui-même la recherche** à partir de l'**adresse** de l'iPhone et ne
reçoit jamais de handle. Le noyau liste un iPhone appairé sous son **adresse d'identité** (l'adresse privée
est résolue dès `LE Connection Complete`, `hci_event.c`) ; aucun autre lien n'est pris pour le sien (le repli
« un seul lien LE » d'avant a été retiré). Pas de lien LE à cette adresse : `no_link`, rien n'est envoyé.

### 15.4 L'auxiliaire et son droit

Voir `systeme.md` §2 (« Tolérance du lien LE »). En bref : `landline-setup apply` le copie à root dans
`/usr/local/libexec/landline/` et pose `cap_net_raw+ep` ; il ne fait qu'envoyer *LE Connection Update* sur
un lien LE existant, avec des valeurs bornées aux règles d'Apple, ne lit ni entrée ni fichier ni variable
d'environnement, et rend 0 (demande faite), 1 (pas faite) ou 2 (mauvais usage). Le démon le lance avec un
environnement vide et l'entrée sur `/dev/null`, une fois par lien (et par relance).

### 15.5 Ce qui reste à vérifier en réel

1. **Avant ou après le chiffrement ?** Le lien tombé à 1,05 s n'était pas chiffré ; attendre le chiffrement
   (0,92 s observé) laisse ~0,1 s. Si des liens meurent encore entre la montée et le chiffrement, essayer
   `LANDLINE_LE_CONN_ENCRYPTED=0` (demande dès que le handle est connu) — la procédure peut alors croiser
   la renégociation de l'iPhone (+0,22 s) ou le démarrage du chiffrement : ce sont les collisions 0x23/0x2A,
   relancées à 2 s.
2. Le délai réel montée → *Update Complete* avec le moteur (ligne `asking … once the link is encrypted`
   puis `now in force`), à comparer aux 1,2–1,5 s du script.
3. Les renégociations de l'iPhone pendant une longue session : combien, et si la redemande (≤ 1/min) suffit.
4. Le coût : intervalle 60 ms au lieu de 30 ms (moins de radio pour l'iPhone) ; présence vue 6 s plus tard.
5. La règle des fautes et les retraits d'annonce devraient devenir rares : à compter sur une journée.

### 15.6 Ce qu'on lit dans le journal

| Ligne | Sens |
|---|---|
| `bluetooth: LE connection parameters are asked for as soon as it is encrypted on each LE link (45-60 ms, latency 0, supervision 6000 ms), by helper` | au démarrage : auxiliaire trouvé |
| `bluetooth: LE connection parameters are not asked for: this process holds no CAP_NET_RAW and there is no usable helper at …` | au démarrage : pas d'auxiliaire (une fois) |
| `bluetooth: asking the iPhone for LE connection parameters 45-60 ms, latency 0, supervision 6000 ms (attempt 1 of 3, by helper), once the link is encrypted` | demande lancée |
| `bluetooth: LE connection parameters now in force: interval 60 ms, latency 0, supervision 6000 ms (handle 3585)` | accordé |
| `[WARN] bluetooth: LE connection parameters not obtained (refused, HCI status 42…); a collision with the iPhone's own procedure, asking again in 2s` | collision (42 = 0x2A, 35 = 0x23) |
| `[WARN] … not obtained (not_ready: the LE link was not encrypted in time…)` | lien jamais chiffré en 5 s |
| `bluetooth: the LE connection parameters changed: interval 45 ms, latency 0, supervision 720 ms (handle N); shorter than asked for, asked again within a minute` | renégociation de l'iPhone |
| `bluetooth: the LE link whose parameters were watched ended (handle N, HCI reason 8)` | fin du lien (8 = délai de supervision) |
| `ancs: the iPhone did not answer the liveness probe within 5000ms; the notification session is subscribed again` | sonde ANCS sans réponse (15.7) |
| `ancs: no notification from the iPhone for 6h; …` | réabonnement après 6 h de silence (15.7) |
| `[WARN] bluetooth: 4 LE links that had served were lost within 60s; the advertisement goes at its slow interval…` | liens servis en boucle (15.8) |

### 15.7 Autres reprises de Pebble : la sonde de vie ANCS (`ancs/liveness.hpp`)

Une fois la session de notifications active, toutes les **15 min** (`LANDLINE_ANCS_PROBE_SECONDS`) le moteur
écrit au point de contrôle un *GetNotificationAttributes* pour l'UID **0xFFFFFFFF**, hors du séquenceur et
seulement quand aucune requête n'est en cours. iOS répond l'erreur ANCS 0xA2 (BlueZ : `Operation failed
with ATT error: 0xa2`) : c'est la preuve de vie. **Sans réponse en 5 s**, la session est reconstruite par le
chemin existant (`rebuild()` : oubli de ce côté, puis `StartNotify` sous les règles de
`subscription_guard.hpp`) — **jamais** de `StopNotify` suivi de `StartNotify` (§10). Une réponse ni oui ni
non (lien en train de tomber) : nouvel essai dans une minute. **Six heures** sans aucun paquet sur la
*Notification Source* (`LANDLINE_ANCS_QUIET_SECONDS`) : même reconstruction. Une notification reçue vaut
preuve de vie et repousse la sonde. `LANDLINE_ANCS_LIVENESS=0` éteint les deux.

Limite : la reconstruction de BlueZ ne réécrit pas le CCCD de l'iPhone pour un abonné qu'il tient encore
(Pebble, lui, réécrit les deux CCCD) ; si la sonde se tait alors que BlueZ se croit abonné, la vraie
réécriture ne vient qu'avec le lien suivant. Non observé ; à surveiller dans le journal.

### 15.8 Liens servis qui tombent en boucle (`le_strikes.hpp`)

*Depuis #29 (14.7), c'est le seul cas où l'annonce passe à 1022,5 ms par défaut.*

La règle des fautes (§14.2) ne compte que les liens morts **avant** service. Pebble ralentit aussi
l'annonce quand des liens **servis** tombent en boucle (`gap_le_slave_reconnect.c`, plus de 3 démarrages
en 60 s). Repris : au-delà de **3 pertes de liens servis en 60 s**, l'annonce passe à 1022,5 ms **sans phase
rapide**, jusqu'à une minute sans nouvelle perte. Les liens morts avant service, eux, ne retirent plus
l'annonce depuis #29 (14.7).

## 16. obexd au démarrage de session

### 16.1 Ce qui s'est passé (17 novembre, 22:50–22:53)

`obex.service` (obexd 5.87, `Type=dbus`, activé par D-Bus) démarre à 22:50:56 et s'arrête en erreur à
22:51:56 : « obex_server_init failed ». Lu dans les sources de BlueZ 5.87 (`obexd/src/logind.c`,
`obexd/plugins/bluetooth.c`) : le greffon Bluetooth n'enregistre son transport qu'une fois la session
dite « active » par logind ; démarré avant, `obex_server_init` trouve une liste de transports vide et
quitte avec le code 1. Pendant ces 60 s, obexd possédait déjà le nom `org.bluez.obex` sans répondre.

Côté moteur : `CreateSession` attendait `CONNECT_TIMEOUT_MS` (30 s), journal « map session failed
(other): Timeout was reached » ; la boucle est restée bloquée 30 s (retardant entre autres la
confirmation des paramètres LE), puis MAP et PBAP sont restés en `other`, obexd mort, sans que le
moteur sache le faire revenir.

### 16.2 Ce que fait le moteur

À chaque tour où un profil doit être ouvert, **un seul** regard sur obexd avant tout `CreateSession`
(`ProfileSupervisor::daemon_ready()`) : `org.freedesktop.DBus.Peer.Ping` sur `org.bluez.obex`,
`G_DBUS_CALL_FLAGS_NO_AUTO_START`, délai `OBEX_PING_TIMEOUT_MS` = 2 s. Le ping est servi par la
bibliothèque D-Bus d'obexd dès que sa boucle tourne : un obexd sain répond en millisecondes.

| Regard (`classify_obex_ping`) | Ce que fait le moteur | `CreateSession` |
|---|---|---|
| répond (`alive`) | rien | oui, comme avant |
| nom sans propriétaire (`NameHasNoOwner`, « Destination does not exist ») | `StartUnit("obex.service", "replace")` au gestionnaire systemd de l'utilisateur | non : motif `daemon_down` |
| nom possédé, ping sans réponse (`Timeout was reached`, `NoReply`) | `RestartUnit("obex.service", "replace")` ; les sessions qu'obexd tenait sont oubliées (rien n'est retiré à travers lui) | non : motif `daemon_down` |
| autre (bus fermé, accès refusé…) | rien | oui : son erreur dit ce qui se passe (`bus_closed`…) |

- Démarrage et redémarrage passent par le **même** compteur : **une fois par 60 s au plus**
  (`OBEX_DAEMON_RECOVERY_SECONDS`). Entre-temps, un obexd absent ou muet donne `daemon_down` sans
  rien redemander.
- systemd répond tout de suite par le chemin du travail mis en file (délai 2 s) : le moteur n'attend
  jamais obexd lui-même.
- **systemd injoignable ou unité inconnue** (pas de systemd utilisateur, obexd non installé) : le moteur
  retombe sur l'ancien chemin, `CreateSession` avec activation D-Bus ; un obexd absent reste nommé
  `no_daemon` (« not activatable »).
- Un `CreateSession` qui **expire quand même** (obexd figé entre le ping et l'appel) est classé
  `daemon_down` (`classify_obex_error` sur « Timeout was reached »), et non plus `other`. L'autre profil
  n'est pas tenté dans le même tour (il attendrait le même délai), obexd est regardé à nouveau, et
  relancé seulement si ce regard le confirme absent ou muet : un obexd qui répond encore au ping
  attendait probablement l'iPhone, et le relancer couperait la session de l'autre profil pour rien.
  Écart volontaire avec la demande initiale (« relancer sur tout délai expiré »).
- `CONNECT_TIMEOUT_MS` reste à **30 s** pour `CreateSession` : une vraie connexion OBEX à l'iPhone
  (page, SDP, L2CAP, OBEX Connect) prend plusieurs secondes, et aucune mesure ne montre qu'on peut
  descendre sans risque. Le ping préalable suffit à éviter le blocage de 30 s dans le cas du
  17 novembre, **si** obexd ne répondait pas non plus au ping pendant sa minute (probable : un
  `CreateSession` sans réponse veut dire que sa boucle ne tournait pas ; non vérifié).

`daemon_down` est **transitoire** : réessai 3 s plus tard (`OBEX_DAEMON_RETRY_SECONDS`) quand l'iPhone est là, au recul sans lien sinon ; « Timeout was reached », « NoReply » et « Remote peer disconnected » sont classés ainsi,
conseil « {Messages, Contacts} is waiting for this computer's OBEX service, which is starting again.
Nothing needs changing on the iPhone. », jamais de ré-appairage. Publié `unavailable` dans `messages`
/ `contacts`. L'extension ne le compte pas comme la panne « Fix It » (`extension/lib/model/obex.js`) :
relancer le moteur n'y changerait rien, et « Restarting did not bring messages and contacts back »
renvoie vers l'oubli de l'iPhone.

### 16.3 Le complément systemd

`system/obex-landline.conf`, posé par `scripts/install-local.sh` dans
`~/.config/systemd/user/obex.service.d/landline.conf` (retiré par `uninstall-local.sh`, dossier compris
s'il est vide) : `Restart=on-failure`, `RestartSec=10`, `StartLimitIntervalSec=300`,
`StartLimitBurst=10`, `TimeoutStopSec=5` (obexd figé ignore l'arrêt : 53 s le 18 novembre). obexd mort au démarrage de session revient seul dix secondes plus tard, sans
attendre le moteur. Les deux se complètent : le complément relance un obexd sorti en erreur, le
moteur démarre un obexd jamais lancé (ou arrêté proprement) et relance un obexd figé.

### 16.4 Ce qu'on lit dans le journal

- `bluetooth: obexd is not on the session bus; asked systemd to start obex.service`
- `bluetooth: obexd holds its name but does not answer; asked systemd to restart obex.service`
- `bluetooth: map session failed (daemon_down): Timeout was reached`
- `bluetooth: obexd ... systemd could not start|restart obex.service: …` : retour à l'activation D-Bus.

### 16.5 Ce qui reste à vérifier en réel

- Qu'obexd, pendant sa minute d'attente de logind, **ne répond pas** au ping (sinon le premier
  `CreateSession` attend encore 30 s, puis le motif est bien `daemon_down`, mais sans relance).
- Que `RestartUnit` sur un obexd figé au démarrage de session ne fait que recommencer l'attente tant
  que la session n'est pas active (sans dommage : une fois par 60 s), puis réussit.
- Que le complément relance obexd après « obex_server_init failed » (vérifiable au prochain démarrage
  de session : `journalctl --user -u obex.service`).

Tests : `ObexDaemon.*` dans `test/test_bluetooth_supervisor.cpp` (nom absent, sans systemd, obexd muet
et limite de 60 s, délai expiré avec obexd vivant puis muet, obexd sain inchangé, textes réels du bus).

**Essai réel du 18 novembre (01:13 puis 01:17).** obex.service arrêté exprès. À 01:13, la relève horaire a bien demandé obexd à systemd, mais l'ouverture du lien classique a été déclarée refusée une seconde plus tard : `daemon_down` comptait comme un refus de l'iPhone. Corrigé : `daemon_down` n'est plus un refus (`demand_refused`), et obexd est revu 3 s plus tard (`OBEX_DAEMON_RETRY_SECONDS`), dans la fenêtre d'ouverture de 20 s. À 01:17, même essai : obexd demandé à 01:17:36, contacts, appels récents et boîte de messages lus à 01:17:45. « Remote peer disconnected » (obexd arrêté pendant un appel) est aussi classé `daemon_down`.
