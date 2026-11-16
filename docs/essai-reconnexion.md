# Essai de reconnexion : protocole à dérouler avec l'iPhone

But : mesurer ce que rapportent les corrections du moteur (`moteur-reconnexion.md`, sections 1 bis et 7)
par rapport aux relevés du 16 novembre 2026.

**État.** La première série (branche `moteur-bluetooth-seul`) a été essayée deux fois avec l'iPhone le
16 novembre : scénarios A et H ci-dessous, un passage chacun, relevés en section 1. La seconde série
(branche `moteur-reconnexion-2`) est compilée et passe les tests unitaires, mais **n'a pas été essayée** :
ses temps « attendus » sont déduits de la lecture du code. Les scénarios A et H sont à refaire en premier.

## 1. Valeurs de référence

### Première série, mesurée le 16 novembre

Temps comptés **depuis le retour du lien LE** (et non depuis la chute des liens comme plus bas).

| Jalon | A — coupure côté PC | H — Bluetooth de l'iPhone coupé 55 s | Attendu, seconde série |
|---|---|---|---|
| Notifications actives (`ancs_ready`) | +9,7 s, **après** la lecture du carnet | +2,4 s | +3 à +5 s dans les deux cas |
| Messages et contacts (`map_open`, `pbap_open`) | +2,2 s | +6,5 s | +1 à +2 s |
| `classic_connected` vu par le client | +7,4 s | +11,4 s (5 s après `map_open`) | en même temps que `map_open`, à 1 s près |
| Appels (`calls_available`) | +7,4 s, sans cycle | +29,6 s, après un cycle | +7 à +9 s sans cycle ; +18 à +20 s avec |
| Coupure volontaire de BR/EDR | aucune | à +19,5 s | aucune si le mains-libres vient seul ; sinon vers +10 s |
| Pendant le cycle | — | `map_open` retombe, carnet relu (767 fiches, 5 s) | `map_open` / `pbap_open` restent à `oui`, carnet non relu |
| Annonce LE au retour | — | 152,5 ms (fenêtre rapide fermée depuis 25 s) | 20 ms (fenêtre de 180 s) |

### Avant toute correction

Relevé du 16 novembre 2026, coupure brève, iPhone éveillé (`iphone-link-study/bench/chrono-reconnexion.log`) :

| Jalon | Avant | Attendu après la première série | D'où vient le gain |
|---|---|---|---|
| T1 — lien LE revenu | < 1 s (0,6 s) | inchangé | rien à corriger iPhone éveillé ; l'intervalle d'annonce vise l'iPhone en veille |
| T2 — lien BR/EDR revenu | +32 s | +1 à +3 s | connexion BR/EDR asynchrone : le fil n'attend plus 30 s |
| T3 — notifications actives (`ancs_ready`) | +47 s | +2 à +6 s | ANCS rétabli avant les lectures longues |
| T4 — appels disponibles (`calls.available`) | +70 s | +12 à +15 s | cycle mains-libres après 8 s au lieu de 20, et démarré plus tôt |
| Coupure volontaire de BR/EDR | à +67 s, sans trace | vers +10 s, **avec** une ligne de journal | journalisation |

Tous les temps partent de **t0 = l'instant où les deux liens tombent**.

## 2. Préparation

Prérequis système (mode expérimental, classe, ERTM) : `moteur-inventaire.md`, section 5. Les vérifier avant
chaque séance, la classe et l'ERTM ne tenant pas au redémarrage :

```sh
busctl --system get-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Class   # attendu : …0408 en hexadécimal
cat /sys/module/bluetooth/parameters/disable_ertm                                  # attendu : N
```

Contrôle d'appel activé et iPhone déjà appairé (sinon T4 n'a pas de sens).

Binaire : `engine/build/local/landlined` (produit par `scripts/build-engine.sh`). Un seul démon à la fois :
arrêter celui du banc s'il tourne.

Quatre terminaux. Dans chacun : `export IPHONE=AA:BB:CC:DD:EE:FF` (adresse de l'iPhone) et
`P=/org/bluez/hci0/dev_${IPHONE//:/_}`. Les relevés vont dans un dossier `essais/` à créer
(`mkdir -p essais`) ; changer le préfixe `A1` à chaque passage.

**T1 — le démon, journal à l'écran**

```sh
cd ~/DEVELOPEMENT/iphone-link
engine/build/local/landlined 2>&1 | tee essais/A1-journal.log
```

Lancé depuis un terminal, le démon écrit son journal sur la sortie d'erreur. Première ligne à vérifier :
`Network side (TCP/TLS, mDNS, LAN info): disabled`.

**T2 — ce que voit un client abonné** (c'est la mesure qui compte pour l'extension)

```sh
scripts/chrono-moteur.py essais/A1-client.log
```

Écrit une ligne horodatée à chaque changement de `classic_connected`, `le_connected`, `ancs_ready`,
`map_open`, `pbap_open`, `calls_available`, à chaque notification et à chaque changement de la liste des
appels. Lecture seule.

**T3 — ce que voit BlueZ** (même outil que le relevé de référence)

```sh
source ~/DEVELOPEMENT/iphone-link-study/bench/env.sh
bt_chrono | tee essais/A1-bluez.log
```

Pour les deux moitiés du lien séparément, comme dans le relevé de référence :

```sh
while sleep 0.2; do
  printf '%s BREDR=%s LE=%s\n' "$(date +%H:%M:%S.%3N)" \
    "$(busctl --system get-property org.bluez $P org.bluez.Bearer.BREDR1 Connected 2>/dev/null)" \
    "$(busctl --system get-property org.bluez $P org.bluez.Bearer.LE1 Connected 2>/dev/null)"
done | uniq -f1 | tee essais/A1-liens.log
```

**T4 — la radio** (seulement pour les essais C et D ; demande les droits administrateur)

```sh
sudo btmon -w essais/C1.btsnoop
```

Vérifier que les trois horloges sont la même machine : tout est horodaté localement, il n'y a rien à
synchroniser.

## 3. Ce qu'il faut chronométrer

Pour chaque passage, relever dans les fichiers :

| Jalon | Où le lire | Ligne à chercher |
|---|---|---|
| t0 | `A1-liens.log` | première ligne `BREDR=b false LE=b false` |
| T1 | `A1-liens.log` / `A1-client.log` | `LE=b true` / `le_connected=oui` |
| T2 | idem | `BREDR=b true` / `classic_connected=oui` |
| T3 | `A1-client.log` | `ancs_ready=oui` |
| T3 bis | `A1-client.log` | première `notification nouvelle` après avoir envoyé un message de test à l'iPhone |
| T4 | `A1-client.log` | `calls_available=oui` |
| Coupure volontaire | `A1-journal.log` | `dropping the BR/EDR link on purpose` |
| Sessions | `A1-client.log` | `map_open=oui`, `pbap_open=oui` |

Dans le journal du démon, noter aussi la présence ou l'absence de :

- `BR/EDR connect failed (Timeout was reached)` — ne doit **plus** apparaître ;
- `BR/EDR connect unanswered after 30s; BlueZ is still working on it` — nouvelle ligne : BlueZ n'a pas
  répondu, mais le moteur n'a pas attendu ;
- `soliciting ANCS for 180s, advertising every 20-20 ms`, répétée ≈ 160 s plus tard (renouvellement), puis
  `… every 152-153 ms` 180 s après la perte du lien LE, puis de nouveau toutes les ≈ 160 s ;
- `BlueZ refused the advertising interval` — si elle apparaît, la correction 5.6 est sans effet sur cette
  machine (voir essai C).

Lignes ajoutées par la seconde série, à relever avec leur heure :

| Ligne | Ce qu'elle dit |
|---|---|
| `asking BlueZ for the BR/EDR link (Bearer.BREDR1.Connect)` ou `(Device1.Connect)` | une demande de connexion est partie |
| `BlueZ answered the BR/EDR connect: accepted` ou `: <erreur>` | la réponse, et son délai |
| `BR/EDR connect already running in BlueZ (…), standing back Ns` | BlueZ dit « déjà en cours » : aucune demande n'est partie à ce tour |
| `BR/EDR up for 5s without hands-free; asking BlueZ for the profile over the live link` | demande directe du mains-libres |
| `BlueZ accepted the hands-free request` / `declined the hands-free request (…)` | la réponse de BlueZ ; ne prouve rien sur l'iPhone |
| `notifications first; address book and messages wait up to 12s for them` | les lectures longues sont retenues |
| `address book read Ns ago and still N entries on the phone; not read again` | carnet gardé |
| `address book read Ns ago; reading it again in 300s` | carnet relu plus tard |

Faire **trois passages** par scénario et garder la médiane : la reconnexion dépend de l'instant où l'iPhone
écoute.

## 4. Scénarios

### A. Coupure brève, iPhone éveillé — la comparaison directe

Reproduit le relevé de référence. iPhone déverrouillé, posé à côté de l'ordinateur, tous les services
actifs depuis au moins une minute.

1. Attendre l'état stable : `classic_connected`, `le_connected`, `ancs_ready`, `calls_available` à `oui`.
2. Couper le lien depuis l'ordinateur : `bluetoothctl disconnect $IPHONE`. Noter l'heure.
3. Ne rien toucher pendant 2 minutes.
4. Relever t0, T1, T2, T3, T4 et l'heure de la coupure volontaire.
5. À +20 s environ, envoyer un message de test à l'iPhone depuis un autre appareil et relever T3 bis.

Si le relevé de référence avait été obtenu autrement (éloignement, Bluetooth coupé sur l'iPhone), refaire
le même geste : c'est la comparaison à l'identique qui compte.

**A bis — même geste, corrections coupées une à une**, pour attribuer le gain. Relancer le démon (T1) avec :

| Passage | Lancement | Ce qu'on isole |
|---|---|---|
| A-ref | `LANDLINE_SYNC_CLASSIC_CONNECT=1 LANDLINE_HFP_ABSENT_SECONDS=20 LANDLINE_ADVERT_INTERVALS=0 engine/build/local/landlined` | l'ancien comportement, moins l'ordre de la boucle : T2 doit retomber vers +32 s, T3 juste après |
| A-sync | `LANDLINE_SYNC_CLASSIC_CONNECT=1 engine/build/local/landlined` | ce que rapporte la seule connexion asynchrone |
| A-hfp20 | `LANDLINE_HFP_ABSENT_SECONDS=20 engine/build/local/landlined` | ce que rapportent les 8 s |
| A-tout | `engine/build/local/landlined` | tout activé |
| A-série1 | `LANDLINE_HFP_CONNECT_PROFILE=0 LANDLINE_CONTACTS_ALWAYS_PULL=1 LANDLINE_ANCS_FIRST_SECONDS=0 LANDLINE_ADVERT_FAST_SECONDS=30 LANDLINE_OBEX_ABSENT_BACKOFF=0 engine/build/local/landlined` | la première série, moins l'état publié dans le tour (7.5) et le masque du cycle, qui n'ont pas d'interrupteur |
| A-sans-attente | `LANDLINE_ANCS_FIRST_SECONDS=0 engine/build/local/landlined` | ce que rapportent l'attente des lectures longues et le pas de 1 s d'ANCS |
| A-sans-demande | `LANDLINE_HFP_CONNECT_PROFILE=0 engine/build/local/landlined` | si la demande directe du mains-libres change quelque chose |

L'ordre de la boucle (5.2) n'a pas d'interrupteur : pour retrouver les +47 s exacts, lancer le binaire
d'origine du banc (`$LANDLINE_BIN/landlined`) — attention, lui ouvre le port TCP, crée ses clés dans
`~/.config/landline/` et affiche ses bulles.

### B. Le cycle mains-libres : 8 s est-il trop court ?

À faire pendant le scénario A, sans geste supplémentaire. Dans `A1-client.log` et `A1-journal.log` :

1. Relever l'écart entre `classic_connected=oui` et `calls_available=oui`.
2. Si `calls_available=oui` arrive **sans** la ligne `dropping the BR/EDR link on purpose` : le profil
   mains-libres est venu seul, le cycle n'a pas eu lieu, c'est le meilleur cas.
3. Si la ligne apparaît : noter combien de temps après `classic_connected=oui`, et vérifier que
   `map_open` / `pbap_open` repassent à `oui` ensuite (la coupure les fait tomber).
4. Refaire avec `LANDLINE_HFP_ABSENT_SECONDS=20`. Si, à 20 s, le profil arrive seul entre 8 et 20 s, alors
   8 s coupe pour rien : remonter la valeur au premier palier qui laisse passer ce cas (12, 15…) et la
   reporter dans `include/tether/bluetooth/bearer_supervisor.hpp`.

Seconde série, mêmes fichiers :

5. Chercher `asking BlueZ for the profile over the live link` : la demande directe est partie. Si
   `calls_available=oui` suit **sans** `dropping the BR/EDR link on purpose`, noter l'écart : ou bien la
   demande a servi, ou bien le profil arrivait de lui-même. Refaire avec `LANDLINE_HFP_CONNECT_PROFILE=0` pour
   trancher. Si la coupure suit quand même trois secondes plus tard, la demande ne sert à rien sur cette
   machine, comme Tether l'avait mesuré.
6. Si la coupure a lieu : `map_open` et `pbap_open` ne doivent **pas** repasser à `non` dans `A1-client.log`
   (`classic_connected`, lui, retombe et revient) ; le journal doit montrer `address book read … not read
   again` et aucune ligne `pulled 767 contacts` après la coupure.
7. Dans l'essai 1 le mains-libres est venu ≈ 6 s après le lien, donc 2 s avant la coupure : la marge est
   mince. Si un passage montre la coupure juste avant l'arrivée du profil, essayer
   `LANDLINE_HFP_ABSENT_SECONDS=12`.

### C. L'intervalle d'annonce est-il vraiment appliqué ?

Avec `btmon` (T4). iPhone éloigné ou Bluetooth coupé dessus, pour que l'annonce reste en ondes.

1. Lancer le démon, attendre `soliciting ANCS for 180s, advertising every 20-20 ms`. Elle doit suivre la
   perte du lien de moins de 2 s (3,8 s le 16 novembre, derrière des tentatives OBEX qui n'ont plus lieu).
2. Dans `btmon`, chercher `LE Set Extended Advertising Parameters` : `Min advertising interval` et `Max`
   doivent valoir **20.000 msec** (0x0020).
3. Vers 160 s, le journal doit montrer la même ligne une seconde fois : c'est le renouvellement, 20 s avant
   l'expiration. Il ne doit **pas** y avoir de ligne `ANCS advertisement released by BlueZ` juste avant.
4. À 180 s après la perte du lien, une ligne `… every 152-153 ms`, et dans `btmon` un nouvel enregistrement
   à **151.875 / 152.500 msec** (0x00f3 / 0x00f4). Ensuite la même ligne toutes les ≈ 160 s, sans fin.
5. Mesurer le trou à chaque réenregistrement (`LE Set Extended Advertising Enable` désactivé puis activé) :
   attendu quelques dizaines de millisecondes ; au-delà de 300 ms, le noter.
6. Laisser tourner 15 minutes et vérifier qu'il n'y a toujours qu'**une** instance d'annonce :
   `busctl --system get-property org.bluez /org/bluez/hci0 org.bluez.LEAdvertisingManager1 ActiveInstances`.
7. Consommation, si on veut la chiffrer : `powertop` sur batterie, cinq minutes démon arrêté puis cinq
   minutes à 152,5 ms, Wi-Fi au repos. L'écart attendu est de quelques milliwatts, sous le bruit de mesure.

Si `btmon` montre toujours 1280 msec : BlueZ a ignoré les propriétés (mode expérimental absent, ou noyau
sans les commandes d'annonce étendues). Si le journal montre `BlueZ refused the advertising interval` :
BlueZ les a rejetées et le moteur s'est rabattu sur l'annonce d'origine. Dans les deux cas le réglage
global reste possible (`[LE] MinAdvertisementInterval` / `MaxAdvertisementInterval` dans
`/etc/bluetooth/main.conf`, voir `moteur-reconnexion.md`, 5.6).

### D. iPhone verrouillé ou en veille — le gain attendu de l'annonce

C'est la mesure que personne n'a publiée, et la seule où l'intervalle d'annonce doit se voir sur T1.

1. État stable, puis verrouiller l'iPhone, écran éteint, et attendre 2 minutes.
2. `bluetoothctl disconnect $IPHONE`. Ne pas toucher l'iPhone.
3. Relever T1 à T4. Laisser jusqu'à 5 minutes.
4. Refaire trois fois avec `LANDLINE_ADVERT_INTERVALS=0`, trois fois sans.

Comparer la médiane et le pire cas de T1. Variante : iPhone verrouillé depuis 30 minutes (veille profonde).

### E. Sortie de portée

Lignes 11a à 11c du banc (`iphone-link-study/04-banc-essai.md`, 4.10), avec les mêmes relevés.

1. S'éloigner jusqu'à la coupure (motif `org.bluez.Reason.Timeout`), attendre 2 minutes, revenir.
2. Relever t0 = l'instant du retour dans la pièce, puis T1 à T4.
3. Pendant l'absence, vérifier dans le journal que l'annonce est bien renouvelée avant chaque expiration
   des 180 s (ligne `soliciting ANCS…` toutes les 160 s environ, à 152-153 ms passé les trois premières
   minutes, sans ligne `released by BlueZ` entre deux) et que le moteur ne page pas l'iPhone
   plus d'une fois par minute environ, une fois le délai monté à son plafond
   (`BR/EDR connect failed (… page-timeout), retrying in 60s`).

Ce dernier point surveille un risque de la connexion asynchrone : une régression ferait réapparaître des
tentatives toutes les 5 à 10 s, ce que Tether a identifié comme capable de bloquer `bluetoothd`.

Seconde série : pendant l'absence, les lignes `map session failed` doivent s'espacer — 15 s après la
perte, puis 30 s, 60 s, 120 s, puis toutes les 2 minutes — et aucune ne doit suivre la perte de moins de
15 s. Au retour, `map_open=oui` doit suivre `le_connected=oui` de 1 à 2 s.

### H. Bluetooth coupé sur l'iPhone, puis rallumé

Le second essai du 16 novembre, et celui où restait le plus à gagner. iPhone à côté, état stable.

1. Couper le Bluetooth dans Réglages (pas dans le Centre de contrôle, qui ne coupe pas tout). Noter l'heure.
2. Attendre 55 s — puis refaire avec 5 minutes, pour sortir de la fenêtre rapide de l'annonce.
3. Rallumer. Relever, depuis `le_connected=oui` : `ancs_ready`, `map_open`, `classic_connected`,
   `calls_available`, et l'heure d'une éventuelle coupure volontaire.
4. Dans le journal, entre le retour du LE et `map_open` : y a-t-il `asking BlueZ for the BR/EDR link`, et
   quelle réponse ? C'est la question laissée ouverte par l'essai du 16 novembre (`moteur-reconnexion.md`,
   1 ter). `already running in BlueZ` à ce moment-là veut dire que le lien est encore monté par obexd seul :
   dans ce cas le cycle aura lieu, et c'est la correction 5.3 (retenir OBEX) qu'il faudra essayer.
5. Vérifier les points 5 et 6 du scénario B.

Référence du 16 novembre (55 s) : notifications +2,4 s, messages +6,5 s, `classic_connected` +11,4 s,
coupure volontaire +19,5 s, carnet relu, appels +29,6 s.

### I. Relance du moteur, iPhone à côté — le LE revient-il ? (branche `moteur-robuste`)

Les cas (a) et (c) du 16 novembre : après une relance du moteur, BR/EDR restait monté et le LE ne revenait
pas pendant des minutes (`moteur-reconnexion.md`, 10.3). État de départ : tout connecté, `ancs_ready=oui`.

1. Noter l'heure, relancer le moteur (`systemctl --user restart landline-engine.service`, ou Ctrl-C
   puis relance dans T1).
2. Relever depuis le démarrage (`landlined version …`) : `le_connected=oui`, `ancs_ready=oui`, `map_open`,
   `classic_connected`, `calls_available`.
3. Dans le journal, deux déroulés possibles :
   - le LE était resté monté : `notification session N started` en 2 à 3 s, **aucune** ligne
     `dropping every link on purpose`. C'est le bon cas ; noter le temps ;
   - le LE était tombé : `BR/EDR up for 12s with the solicitation on air and no LE link; dropping every
     link on purpose, once…`, puis `no link to the iPhone; leaving BR/EDR alone for up to 10s…`, puis
     `the LE link came first, Ns after every link was down`. Noter N, et le temps total.
4. **Recyclage à tort ?** Si la ligne `dropping every link on purpose` apparaît alors que le LE était monté
   avant la relance (le vérifier dans `A-liens.log` : `LE=b true` jusqu'à la coupure), le noter : c'est le
   point 5 de 10.7, et `LANDLINE_LE_RECYCLE_SECONDS=30` est le premier réglage à essayer.
5. Vérifier que l'annonce n'est **plus** retirée 3 s après le démarrage : entre deux lignes `soliciting
   ANCS`, pas d'écart de ≈ 49 s comme le 16 novembre (18:52:48 → 18:53:37).
6. Le son : `pactl list cards short | grep bluez` avant et après ; une lecture en cours sur l'iPhone
   (réglage « ici ») doit empêcher le recyclage (aucune coupure pendant qu'elle joue).

Trois passages, puis un passage avec `LANDLINE_LE_FIRST=0` pour retrouver le défaut et le mesurer.

Attendu (**déduit**) : LE resté monté, notifications en 3 s ; LE tombé, recyclage vers +13 s, LE vers
+15 s, tout vers +23 s. Référence sans la correction : LE absent 24 min (cas (c)).

### J. Relance de bluetoothd — tout revient-il sans toucher au moteur ?

Le cas (b). **Coupe le clavier, la souris et le casque Bluetooth quelques secondes** : brancher un clavier
filaire, prévenir. Demande les droits d'administrateur ; à faire par l'utilisateur.

1. État stable, `ancs_ready=oui`. Dans un terminal : `journalctl -fu bluetooth`. Noter l'heure, puis
   `sudo systemctl restart bluetooth`.
2. Journal du moteur, dans l'ordre attendu :
   `[WARN] bluetoothd left the bus…` → `bluetoothd is back on the bus` →
   `[WARN] bluetoothd was restarted; registering the advertisement again…` →
   `soliciting ANCS for 180s, advertising every 20-20 ms` (dans les 2 s) →
   `no link to the iPhone; leaving BR/EDR alone for up to 10s…` →
   `the LE link came first, Ns after every link was down` →
   `asking BlueZ for the BR/EDR link (Bearer.BREDR1.Connect)` → sessions, appels, notifications.
3. Relever, depuis `bluetoothd is back` : LE, `map_open`, `classic_connected`, `calls_available`,
   `ancs_ready`. Référence à battre : 19:18 du 16 novembre, tout en 8 s après le LE.
4. Le son : `pactl list cards short | grep bluez` doit montrer la carte de l'iPhone ; l'iPhone doit
   proposer l'ordinateur comme sortie audio. Le 16 novembre, plus rien jusqu'à une coupure complète.
5. Si la ligne `leaving BR/EDR alone` **manque** : l'iPhone n'est pas vu comme offrant ANCS au retour de
   bluetoothd (point 4 de 10.7). Relever `busctl --system get-property org.bluez
   /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX org.bluez.Device1 UUIDs` juste après la relance.
6. Si `no LE link after 10s with the solicitation alone on air` apparaît : la retenue n'a pas suffi ; noter
   si quelque chose a remonté BR/EDR pendant ces 10 s (`A-liens.log`), et refaire iPhone déverrouillé.
7. Le moteur ne doit pas avoir été relancé : même PID avant et après (`systemctl --user status`).
8. Variante : `sudo systemctl stop bluetooth`, attendre 30 s, `start`. Et : relancer le moteur pendant que
   bluetoothd est arrêté ; il doit écrire `BlueZ unavailable: …; waiting for bluetoothd to come to the
   bus` et reprendre seul au `start`.

### K. bluetoothd ne plante plus — la séquence de 18:47

Le plantage demandait un lien qui cesse de répondre pendant qu'un abonnement est reconstruit. Il ne se
provoque pas sur commande ; on vérifie que le moteur n'émet plus la séquence.

1. `busctl --system monitor org.bluez > K-bus.log` (lecture seule) pendant les scénarios A, E, H et I.
2. Dans `K-bus.log`, chercher `Member=StopNotify` : **il ne doit y en avoir aucun** pendant une
   reconnexion. Le seul admis suit la coupure des notifications par l'utilisateur.
3. Chercher `Member=StartNotify` : nombre par reconnexion, et aucun dans les 35 s qui suivent un
   `StopNotify`.
4. Après une coupure du **seul** LE (s'éloigner jusqu'à `Bearer.LE1 disconnected (Timeout)` avec BR/EDR
   encore monté, revenir) : `ancs: the notification subscription is no longer live (LE link dropped)`,
   puis `notification session N started` et `Notification mirroring is active` **sans relancer le
   moteur**, puis une notification de test qui arrive. C'est le point 2 de 10.7 : si les notifications
   restent mortes, relever `Notifying` des deux caractéristiques et le dire — ce serait le cas que Tether
   croyait corriger par `StopNotify`.
5. Sur toute la séance : `journalctl -u bluetooth | grep -c Assertion` doit rester à zéro, et
   `systemctl show bluetooth -p NRestarts` ne doit pas bouger.
6. Couper puis remettre les notifications depuis l'extension, deux fois de suite en moins de 30 s : le
   journal doit dire `waiting for BlueZ to finish turning notifications off before asking for them again`
   et les notifications revenir au plus 35 s après.

### L. Batterie de l'iPhone et cadences

Le 16 novembre : 80 % à 18:35, 60 % à 19:18, dont un appel de 13 min. Rien ne dit que le moteur y est pour
quelque chose ; il faut une mesure à blanc.

1. iPhone chargé à 100 %, écran éteint, posé à côté, une heure moteur arrêté et Bluetooth de l'ordinateur
   coupé : noter la baisse.
2. Même heure, tout connecté, moteur en marche, sans appel ni son : noter la baisse.
3. Même heure avec `LANDLINE_MESSAGE_POLL_SECONDS=120` : la relève des messages toutes les 15 s est la seule
   sollicitation sans fin (`moteur-reconnexion.md`, 10.6).
4. Pendant le pas 2, `busctl --system monitor org.bluez | grep -c ReadValue` sur une minute : s'il y a une
   lecture par seconde sur la caractéristique du nom (GAP), c'est le contrôle du lien LE qui tourne à chaque
   tour ; le noter.
5. Réglages › Batterie sur l'iPhone ne détaille pas le Bluetooth ; seule la différence entre les pas 1, 2
   et 3 compte.

### F. Appels

Contrôle d'appel activé. Faire appeler l'iPhone depuis un autre téléphone.

1. Appel entrant, décroché sur l'iPhone, raccroché : le journal doit montrer trois lignes
   `call: incoming from ***NN`, `call: answered, …`, `call: ended, …`, avec les deux derniers chiffres du
   numéro et rien d'autre ; `A1-client.log` montre `appels=[incoming]`, `appels=[active]`, `appels=[]`.
2. Appel manqué (laisser sonner) : deux lignes, `incoming` puis `ended, … (was incoming)`.
3. **Appel dans les 30 s qui suivent une reconnexion** (scénario A puis appel à +10 s) : c'était le cas où
   l'appel sonnait sans laisser de trace. Les lignes doivent maintenant apparaître ; sinon, noter ce que
   faisait le fil à ce moment (dernières lignes du journal).
4. Vérifier qu'aucun numéro complet n'apparaît dans le journal : `grep -E '\+?[0-9]{6,}' essais/*-journal.log`.

### G. Bluetooth seul et confirmation d'appairage

À faire une fois, pas à chaque séance.

1. Démon lancé : `ss -ltnp | grep 5134` ne renvoie rien ; `ls ~/.config/landline/` ne montre ni `cert.pem` ni
   `key.pem` ; `avahi-browse -t _tether._tcp` ne trouve pas la machine.
2. Aucune bulle de bureau du démon à l'arrivée d'une notification iPhone (l'événement `bt_notification`
   apparaît bien dans `A1-client.log`).
3. Appairage neuf (après `bt_unpair`) piloté par le socket : aucune fenêtre `tether-dialog` ; la ligne
   `demande de confirmation d'appairage` apparaît dans `A1-client.log`. Répondre depuis un second client :

   ```sh
   python3 - <<'EOF'
   import os, socket
   s = socket.socket(socket.AF_UNIX)
   s.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], "landline", "landlined.sock"))
   s.sendall(b'{"command":"bt_pair_confirm","accept":true}\n')
   print(s.recv(100))
   EOF
   ```

   (`operation_id` à ajouter s'il figurait dans le `bt_pair` d'origine ; voir `moteur-api.md`, section 6.)

## 5. Tableau à remplir

| Scénario | Passage | t0 | T1 LE | T2 BR/EDR | T3 `ancs_ready` | T3 bis 1re notif. | T4 appels | Coupure volontaire | Remarques |
|---|---|---|---|---|---|---|---|---|---|
| Référence 16 nov. | — | 13:15:06,9 | +0,6 s | +31,9 s | +47 s | — | +70 s | +66,9 s (muette) | BR/EDR revenu par la session messages |
| A, 1re série, 16 nov. | 1 | 14:49:07,9 | ≈ +0,4 s | +7,8 s | +9,7 s après le LE | — | +7,4 s après le LE | aucune | carnet lu avant les notifications |
| H, 1re série, 16 nov. | 1 | 14:54:09,9 | +56,8 s (Bluetooth rallumé) | LE +11,4 s | LE +2,4 s | — | LE +29,6 s | LE +19,5 s | carnet relu après la coupure |
| A-tout | 1 | | | | | | | | |
| A-tout | 2 | | | | | | | | |
| A-tout | 3 | | | | | | | | |
| A-sync | 1 | | | | | | | | |
| A-hfp20 | 1 | | | | | | | | |
| A-ref | 1 | | | | | | | | |
| D verrouillé, intervalle | 1–3 | | | | | | | | |
| D verrouillé, sans | 1–3 | | | | | | | | |
| E portée | 1 | | | | | | | | |
| H 55 s | 1–3 | | | | | | | | |
| H 5 min | 1–3 | | | | | | | | |

## 6. Lecture des résultats

- **Le LE ne revient pas après une relance (I, J)** : lire quelle ligne manque. Pas de `leaving BR/EDR
  alone` ni de `dropping every link` : la garde « l'iPhone a déjà offert ANCS » n'est pas remplie, ou
  `LANDLINE_LE_FIRST=0`. `no LE link after 10s…` : l'iPhone n'a pas répondu à l'annonce seule en ondes ;
  l'hypothèse de `moteur-reconnexion.md` 10.3 est alors à revoir, et le relevé `btmon` de ces 10 s est ce
  qu'il faut garder.
- **Une ligne `Assertion` dans le journal de bluetooth** : garder `K-bus.log` et le journal du moteur de la
  minute qui précède ; la séquence `StopNotify` / `StartNotify` s'y lit directement.

- **T2 reste vers +30 s** : la connexion asynchrone n'est pas en cause (le fil n'attend plus), c'est BlueZ
  qui ne rétablit pas le lien et obexd qui ne le fait plus à sa place. Regarder si `map_open=oui` arrive
  tôt ; sinon reprendre la capture de `moteur-reconnexion.md`, section 4.
- **T3 ne descend pas sous +6 s** : vérifier que la ligne `notifications first…` apparaît et qu'aucune
  ligne `pulled … contacts` ne la suit avant `Notification mirroring is active`. Si c'est le cas et que T3
  reste haut, ce qui retient ANCS n'est plus une lecture longue : regarder l'écart entre `Subscribing…` et
  `notification session N started` (pas de 1 s pendant 20 s), puis la sonde d'autorisation (≈ 2,5 s, non
  modifiée). L'ouverture des sessions OBEX, non retenue, occupe encore le fil 1 à 2 s.
- **T4 ne descend pas sous +18 s** : le cycle a eu lieu. Lire dans le journal ce qu'est devenue la demande
  de connexion au retour du LE (scénario H, point 4) ; `already running in BlueZ` désigne la correction 5.3.
- **Les sessions messages et contacts retombent pendant le cycle** : le masque n'a pas tenu. Noter combien
  de temps elles restent à `non` (il couvre 20 s) et si `pbap_open` ou `map_open` était à `non` avant la
  coupure.
- **`pulled N contacts` après chaque reconnexion** : `GetSize` n'est pas servi par l'iPhone ou rend un autre
  nombre ; chercher la ligne `address book read …` qui précède et les deux nombres.
- **Un passage plus lent qu'avant** : le refaire avec l'interrupteur correspondant coupé (A bis) avant de
  conclure ; garder les quatre fichiers du passage.

## 7. Après l'essai

Arrêter le démon (Ctrl-C dans T1) et vérifier qu'il ne reste rien : `pgrep -x landlined`. Le démon aura créé
`~/.config/landline/` (réglages), `~/.local/state/landline/` et `~/.local/share/landline/` (journal chiffré des
messages, cache des contacts) : c'est attendu dès qu'il tourne pour de bon. Reporter les médianes dans
`moteur-reconnexion.md`, section 1 bis, à la place des temps « déduits ».
