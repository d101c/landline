# Essai de reconnexion : protocole à dérouler avec l'iPhone

But : mesurer ce que rapportent les corrections du moteur (`moteur-reconnexion.md`, sections 1 bis et 7)
par rapport aux relevés du 16 novembre 2026.

**État.** La première série (branche `moteur-bluetooth-seul`) a été essayée deux fois avec l'iPhone le
16 novembre : scénarios A et H ci-dessous, un passage chacun, relevés en section 1. La seconde série
(branche `moteur-reconnexion-2`) est compilée et passe les tests unitaires, mais **n'a pas été essayée** :
ses temps « attendus » sont déduits de la lecture du code. Les scénarios A et H sont à refaire en premier.

## 00. Tolérance du lien LE intégrée : essais restants (branche `tolerance`)

**P1 est passé le 17 novembre 2026** (20:40 et 20:55–21:53, `essais/lecup-suivi.sh`) : l'iPhone applique
60 ms / latence 0 / 6 s, et un lien a tenu près d'une heure (`moteur-reconnexion.md` §15.1). Le moteur fait
désormais la demande lui-même, par défaut, dès que l'auxiliaire est posé. Ce qui reste à vérifier en réel,
dans cet ordre. **Arrêter d'abord le script `lecup-suivi.sh`** s'il tourne : deux demandeurs se
croiseraient (collisions 0x23/0x2A).

### T1. Poser l'auxiliaire par l'assistant (5 min, mot de passe)

1. `scripts/build-engine.sh`, puis `scripts/install-local.sh` (copie `landline-le-params` dans
   `~/.local/lib/landline/engine/`, sans droit).
2. `~/.local/lib/landline/landline-setup apply --dry-run` : attendu `copierait : …/engine/landline-le-params
   -> /usr/local/libexec/landline/landline-le-params` et `exécuterait : setcap cap_net_raw+ep …`.
3. `sudo ~/.local/lib/landline/landline-setup apply` (ou l'écran d'autorisation de l'extension : « Keep the
   Link Steady… » dans le menu). Attendu : `auxiliaire en place avec cap_net_raw`, code 0, et
   `"le_params":true` à la fin.
4. `getcap /usr/local/libexec/landline/landline-le-params` → `cap_net_raw=ep` ; `ls -l` → `root root
   -rwxr-xr-x`.
5. `systemctl --user restart landline-engine.service` ; au journal, une fois : `LE connection parameters are
   asked for as soon as it is encrypted on each LE link (45-60 ms, latency 0, supervision 6000 ms), by
   helper`.

### T2. La demande à chaque lien, au plus tôt (15 min, `btmon`)

1. `sudo btmon | tee essais/T2-btmon.log`.
2. Éloigner l'iPhone jusqu'à la perte du LE, revenir ; recommencer 5 fois.
3. Pour chaque lien, relever dans `btmon` : *LE Enhanced Connection Complete* → *Encryption Change* →
   *LE Connection Update* (commande) → *LE Connection Update Complete* 6000 ms. Attendu : la commande
   **moins de 0,2 s après le chiffrement** (le moteur regarde toutes les 50 ms) ; au journal, `asking … once
   the link is encrypted` puis `now in force: interval 60 ms, latency 0, supervision 6000 ms`.
4. Le menu affiche « Steady link: rides out 6 s gaps » (état publié : `le_conn_params` `applied`,
   `le_conn_values` `{"interval_ms":60,"latency":0,"timeout_ms":6000}`).
5. Compter les liens morts **entre la montée et le chiffrement** (avant toute demande possible). S'il y en a
   plusieurs : refaire T2 avec `LANDLINE_LE_CONN_ENCRYPTED=0` (`systemctl --user edit` puis
   `Environment=`) et comparer le nombre de collisions (`HCI status 35` ou `42` au journal).

### T3. Renégociation de l'iPhone (une soirée, sans rien faire)

Laisser tourner une soirée, `btmon` en route. Chercher `the LE connection parameters changed` au journal :
chaque renégociation de l'iPhone sous 6 s doit être suivie, **au plus une minute après** la précédente
demande, d'une nouvelle demande et de `now in force … 6000 ms`. Noter combien, et à quelle heure.

### T4. Refus et retrait (10 min)

1. Refuser le mot de passe sur l'écran « Keep the link to your iPhone steady » : la configuration continue,
   rien n'est demandé ailleurs, le menu propose encore « Keep the Link Steady… ».
2. `sudo /usr/local/libexec/landline/landline-setup revert` : l'auxiliaire et son témoin disparaissent ;
   au redémarrage du moteur, une ligne `are not asked for: … no usable helper`, `le_conn_params:"no_right"`,
   et le comportement d'avant.

### T5. Sonde de vie ANCS et liens servis en boucle (une nuit, puis 10 min)

1. Une nuit au repos : au journal (niveau debug), une ligne `answered the liveness probe in Nms` toutes les
   15 min ; aucune ligne `did not answer the liveness probe`. Après 6 h sans notification : `no notification
   from the iPhone for 6h`, suivi d'une session de nouveau active.
2. Aller-retour en limite de portée avec des liens qui servent puis tombent : au-delà de 4 pertes en
   une minute, `LE links that had served were lost within 60s; the advertisement goes at its slow
   interval` ; l'annonce reste en ondes (pas de `holding_off`).

| Essai | Résultat | Délai montée → 6 s | Collisions | Renégociations | Remarques |
|---|---|---|---|---|---|
| T1 | | sans objet | sans objet | sans objet | |
| T2 | | | | | |
| T2 sans chiffrement | | | | | |
| T3 | | sans objet | | | |
| T4 | | sans objet | sans objet | sans objet | |
| T5 | | sans objet | sans objet | sans objet | |

## 0. À faire en premier : paramètres de connexion et « trois fautes » (branche `moteur-trois-fautes`)

> **P1 fait le 17 novembre** : voir la section 00 ci-dessus. Les instructions d'activation de P1
> ci-dessous (`LANDLINE_LE_CONN_PARAMS=1`, 150–180 ms, installation à la main) sont **dépassées** :
> la demande est active par défaut et `landline-setup apply` pose l'auxiliaire.

À lire avant : `moteur-reconnexion.md` §14. Trois essais, dans cet ordre. Le premier tient en une ligne et
décide de la suite ; il se fait **avec le démon installé tel qu'il est**, sans rien recompiler.

### P1. L'essai d'une ligne : 6 s de délai de supervision, à la main (5 min, root)

But : savoir si le contrôleur et l'iPhone acceptent la demande que le moteur sait maintenant faire
(`LANDLINE_LE_CONN_PARAMS=1`, désactivée tant que cet essai n'est pas passé), et si les coupures cessent.

1. iPhone à côté, lien LE monté (`le_connected` vrai, notifications actives depuis plus de 10 s). Dans un
   terminal, laisser tourner la capture :

   ```sh
   sudo btmon | tee essais/P1-btmon.log
   ```

2. Trouver le *handle* du lien LE (aucun droit requis) :

   ```sh
   hcitool con
   #   Connections:
   #       > LE A0:B1:C2:D3:E4:F5 handle 3585 state 1 lm PERIPHERAL
   ```

   C'est la ligne `LE` à l'adresse de l'iPhone ; le nombre après `handle` est celui qu'il faut. `>` : lien
   ouvert par l'iPhone ; `PERIPHERAL` : l'ordinateur est le périphérique (attendu).

3. Demander les paramètres (intervalles en unités de 1,25 ms, délai en unités de 10 ms) — ceux que le moteur
   demandera, 150–180 ms, latence 0, 6 s :

   ```sh
   sudo hcitool lecup --handle 3585 --min 120 --max 144 --latency 0 --timeout 600
   ```

4. Ce qu'on doit voir dans `btmon`, dans l'ordre :

   ```
   < HCI Command: LE Connection Update (0x08|0x0013) plen 14
           Handle: 3585
           Min connection interval: 150.00 msec (0x0078)
           Max connection interval: 180.00 msec (0x0090)
           Connection latency: 0 (0x0000)
           Supervision timeout: 6000 msec (0x0258)
   > HCI Event: Command Status (0x0f) plen 4
         LE Connection Update (0x08|0x0013) ncmd 1
           Status: Success (0x00)
   > HCI Event: LE Meta Event (0x3e) plen 10
         LE Connection Update Complete (0x03)
           Status: Success (0x00)
           Handle: 3585
           Connection interval: 165.00 msec (0x0084)
           Connection latency: 0 (0x0000)
           Supervision timeout: 6000 msec (0x0258)
   ```

   C'est la dernière ligne qui compte : **`Supervision timeout: 6000 msec`** dans *LE Connection Update
   Complete*. L'intervalle peut être n'importe quelle valeur entre 150 et 180 ms, ou une autre si l'iPhone
   répond par les siennes.

   | Ce qu'on voit à la place | Sens | Suite |
   |---|---|---|
   | `hcitool` : `Could not change connection params: Operation not permitted` | lancé sans `sudo` | relancer avec |
   | *Command Status* : `Command Disallowed (0x0c)` | le contrôleur refuse (procédure déjà en cours, ou rôle périphérique non admis par ce micrologiciel) | réessayer une fois après 30 s ; sinon noter, la voie est fermée sur ce contrôleur |
   | *Update Complete* : `Unsupported Remote Feature (0x1a)` | l'iPhone ne fait pas la procédure de couche liaison | noter ; seule la demande L2CAP resterait, que le noyau garde pour lui |
   | *Update Complete* : `Unacceptable Connection Parameters (0x3b)` ou d'autres valeurs que celles demandées | l'iPhone refuse ou propose autre chose | essayer `--min 24 --max 36` (30–45 ms), puis `--min 36 --max 48` (45–60 ms), toujours `--timeout 600` |
   | rien pendant 40 s, puis `Disconnect Complete` … `LL Response Timeout (0x22)` | l'iPhone n'a pas répondu, le lien est fermé | noter ; ne pas activer la demande dans le moteur |
   | plus tard, `LE Remote Connection Parameter Request` suivi d'une réponse négative | l'iPhone propose d'autres valeurs et le noyau refuse | noter les valeurs proposées |

5. **Constater que les coupures cessent**, lien à 6 s (le réglage ne vaut que pour ce lien : après une
   coupure, refaire les étapes 2 et 3). Dans un autre terminal, compter les coupures LE au journal :

   ```sh
   tail -f ~/.local/state/landline/landlined.log | grep --line-buffered 'Bearer.LE1 disconnected'
   ```

   a. *Recherche en classique* : iPhone posé deux pièces plus loin, là où le lien tient tout juste ; demander
      une lecture des messages depuis l'interface dix fois, à 1 min d'intervalle. Le 17 novembre : 1 lien LE
      perdu sur 6 ouvertures, 0,72 s après le début de la recherche. Attendu : **zéro sur dix**, et dans
      `btmon` des `Create Connection` de plusieurs secondes sans `Disconnect Complete` sur le *handle* LE.
   b. *Limite de portée* : iPhone en poche, aller jusqu'à la porte et revenir, trois fois, sans sortir de
      portée plus de 5 s. Attendu : aucune ligne `disconnected`. Sans le réglage, chaque trou de 0,72 s en
      est une.
   c. *Départ* : sortir pour de bon. Attendu : une seule ligne `disconnected` (`Reason.Timeout`), environ
      6 s après la perte réelle, au lieu de 0,7 s — c'est le prix du réglage.
   d. Contre-épreuve, un lien neuf sans la commande : refaire (a) et (b) et comparer.

6. À noter dans le tableau : accepté o/n, valeurs obtenues, coupures en (a), (b), (c) avec et sans.

**Si P1 passe**, activer la demande dans le moteur. Ce que l'assistant système doit installer une fois
(détail et risques : `moteur-reconnexion.md` §14.4) :

```sh
sudo install -d -o root -g root -m 0755 /usr/local/libexec/landline
sudo install -o root -g root -m 0755 engine/build/local/landline-le-params /usr/local/libexec/landline/
sudo setcap cap_net_raw+ep /usr/local/libexec/landline/landline-le-params
```

puis `LANDLINE_LE_CONN_PARAMS=1` dans l'environnement du démon. Attendu au journal, à chaque lien LE, 5 s
après sa montée : `asking the iPhone for LE connection parameters 150-180 ms, latency 0, supervision 6000 ms
(attempt 1 of 3, by helper)` puis `LE connection parameters now in force: interval … ms, latency 0,
supervision 6000 ms` ; dans `btmon`, les trois blocs de l'étape 4. Sans l'auxiliaire : une seule ligne
`LE connection parameters are wanted … but this process may not ask for them`, et rien d'autre ne change.

### P2. Retour à la maison : la règle des trois fautes (10 min, sans root)

Remplace R1 (section 10). Démon de cette branche, mode sobre, `btmon` lancé si possible.

1. iPhone verrouillé en poche. Sortir jusqu'à `iPhone away` au journal, attendre 2 min, revenir à pas
   normal jusqu'au bureau.
2. Attendu au journal, au départ : `Bearer.LE1 disconnected` (le lien servi : pas une faute), puis, si un
   lien renaît et meurt en limite de portée, **une** ligne `the LE link ended before the notification service
   had answered (unused links in a row: 1); the advertisement is taken off air for 15s`, puis au plus une
   seconde (`… in a row: 2 … for 120s, then put back at its slow interval`). Le 17 : 9 liens en 35 s.
3. Attendu au retour : `org.bluez.Bearer.LE1 connected to …`, `notification subscription confirmed Nms
   after the LE link came up`, `notification service in use Nms after the LE link came up` — **noter ces
   deux durées** (c'est la mesure que réclame l'étude 12 : la fenêtre pendant laquelle une coupure est une
   faute) — puis, 10 s plus tard, `the notification service has been live on the LE link for 10s; the N
   unused link(s) before it are forgotten` s'il y avait eu des fautes. Le 17 : une vingtaine de liens en 80 s.
4. Pendant un retrait, l'état publié doit être `le_state:"waiting"`, `le_cause:"holding_off"`, avec
   `le_strikes` à 1 ou 2 ; au journal : `notification link waiting: holding_off`.
5. Noter : nombre de liens LE entre le pas de la porte et le lien qui tient ; nombre de fautes ; délai entre
   le pas de la porte et `Notification mirroring is active` ; l'iPhone est-il revenu **seul** ?
6. Échec : l'iPhone ne revient plus après le dernier retrait (`paused` / `phone_backoff` ou `phone_away`) →
   noter l'heure et le nombre de fautes, passer à R5 sans toucher à l'iPhone.
7. Contre-essai, un autre jour : `LANDLINE_LE_STRIKES=0 LANDLINE_LE_PROMPT=0` (comportement de la branche
   précédente), même trajet. Comparer le nombre de liens et le retour de l'iPhone.
8. Refaire le trajet avec le réglage de P1 actif (auxiliaire installé) : attendu, un ou deux liens au lieu
   de vingt, aucune faute.

### P3. La console de l'iPhone : voir les « strike » (30 min, avec un Mac)

C'est le seul essai qui confirme ou infirme le mécanisme sous iOS 26 (`moteur-reconnexion.md` §14.1).

1. Sur l'iPhone, installer le profil de journalisation **Bluetooth** d'Apple
   (<https://developer.apple.com/bug-reporting/profiles-and-logs/>, depuis Safari sur l'iPhone ; Réglages ›
   Général › VPN et gestion de l'appareil › installer), puis **redémarrer l'iPhone**. Le profil expire seul
   après quelques jours ; le retirer au même endroit après l'essai.
2. Brancher l'iPhone au Mac par câble, « Faire confiance ». Ouvrir **Console.app**, choisir l'iPhone dans la
   colonne de gauche, menu Action › cocher « Inclure les messages d'information » et « de débogage »,
   cliquer « Démarrer ».
3. Dans le champ de recherche : `strike`, puis ajouter un second filtre `built-in service`. Filtre plus
   large si rien ne sort : processus `bluetoothd`, texte contenant le nom Bluetooth de l'ordinateur.
4. Provoquer des liens morts avant service, côté ordinateur **sans** la règle des fautes
   (`LANDLINE_LE_STRIKES=0`), en limite de portée ou avec un sac de blindage / une boîte métallique ouverte
   et refermée (le montage de 2015) : trois liens de 1 à 2 s en moins de 15 s.
5. Ce qu'on cherche, mot pour mot dans la source de 2015 :

   ```
   BTLEServer: Peripheral "…" did not use any built-in service, strike #1
   BTLEServer: Peripheral "…" did not use any built-in service, strike #2
   BTLEServer: Peripheral "…" did not use any built-in service, strike #3
   ```

   et autour : `is now subscribed to characteristic "ANCS Data Source"`, `Connecting peripheral`,
   `Connection timed out`.
6. À noter : la ligne existe-t-elle encore, sous quel libellé ; le seuil (3 ?) ; l'écart de temps au-delà
   duquel deux fautes ne se cumulent plus (refaire avec 20 s, 60 s, 5 min entre deux liens) ; **un lien
   coupé après la seule lecture de la batterie par BlueZ est-il compté** (lien de moins d'une seconde) ; ce
   qui remet le compte à zéro (lien abonné, verrouillage, Réglages › Bluetooth ouvert) ; ce qui se passe
   après la troisième (plus aucun `Connecting peripheral` ?).
7. Refaire l'étape 4 **avec** la règle (défaut) : attendu, jamais plus de `strike #2`.
8. Sans Mac, deux pis-aller, **non vérifiés** : `idevicesyslog` (paquet `libimobiledevice-utils`, iPhone
   branché à l'ordinateur) filtré par `grep -i -E 'strike|built-in service'` — il ne relaie qu'une partie du
   journal ; ou un *sysdiagnose* (les deux boutons de volume et le bouton latéral 1,5 s, puis Réglages ›
   Confidentialité et sécurité › Analyse et améliorations › Données d'analyse › `sysdiagnose_…`), dont
   l'archive de journaux ne se lit qu'avec `log show` sur un Mac mais dont la trace HCI de l'iPhone
   (`.pklg`) s'ouvre avec Wireshark : elle donne les paramètres de connexion réels et le sort de nos
   demandes.

### Tableau à remplir

| Essai | Demande acceptée (o/n), valeurs obtenues | Coupures LE : recherche classique (n/10) | limite de portée | Liens LE au retour | Fautes comptées | Lien → abonnement / → service (ms) | iPhone revenu seul | Console : `strike` vu, seuil, fenêtre |
|---|---|---|---|---|---|---|---|---|
| P1 avec | | | | sans objet | sans objet | sans objet | sans objet | sans objet |
| P1 sans | sans objet | | | sans objet | sans objet | sans objet | sans objet | sans objet |
| P2 | sans objet | sans objet | sans objet | | | | | sans objet |
| P2 contre-essai | sans objet | sans objet | sans objet | | | | | sans objet |
| P2 + réglage P1 | | sans objet | sans objet | | | | | sans objet |
| P3 | sans objet | sans objet | sans objet | sans objet | | sans objet | | |

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

### M. Veille de l'ordinateur, iPhone à côté (branche `moteur-le-apres-veille`)

À faire en premier : c'est l'incident du 17 novembre, et il tranche trois questions à la fois. Lire
`moteur-reconnexion.md`, section 11.

Préparation : quatre terminaux comme en section 2, préfixe `M1`. iPhone déverrouillé, posé à côté. **Point
de départ obligatoire : le LE est monté** (`LE=b true` dans le relevé des liens de T3). S'il ne l'est pas, couper puis
rallumer le Bluetooth sur l'iPhone et attendre `Notification mirroring is active` ; noter l'heure.

T4, à lancer **avant** la mise en veille et à laisser tourner pendant tout le passage :

```sh
sudo btmon -w essais/M1.btsnoop | tee essais/M1-btmon.log
```

Déroulé :

1. Noter t0, puis mettre l'ordinateur en veille (`systemctl suspend`, ou le capot). Attendre **2 minutes**,
   sans toucher à l'iPhone.
2. Réveiller l'ordinateur. Noter l'heure du réveil (t1). Ne rien toucher pendant **3 minutes**.
3. Relever dans le journal du moteur (T1), dans l'ordre attendu :

   | Ligne | Attendu |
   |---|---|
   | `Device1 disconnected … Reason.Suspend` | à t1 |
   | `the computer slept for Ns; registering the advertisement again…` | à t1 + 1 s au plus, N ≈ 120 |
   | `no link to the iPhone; leaving BR/EDR alone for up to 10s…` | **sans** ligne `asking BlueZ for the BR/EDR link` avant elle |
   | `soliciting ANCS for 180s, advertising every 20-20 ms` | dans la seconde qui suit le réveil |
   | `the LE link came first, Ns after every link was down` | le résultat espéré |
   | `LE link up with nothing else; leaving BR/EDR alone 5s more…` | juste après |
   | `notification link connected` | |

4. Dans `M1-btmon.log`, après t1, chercher et recopier dans le tableau :

   | Question | Quoi chercher | Lecture |
   |---|---|---|
   | L'annonce est-elle remise en ondes ? | `LE Set Extended Advertising Enable` (ou `LE Set Advertise Enable`) avec `Enable: 0x01`, après la reprise | absent : l'annonce n'est **pas** en ondes alors que BlueZ la croit active → cause trouvée (11.6, ligne 2) |
   | À quel intervalle ? | `LE Set Extended Advertising Parameters`, `Min advertising interval` / `Max` | 20.000 msec attendu pendant 60 s, puis 152.500 |
   | Connectable ? | même commande, `Properties` : `Connectable` | doit y être |
   | Avec quelle adresse ? | `Own address type` et, s'il y en a un, `LE Set Advertising Set Random Address` | `Public` attendu. Une adresse aléatoire signifierait que l'iPhone ne peut pas reconnaître l'accessoire appairé |
   | Qui rouvre BR/EDR ? | premier `Connect Request` (événement, **entrant** : l'iPhone) ou `Create Connection` (commande, **sortant** : BlueZ) après t1 | entrant : c'est l'iPhone, rien à faire de notre côté ; sortant sans ligne `asking BlueZ` dans notre journal : chercher qui (politique, obexd) |
   | L'iPhone répond-il à l'annonce ? | `LE Enhanced Connection Complete` avec `Role: Peripheral` | adresse du pair, `Peer address type`, et s'il y a une ligne d'adresse d'identité résolue |
   | Le lien LE tient-il ? | `Disconnect Complete` sur ce lien, `Reason` | `Connection Timeout (0x08)` dans les secondes qui suivent un `Create Connection` BR/EDR : c'est 11.3 |

5. Relever dans le journal les lignes `notification link <état>: <cause>`. Attendu si le LE ne revient
   pas : `waiting: after_resume`, puis `paused: after_resume` 180 s après la perte du LE. Le même état est
   publié aux clients (`le_state`, `le_cause` de `bt_connection_changed`).

Trois passages. Puis un passage avec chacun des interrupteurs, pour comparer :
`LANDLINE_RESUME_WATCH=0` (comportement du 17 novembre, la course en moins),
`LANDLINE_LE_FIRST_STRICT=0` (la course revient : `asking BlueZ` avant `leaving BR/EDR alone`).

Variante M-verrouillé : même déroulé, iPhone verrouillé écran éteint depuis 1 min avant la veille.
Variante M-long : veille de 30 min (l'iPhone a eu le temps de classer l'accessoire comme absent).

### N. L'appareil à adresse privée (piste de l'adresse non résolue)

But : savoir si un appareil LE inconnu qui se connecte à notre annonce est l'iPhone.

1. `sudo btmon -w essais/N1.btsnoop | tee essais/N1-btmon.log` dans T4, laissé tourner **toute la séance**
   (il ne coûte rien). Le moteur écrit de lui-même la ligne à chercher :
   `[WARN] an LE device BlueZ holds no pairing for (/org/bluez/hci0/dev_XX_…) connected and left`.
2. À cette heure-là dans `N1-btmon.log`, lire le `LE Enhanced Connection Complete` :

   | Champ | Si c'est l'iPhone non résolu | Si c'est un autre appareil |
   |---|---|---|
   | `Role` | `Peripheral (0x01)` — il a répondu à notre annonce | `Central (0x00)` — c'est l'ordinateur qui s'est connecté (clavier, souris, manette) |
   | `Peer address` | résolvable, sans ligne d'identité résolue | — |
   | suite | une demande de sécurité qui échoue (`Pairing Failed`, `PIN or Key Missing`, ou `Encryption Change` en erreur) puis `Disconnect Complete` | échanges GATT ordinaires, ou rien |

3. Juste après une telle ligne, sur l'iPhone : Réglages > Bluetooth. L'ordinateur y figure-t-il « Connecté »
   un instant ? Le noter.
4. Sans root, on peut déjà comparer avec un lien sain : pendant que le LE est monté normalement,
   `busctl --system tree org.bluez | grep dev_` ne doit montrer **aucun** appareil à adresse `4x:`–`7x:`
   en plus de l'iPhone.
5. Si l'hypothèse se confirme (rôle `Peripheral`, adresse non résolue, échec de sécurité) : relancer
   bluetoothd (`sudo systemctl restart bluetooth`, le moteur survit, 10.4) et voir si le LE revient dans les
   10 s. Si oui, les clés n'étaient plus chargées : le noter, avec `sudo ls /var/lib/bluetooth/*/` et la
   présence d'une section `[IdentityResolvingKey]` dans le fichier `info` de l'iPhone (ne pas recopier la
   clé).

### O. Le lien LE tient-il maintenant quand il revient en premier ?

Reprend la séquence du 16 novembre à 22:25 (cinq liens LE perdus en deux minutes).

1. LE et BR/EDR montés. `bluetoothctl disconnect $IPHONE`. Noter t0.
2. Attendu : `leaving BR/EDR alone`, `the LE link came first`, `LE link up with nothing else; leaving
   BR/EDR alone 5s more`, puis 5 s plus tard `asking BlueZ for the BR/EDR link`, et **aucune** ligne
   `Bearer.LE1 disconnected` entre les deux ni dans les 10 s qui suivent.
3. Dix passages (c'était 4 échecs sur 5). Compter les liens LE perdus dans les 10 s.
4. Les mêmes dix avec `LANDLINE_LE_SETTLE_SECONDS=0`. Si le compte ne change pas, l'hypothèse de 11.3 est
   fausse et les 5 s sont à retirer.
5. Relever T1 à T4 comme en A : le coût attendu est +5 s sur les messages et les appels.

Variante O-portée : sortir de portée 1 min puis revenir (`Reason.Timeout`). Ici la politique de BlueZ
rappelle l'iPhone 1 s après la perte, quoi que fasse le moteur ; compter de même. Si les liens LE tombent
dans ce cas seulement, essayer une séance avec, dans `/etc/bluetooth/main.conf` (root, puis
`sudo systemctl restart bluetooth`) :

```ini
[Policy]
ReconnectAttempts=0
```

et la retirer ensuite : elle vaut pour tous les appareils (casques compris).

### P. iPhone « coincé » : qu'est-ce qui le décoince, en dehors de lui ?

À faire **quand l'état du 17 novembre se présente de lui-même** (`le_state=paused`, `phone_silent` ou
`after_resume`, BR/EDR monté) — ne pas basculer le Bluetooth de l'iPhone avant d'avoir fini. `btmon` lancé.
Essayer dans cet ordre, 2 min d'attente entre deux, et noter après chacun si le LE revient :

| # | Action | Ce que cela distingue |
|---|---|---|
| 1 | rien : relever dans `btmon` si l'annonce est en ondes (question 1 du scénario M) | annonce absente → notre côté |
| 2 | verrouiller puis déverrouiller l'iPhone | l'iPhone ne balaie qu'écran allumé |
| 3 | iPhone : Réglages > Bluetooth, laisser l'écran ouvert 30 s | ce panneau force un balayage |
| 4 | `bluetoothctl disconnect $IPHONE` | (déjà vu sans effet le 17 à 12:23) |
| 5 | relancer le moteur | annonce réenregistrée par un nouveau client D-Bus |
| 6 | `sudo systemctl restart bluetooth` | état de BlueZ et du contrôleur (liste de résolution, annonces) |
| 7 | veille de l'ordinateur 2 min | — |
| 8 | iPhone : mode avion 10 s | radio de l'iPhone, sans toucher au réglage Bluetooth |
| 9 | iPhone : Bluetooth coupé puis rallumé | la référence : < 10 s le 16 novembre |

Le premier numéro qui rétablit le LE dit de quel côté est l'état perdu : 1, 5 ou 6 → le nôtre, et le moteur
peut le faire seul ; 2, 3, 8, 9 seulement → celui de l'iPhone, et il ne reste qu'à le dire
(`le_detail`). Garder le `btsnoop` complet.

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

- **Le LE ne revient pas après une veille (M)** : d'abord la question 1 du tableau `btmon`. Annonce absente
  des ondes : c'est notre côté, garder le `btsnoop` et les lignes du noyau (`journalctl -k`) autour du
  réveil. Annonce en ondes, connectable, adresse publique, et aucun `LE Enhanced Connection Complete` :
  l'iPhone ne répond pas ; passer au scénario P sans toucher à son Bluetooth. Pas de ligne `the computer
  slept` : `CLOCK_BOOTTIME` n'a pas avancé ou `LANDLINE_RESUME_WATCH=0`.
- **`asking BlueZ for the BR/EDR link` avant `leaving BR/EDR alone`** : la course de 11.2 est revenue ;
  noter ce qui était ouvert (`map_open`, `pbap_open`) dans la seconde qui précède.
- **`Bearer.LE1 disconnected` dans les secondes qui suivent `the LE link came first`** : lire dans `btmon`
  ce que la radio faisait (un `Create Connection` BR/EDR, de qui) ; c'est le scénario O.

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

## 8. Mode sobre : essais E1 à E9 (branche `moteur-sobre`)

Ce sont les essais de `iphone-link-study/11-classique-a-la-demande.md`, section 6, rédigés pour être
déroulés. Ils n'ont rien à voir avec le scénario « E. Sortie de portée » de la section 4 : ici « E » veut
dire « essai ». À lire avant : `moteur-reconnexion.md` §12 et `moteur-api.md` section 13.

**Ordre conseillé** : E1, E2, E7 (une heure et demie : ils disent si le mode sobre tient debout), puis E3,
E4, E5, E6, puis E9 sur une journée et E8 sur quatre nuits. Si E1 ou E2 échoue, s'arrêter et repasser en mode
complet (`LANDLINE_LINK_MODE=full`).

### Préparation commune

Section 2 (prérequis, un seul démon, `IPHONE`, `P`, dossier `essais/`), avec ces différences.

**T1 — le démon, en mode sobre**

```sh
cd ~/DEVELOPEMENT/iphone-link
LANDLINE_LINK_MODE=lean engine/build/local/landlined 2>&1 | tee essais/E1-journal.log
```

À vérifier dans les dix premières secondes : `bluetooth: link mode lean (LE kept, the classic link on
demand)`. Si la ligne dit `link mode full … instead of lean: notifications_off`, c'est que les notifications
n'ont encore jamais été prêtes sur cet appairage avec ce moteur : attendre `Notification mirroring is
active`, le mode passe alors à `lean` tout seul (et le restera aux lancements suivants).

**T2 — ce que voit un client.** `scripts/chrono-moteur.py` ne connaît pas les nouveaux champs. Créer une
fois `essais/suivi.py` :

```python
#!/usr/bin/env python3
"""Suit le démon en lecture seule : champs du mode sobre, événements d'appel et d'envoi."""
import json, os, socket, sys, time

SUIVIS = ("le_connected", "classic_connected", "ancs_ready", "map_open", "pbap_open",
          "link_mode_effective", "link_mode_reason", "classic_state", "classic_leases",
          "classic_opened_by", "messages", "contacts", "calls_available", "last_sync")
EVENEMENTS = ("bt_send_progress", "bt_send_result", "bt_incoming_call", "bt_call_progress",
              "bt_call_result", "bt_calls", "bt_presence", "bt_notification_removed")

s = socket.socket(socket.AF_UNIX)
s.connect(os.environ["XDG_RUNTIME_DIR"] + "/landline/landlined.sock")
s.sendall(b'{"command":"subscribe"}\n{"command":"bt_connection"}\n')
avant, tampon = {}, b""
sortie = open(sys.argv[1], "a") if len(sys.argv) > 1 else None

def dire(texte):
    ligne = time.strftime("%H:%M:%S") + ".%03d " % (time.time() % 1 * 1000) + texte
    print(ligne, flush=True)
    if sortie:
        sortie.write(ligne + "\n")
        sortie.flush()

while True:
    bloc = s.recv(65536)
    if not bloc:
        break
    tampon += bloc
    while b"\n" in tampon:
        brut, tampon = tampon.split(b"\n", 1)
        try:
            objet = json.loads(brut)
        except ValueError:
            continue
        nom = objet.get("command")
        if nom == "bt_connection_changed":
            for cle in SUIVIS:
                if cle in objet and objet[cle] != avant.get(cle):
                    dire("%s=%s" % (cle, json.dumps(objet[cle])))
                    avant[cle] = objet[cle]
        elif nom == "bt_notification":
            dire("bt_notification app=%s uid=%s cat=%s titre=%r sous-titre=%r texte=%d octets" % (
                objet.get("app_id"), objet.get("uid"), objet.get("category"), objet.get("title"),
                objet.get("subtitle"), len((objet.get("body") or "").encode())))
        elif nom == "bt_message":
            dire("bt_message fil=%s sortant=%s texte=%d octets" % (
                objet.get("thread"), objet.get("outgoing"), len((objet.get("body") or "").encode())))
        elif nom in EVENEMENTS:
            dire(json.dumps(objet, ensure_ascii=False))
```

```sh
python3 essais/suivi.py essais/E1-client.log
```

**T5 — envoyer une commande.** Créer une fois `essais/ll.py` :

```python
#!/usr/bin/env python3
"""ll.py '<json>' [secondes] : envoie une commande au démon et affiche ce qu'il répond pendant N secondes."""
import os, socket, sys

s = socket.socket(socket.AF_UNIX)
s.connect(os.environ["XDG_RUNTIME_DIR"] + "/landline/landlined.sock")
s.sendall((sys.argv[1] + "\n").encode())
s.settimeout(float(sys.argv[2]) if len(sys.argv) > 2 else 2)
try:
    while True:
        bloc = s.recv(65536)
        if not bloc:
            break
        sys.stdout.write(bloc.decode(errors="replace"))
        sys.stdout.flush()
except socket.timeout:
    pass
```

```sh
alias ll='python3 essais/ll.py'
ll '{"command":"bt_connection"}' | python3 -m json.tool | grep -E 'link_mode|classic_|"messages"|"contacts"|calls_available|_sync'
```

**T3** : la boucle `BREDR= LE=` de la section 2 (`essais/E1-liens.log`). **C'est le relevé de référence
de toute la série** : il dit, à 200 ms près, quand chaque lien monte et tombe.

**T4** : `sudo btmon -w essais/E1.btsnoop | tee essais/E1-btmon.log`, pour E1, E2 et E7. Sert à dire **qui**
ouvre un lien BR/EDR : `Create Connection` (commande, sortant : l'ordinateur) ou `Connect Request`
(événement, entrant : l'iPhone).

État de départ de chaque essai, sauf mention : `le_connected=true`, `classic_connected=false`,
`classic_state="idle"`, `messages="on_demand"`, `calls_available="on_demand"`. Si `messages` vaut `unknown`,
attendre la première relève (elle suit le retour du LE) ; s'il vaut `forbidden`, voir E6.

---

### E1. Le mode sobre au repos — 1 h

**Décide** : la tenue du LE seul ; si et quand l'iPhone rouvre le classique de lui-même ; les durées du
garde-fou (120 s, 60 s, 15 min, 30 min).

Ne rien demander depuis l'ordinateur pendant tout l'essai (ni envoi, ni menu de l'extension : la désactiver).

| Phase | Durée | Geste |
|---|---|---|
| 1 | 20 min | iPhone déverrouillé puis laissé se verrouiller, posé à côté |
| 2 | 20 min | iPhone verrouillé, en poche, dans la pièce |
| 3 | 5 min | un tiers appelle l'iPhone ; laisser sonner, ne pas décrocher |
| 4 | 5 min | lancer une musique sur l'iPhone (haut-parleur de l'iPhone), 2 min, puis l'arrêter |
| 5 | 5 min | Bluetooth de l'iPhone coupé 30 s, puis rallumé |
| 6 | 5 min | sortir de portée 2 min, revenir |

À relever :

| Quoi | Où | Attendu |
|---|---|---|
| coupures du LE | `E1-liens.log`, passages `LE=b false` | ≤ 2 par heure hors phases 5 et 6, chacune rétablie en < 5 s |
| apparitions du BR/EDR | `E1-liens.log`, `BREDR=b true` ; dans le journal `the iPhone opened the BR/EDR link itself` | noter l'heure et la phase de chacune |
| qui l'a ouvert | `E1-btmon.log` à la même heure : `Connect Request` (iPhone) ou `Create Connection` (ordinateur) | l'iPhone. Un `Create Connection` sans ligne `asking BlueZ` dans notre journal est une ouverture par BlueZ ou obexd : à noter, c'est un défaut |
| ce que l'iPhone ouvre dessus | journal : `hands-free` ; `pactl list cards short` (carte `bluez`) ; `bt_calls` | à noter |
| fermeture | journal : `BR/EDR link closed on purpose (nothing needs it)` | 120 s après la dernière activité ; jamais pendant la sonnerie ni pendant la musique |
| garde-fou | `link_mode_reason="phone_keeps_link"` dans `E1-client.log` | au plus une fois dans l'heure |
| relèves | journal : `inbox listed (…)` | une à l'ouverture de chaque lien ; **aucune** entre deux ouvertures |
| lignes du mode complet | `grep -E 'leaving BR/EDR alone\|dropping every link\|link on purpose, once' essais/E1-journal.log` | rien |

**Réussite** : LE tenu (critère ci-dessus) ; tout lien ouvert par l'iPhone adopté puis refermé ; pas de
boucle. **Échec** : LE qui tombe plus de deux fois par heure au repos → noter les motifs (`Reason.…`) ;
`phone_keeps_link` plus d'une fois → l'iPhone tient au lien, noter après quel geste.

### E2. Vingt ouvertures et fermetures — 20 min

**Décide** : les délais ; si l'ouverture fait tomber le LE ; si la session ouverte par obexd reste sans audio.

Le bail `reading` ouvre le lien pour des données, puis on le rend :

```sh
for i in $(seq 1 20); do
  date +%H:%M:%S.%3N
  ll '{"command":"bt_lease","name":"reading","held":true}' 12 >/dev/null
  ll '{"command":"bt_lease","name":"reading","held":false}' 45 >/dev/null
done 2>&1 | tee essais/E2-cycles.log
```

Chaque tour : ouverture, 12 s ouvert, bail rendu, 20 s de maintien, fermeture, repos. Dix tours iPhone
déverrouillé, dix iPhone verrouillé depuis plus d'une minute.

| Mesure | Où | Réussite |
|---|---|---|
| temps d'ouverture : de la ligne `date` à `map_open=true` | `E2-client.log` | médiane ≤ 4 s, pire cas ≤ 10 s |
| le LE tient pendant et après | `E2-liens.log` : pas de `LE=b false` | intact dans ≥ 18 tours sur 20 |
| aucun profil audio | journal : pas de `hands-free` ; pas de carte `bluez` dans `pactl list cards short` pendant un tour | aucun |
| fermeture | `classic_state` : `open` → `lingering` (20 s) → `closing` → `idle` ; `BREDR=b false` | dans les 2 s qui suivent `closing` |
| le lien tombe-t-il seul après `RemoveSession` ? | écart entre `map_open=false` et `BREDR=b false` | à noter (attendu : fermé par notre `Disconnect` aussitôt) |

**Échec** : LE tombé dans plus de deux tours → relever dans `btmon` le motif de la coupure LE et sa position
par rapport au `Create Connection` ; c'est le risque principal du mode sobre.

### E3. Ce que dit la notification de Messages — 30 min

**Décide** : l'anti-rebond (2 s), les relances (+3 s, +10 s), ce que vaut la notification seule.

Un tiers envoie, dans l'ordre, lien fermé à chaque fois (attendre `classic_state="idle"` entre deux) :

| # | Message | Réglage de l'iPhone |
|---|---|---|
| 1 | court | aperçus « Toujours », déverrouillé |
| 2 | court | verrouillé |
| 3 | long, plus de 1 100 octets | verrouillé |
| 4 | trois lignes | verrouillé |
| 5 | une image | verrouillé |
| 6 | dans un groupe nommé | verrouillé |
| 7 | dans un groupe sans nom | verrouillé |
| 8 | d'un numéro absent du carnet | verrouillé |
| 9 | court | aperçus « Si déverrouillé », verrouillé |
| 10 | court | aperçus « Jamais » |
| 11 | court | Messages ouvert à l'écran de l'iPhone, sur la conversation |
| 12 | (aucun) lire sur l'iPhone le message 2 resté non lu | — |

Pour chacun, relever dans `E3-client.log` et `E3-journal.log` :

| Colonne | Où |
|---|---|
| notification reçue ? titre, sous-titre, longueur du texte | ligne `bt_notification app=com.apple.MobileSMS` |
| t0 : heure de la notification | même ligne |
| t1 : ouverture | `classic_state="opening"` (attendu t0 + 2 s) |
| t2 : message complet | ligne `bt_message` |
| relève qui l'a ramené | journal : `inbox listed (session)`, `(retry: ancs)` |
| retrait | `bt_notification_removed` (cas 12) et `message sync asked (ancs_removed)` |

**Réussite** : tableau rempli ; t2 − t0 ≤ 7 s ; le message est dans la **première** relève (`session`) dans
au moins 9 cas sur 10 — sinon l'écart notification → MAP dépasse 2 s et l'anti-rebond est à allonger. Cas 10 :
le texte de la notification est générique et `bt_message` porte le vrai texte. Cas 11 : pas de notification
attendue ; le message doit arriver à la relève suivante (au plus tard une heure, ou au prochain message).
Cas 12 : pas d'ouverture du lien (écart 11 de `moteur-sobre-ecarts.md`).

### E4. Appel entrant, lien fermé — 20 min

**Décide** : la séquence d'appel entrant. **À faire avant de se fier à la carte d'appel de l'extension.**
Contrôle d'appel activé (`calls_available` vaut `on_demand`), `call_audio` sur `phone`.

Relever l'`uid` dans la ligne `bt_incoming_call` de T2 et le reporter dans les commandes.

| # | Geste | Commande | À relever | Réussite |
|---|---|---|---|---|
| 1 | laisser sonner | — | t0 = `bt_incoming_call` `ringing` / `connecting` ; `classic_state="opening"` ; t1 = `answer_here":"ready"` avec `path` | t1 − t0 ≤ 9 s ; l'appel est vu par la passerelle (`bt_calls`, état `incoming`) |
| 2 | refuser avant `ready` | `ll '{"command":"bt_call_action","action":"decline","uid":UID}' 5` | `bt_call_result` : `success`, `via` ; l'iPhone cesse-t-il de sonner ? | `via:"ancs"`, sonnerie arrêtée, `ended` dans la seconde |
| 3 | décrocher sur l'iPhone avant `ready` | `ll '{"command":"bt_call_action","action":"answer","uid":UID,"audio":"phone"}' 5` | où sort le son ; `bt_calls` quand la passerelle arrive ; pas de `ended` | appel pris, son sur l'iPhone, l'appel apparaît dans `bt_calls` (état `active`) |
| 4 | suite de 3 : basculer le son ici | `ll '{"command":"bt_set_call_audio","route":"here","persist":false}' 5` | le son passe-t-il sur l'ordinateur ; l'iPhone l'avait-il déjà basculé seul à l'arrivée du mains-libres ? | bascule faite à la demande, **pas** avant |
| 5 | décrocher ici après `ready` | `ll '{"command":"bt_call_action","action":"answer","uid":UID,"audio":"here"}' 5` | `via` ; son | `via:"hfp"`, son ici |
| 6 | décrocher ici **avant** `ready` | même commande, tout de suite | `message` | refus clair : « This computer is not connected for the call yet. » ; l'appel sonne toujours |
| 7 | laisser sonner jusqu'au bout | — | `ended` ; relecture du journal d'appels (`pulled N recent calls`) ; fermeture | `ended` à l'appel manqué ; lien fermé ≤ 40 s après |
| 8 | l'appelant raccroche à 3 s | — | idem | `ended` ; pas de nouvelle tentative d'ouverture après |
| 9 | appel reçu lien de données ouvert (envoyer un message 5 s avant) | — | journal : `dropping the data link for hands-free` | recyclage immédiat, `ready` ≤ 9 s |

Après chaque appel terminé : `classic_leases` se vide, `classic_state` passe à `lingering` (15 s) puis
`idle`. **Échec bloquant** : 2 ou 3 sans effet sur l'iPhone → les actions ANCS ne pilotent pas l'appel, la
carte n'a rien à offrir avant le mains-libres ; le dire avant d'aller plus loin.

### E5. Composer à froid — 10 min

**Décide** : le délai, le libellé d'attente, la garde « passerelle prête ».

```sh
ll '{"command":"bt_call_dial","number":"NUMERO","audio":"here","operation_id":"e5-1"}' 45
```

| # | Variante | Attendu |
|---|---|---|
| 1 | lien fermé | `bt_call_progress` `connecting` → `hands_free` → `dialing`, puis `bt_call_result` `success:true`, puis `bt_calls` (`dialing`, `alerting`) ; total ≤ 10 s ; appel établi, son ici |
| 2 | relancer dans la minute (lien encore là) | `dialing` seul, < 1 s |
| 3 | annuler : `ll '{"command":"bt_call_cancel","operation_id":"e5-3"}'` 2 s après le `bt_call_dial` | `bt_call_result` `reason:"cancelled"` ; aucun appel sur l'iPhone ; lien refermé après 15 s |
| 4 | deux `bt_call_dial` à 1 s d'intervalle | le second : `reason:"busy"` |
| 5 | `"audio":"phone"` | noter si l'appel a du son sur l'iPhone ou s'il est muet |
| 6 | Bluetooth de l'iPhone coupé | `reason:"no_link"` à 20 s, message « Couldn’t reach the iPhone. » |
| 7 | pendant l'attente de 1, dans un autre terminal : `ll '{"command":"bt_status"}'` | réponse immédiate : la boucle n'est pas retenue |

Si 1 échoue en `no_hands_free` alors que `pactl list cards short` montre la carte de l'iPhone : relancer le
démon avec `LANDLINE_GATEWAY_STRICT=0` et refaire ; noter le résultat (hypothèse 9 de
`moteur-reconnexion.md` §12.6). Si 5 donne un appel muet, le choix « iPhone » est à retirer de la carte de
composition.

### E6. Les interrupteurs de l'iPhone — 10 min

**Décide** : le parcours d'installation en mode sobre.

1. Lien fermé. Sur l'iPhone : Réglages › Bluetooth › ⓘ de l'ordinateur. Les bascules « Afficher les
   notifications » et « Synchroniser les contacts » sont-elles visibles ? Noter.
2. Prendre le bail d'installation : `ll '{"command":"bt_lease","name":"setup","held":true}'`. Attendre
   `classic_state="open"`. Les bascules sont-elles visibles maintenant ?
3. Couper « Afficher les notifications ». Relever le délai jusqu'à `messages="forbidden"` (les essais
   rapprochés d'un profil refusé tournent toutes les 2 s tant que le lien est monté).
4. Rendre le bail (`"held":false`). Le lien se ferme ; `messages` doit **rester** `forbidden`, et
   `profile_reason` nommer l'interrupteur.
5. Reprendre le bail, rallumer la bascule : `messages="ready"` en quelques secondes ; rendre le bail :
   `messages="on_demand"`.
6. Relancer le démon : `messages` doit valoir `on_demand` avant toute ouverture (verdict relu sur disque).

**Réussite** : 3 à 6 conformes. Le point 1 n'est qu'une observation : si les bascules sont invisibles lien
fermé, l'aide de l'extension doit tenir le bail `setup` (c'est prévu).

### E7. Veille de l'ordinateur, dix fois — 30 min

**Décide** : la promesse principale. À comparer au scénario M (section 4) et aux relevés du 17 novembre.

Pour chaque passage : lien classique fermé (`classic_state="idle"`), LE monté, puis `systemctl suspend`,
30 s de veille, réveil, 2 min sans toucher. Cinq passages iPhone déverrouillé, cinq verrouillé. Deux
passages supplémentaires **lien ouvert** au moment de la veille (prendre le bail `reading` 5 s avant).

| Mesure | Où | Réussite |
|---|---|---|
| délai réveil → LE | `E7-liens.log` : `LE=b true` après le réveil | ≤ 5 s dans 9 passages sur 10 |
| BlueZ remonte-t-il le BR/EDR ? | `E7-liens.log` : `BREDR=b true` ; `E7-btmon.log` : `Create Connection` sortant ; `journalctl -u bluetooth` : `br-connection-busy`, `reconnect_timeout` | jamais |
| relève au retour | journal : `sync wanted (the iPhone is back)` puis une ouverture | une, sauf si la précédente a moins de 10 min |
| passages lien ouvert : fermeture avant la veille | journal : `sleep: logind says the computer is about to sleep`, puis `classic link closing`, **avant** `Reason.Suspend` | fermé avant la veille ; au réveil comme les autres |
| inhibiteur | `systemd-inhibit --list` pendant que le démon tourne | une ligne « Landline », mode `delay` |

**Échec** : le BR/EDR revient au réveil sans ligne `asking BlueZ` chez nous → regarder si un profil audio
était connecté à la veille (passages lien ouvert) ; si l'inhibiteur manque, la fermeture n'a pas eu le temps
de se faire (hypothèse 8). Dans les deux cas noter et comparer à `LANDLINE_SLEEP_CLOSE=0`.

### E8. Batterie de l'iPhone — 4 nuits

**Décide** : ce que l'on gagne vraiment. Une configuration par nuit, 8 h, iPhone verrouillé, écran éteint,
Wi-Fi allumé, même pièce, sans chargeur, parti de 100 % ; ordinateur éveillé (pas de veille automatique).

| Nuit | Configuration | Lancement du démon |
|---|---|---|
| A | deux liens, relève à 15 s (l'ancien moteur) | `LANDLINE_LINK_MODE=full LANDLINE_MESSAGE_POLL_SECONDS=15 engine/build/local/landlined` |
| B | deux liens, relève horaire | `LANDLINE_LINK_MODE=full engine/build/local/landlined` |
| C | mode sobre, sans geste | `LANDLINE_LINK_MODE=lean engine/build/local/landlined` |
| D | témoin : moteur arrêté, Bluetooth de l'iPhone allumé | — |

Relever le niveau au coucher et au lever (Réglages › Batterie) ; pour A à C, la courbe est aussi dans le
journal (`battery`, pas de 1 point). Calculer les points perdus par heure.

| Différence | Isole | Pour conclure |
|---|---|---|
| A − B | le coût de la relève à 15 s (étape M1) | écart > 0,25 point par heure |
| B − C | le coût du lien BR/EDR tenu (étape M2) | idem |
| C − D | le coût du LE et d'ANCS | attendu à peine mesurable |

La précision est d'un point sur 8 h (0,12 point par heure) : refaire A une seconde fois pour connaître le
bruit d'une nuit à l'autre. Une nuit sans message ne mesure pas le coût des ouvertures : refaire C avec
vingt messages envoyés par un tiers, ou le lire dans E9. Vérifier au matin, dans le journal de C, le nombre
d'ouvertures (`classic link opening`) : 8 attendues (les relèves horaires), plus une par message.

### E9. Une journée d'usage — 1 jour

**Décide** : si le mode sobre reste le défaut. Démon lancé sans `LANDLINE_LINK_MODE` (le défaut est `lean`),
extension à jour, usage normal. Le soir, avec `J` = le journal du démon de la journée :

```sh
grep -c 'classic link opening' "$J"                                          # ouvertures
grep 'classic link opening' "$J" | grep -o 'leases: [^;)]*' | sort | uniq -c  # leur cause
grep -c 'the iPhone opened the BR/EDR link itself' "$J"                      # ouvertures par l'iPhone
grep -c 'not opened for it' "$J"                                             # plafond, ou attente après échec
grep -c 'instead of lean' "$J"                                               # replis sur le mode complet
```

Temps cumulé lien monté et coupures du LE : dans le relevé `BREDR= LE=` de T3 s'il a tourné toute la
journée ; sinon additionner les intervalles entre `classic link opening` et `classic link idle`.

| Mesure | Réussite |
|---|---|
| ouvertures dans la journée | < 60 |
| temps cumulé BR/EDR monté | < 30 min |
| messages manqués ou vus avec plus d'une heure de retard (comparer à l'iPhone le soir) | aucun |
| envois : délai ressenti à froid | 3 à 5 s ; aucun échec « Couldn’t reach the iPhone to send. » iPhone présent |
| appels reçus : « Answer here » disponible avant la fin de la sonnerie | dans la majorité des appels |
| replis `phone_keeps_link` | au plus un |

Noter aussi tout moment où l'on a attendu sans savoir pourquoi : c'est un défaut d'état publié, pas un
détail.

### Tableau de synthèse à remplir

| Essai | Date | Résultat | Chiffre clé | Suite |
|---|---|---|---|---|
| E1 | | | coupures LE par heure : … ; réouvertures par l'iPhone : … | |
| E2 | | | ouverture médiane : … s, pire : … s ; LE intact : …/20 | |
| E3 | | | notification → message : … s ; relève `session` suffisante : …/10 | |
| E4 | | | sonnerie → `ready` : … s ; refus ANCS : oui / non ; décroché ANCS : oui / non | |
| E5 | | | composition à froid : … s | |
| E6 | | | bascules visibles lien fermé : oui / non | |
| E7 | | | réveil → LE : … s ; BR/EDR remonté par BlueZ : …/10 | |
| E8 | | | A : … ; B : … ; C : … ; D : … points par heure | |
| E9 | | | ouvertures : … ; minutes BR/EDR : … | |

## 9. Le son des appels : essais S1 à S4 (branche `moteur-son-appels`)

À dérouler avec l'iPhone et un second téléphone, en mode sobre, lien classique fermé au départ. Vingt
minutes. À lire avant : `moteur-quotidien.md` §7. Préparation : celle de la section 8 (T1 le démon, T2
`essais/suivi.py`, la fonction `ll`), `NUMERO` étant le second téléphone.

**T3 — le relevé, à lancer PENDANT l'appel** (lecture seule, rien n'est modifié). Créer une fois
`essais/releve-son.sh` :

```sh
#!/usr/bin/env bash
# Relevé du son d'un appel : à lancer pendant l'appel, une ou plusieurs fois.
out="essais/son-$(date +%H%M%S).txt"
{
  echo "== $(date +%T.%N) rôle sur le lien classique"; hcitool con
  echo "== passerelle PipeWire"
  for ag in $(busctl --user tree --list org.pipewire.Telephony | grep -E '/ag[0-9]+$'); do
    busctl --user introspect org.pipewire.Telephony "$ag" | grep -E 'State|RejectSCO|Codec|Address'
    busctl --user tree org.pipewire.Telephony | grep call
  done
  echo "== carte et nœuds de l'iPhone"
  pw-dump | python3 -I -c '
import json, sys
for o in json.load(sys.stdin):
    info = o.get("info") or {}
    p = info.get("props") or {}
    name = p.get("device.name") or p.get("node.name") or ""
    if "bluez" not in name or "midi" in name:
        continue
    print(o["id"], name, p.get("media.class"), info.get("state", ""))
    for prof in (info.get("params") or {}).get("Profile", []):
        print("   profil actif :", prof.get("name"))
    for prof in (info.get("params") or {}).get("EnumProfile", []):
        print("   profil possible :", prof.get("name"), prof.get("available"))'
  echo "== liens"; pw-link -l | grep -B1 -A2 bluez
  echo "== sortie et micro par défaut"; wpctl status | sed -n '/Audio/,/Video/p' | grep -E '\*|bluez'
} > "$out" 2>&1
echo "$out"
```

Si le son manque ou si le lien tombe, refaire l'essai avec une trace radio dans un quatrième terminal
(mot de passe demandé, à lancer avant de composer, Ctrl-C après) : `sudo btmon -w essais/S1.btsnoop`.

### S1. Appel sortant, son ici — lien fermé au départ

```sh
ll '{"command":"bt_call_dial","number":"NUMERO","audio":"here","operation_id":"s1"}' 60
```

Décrocher le second téléphone après trois sonneries, parler dans les deux sens, lancer
`essais/releve-son.sh` pendant qu'on parle, raccrocher depuis le second téléphone.

| Moment | Journal du démon (T1) | Client (T2) |
|---|---|---|
| demande | `classic link opening; connecting for hands-free (leases: call…)` | `bt_call_progress` `connecting` puis `hands_free` |
| passerelle prête | `dial asked, sound on this computer (client said: here)` ; `telephony: the voice link of the next call will be accepted here` ; `dial accepted by the iPhone` | `dialing`, `bt_call_result` `success:true` |
| appel vu, < 1 s | `call: outgoing to ***NN (dialing)` ; `a call is in hand; address book, call history and messages wait for it` | `bt_calls` (`dialing`) |
| lien voix | `telephony: voice link pending (was idle; RejectSCO false)` puis `voice link active` ; `the sound of the call is on this computer` | `bt_call_progress` `audio_here` ; `calls.audio_route:"here"` |
| le second téléphone sonne | `call: ***NN dialing -> alerting` | `bt_calls` (`alerting`) |
| décroché | `call: answered, ***NN (alerting -> active)` | `bt_calls` (`active`) |
| raccroché | `call: ended, ***NN (was active)` ; `the sound of calls stays on the iPhone` ; `the call ended; reading the call history over the link it leaves` ; `pulled N recent calls` | `bt_calls` vide |
| 15 à 30 s après | `classic link lingering` puis `closing` | `classic_state` `idle` |

Attendu dans le relevé : `State "active"`, `RejectSCO false`, carte sur `audio-gateway`, nœuds
`bluez_input.….0` et `bluez_output.….1` en `running`, reliés à la sortie et au micro par défaut ;
`hcitool con` : noter `MASTER` ou `SLAVE`.

**Ne doit jamais apparaître** : `Activate failed` avant `call: outgoing` ; `SetFolder`, `inbox listed`,
`address book` ou `phone audio: back on the iPhone` entre `dial accepted` et `call: ended` ;
`classic link lingering` pendant l'appel ; `Bearer.BREDR1 disconnected` pendant l'appel.

À noter : le temps entre `dial accepted` et `voice link active` ; si le son n'arrive qu'après la ligne
`the call is up and its sound is not here …; asking the iPhone for it` (l'iPhone n'a pas offert le lien
voix de lui-même) ; si l'on entend la tonalité de retour d'appel sur l'ordinateur avant le décroché.

Si le lien tombe ou gèle encore (`dialing` qui ne bouge plus, `Reason.Unknown`) : relever `hcitool con`,
refaire avec `btmon`, puis refaire en mode complet (`LANDLINE_LINK_MODE=full`) pour comparer. C'est le
cas qui départage les deux hypothèses de `moteur-quotidien.md` §7.

### S2. Appel sortant, son sur l'iPhone

```sh
ll '{"command":"bt_call_dial","number":"NUMERO","audio":"phone","operation_id":"s2"}' 60
```

Attendu : `dial asked, sound on the iPhone (client said: phone)` ; `telephony: the voice link of this
call will be refused; its sound stays on the iPhone` ; les états `dialing` → `alerting` →
`active` comme en S1 ; **aucune** ligne `voice link pending` ni `active` ; pas de `bt_call_progress`
`audio_*` ; le son dans l'écouteur de l'iPhone. Relevé : `State "idle"`, `RejectSCO true`, pas de nœud
`bluez_input`.

Variante sans `audio` (`{"command":"bt_call_dial","number":"NUMERO"}`) : doit se comporter comme S1, avec
`(client said: nothing)`.

### S3. Appel entrant, « Answer here » — lien fermé au départ

Appeler l'iPhone depuis le second téléphone. Attendre `bt_incoming_call` avec `answer_here:"ready"`
(section 8, E4), puis :

```sh
ll '{"command":"bt_call_action","action":"answer","audio":"here"}' 30
```

| Moment | Journal du démon |
|---|---|
| sonnerie | `classic link opening; connecting for hands-free (leases: call…)` ; `call: incoming from ***NN (incoming)` ; `the sound of calls stays on the iPhone` (la sonnerie ne se joue pas sur l'ordinateur) |
| « Answer here » | `telephony: the voice link of the next call will be accepted here` ; `call: answered, ***NN (incoming -> active)` |
| son | `voice link pending` → `voice link active` ; `the sound of the call is on this computer` ; au pire, 3 s plus tard, `asking the iPhone for it` puis `voice link active` |
| fin | `call: ended` ; `the sound of calls stays on the iPhone` |

Client : `bt_call_result` `success:true`, `via:"hfp"`, puis `bt_call_progress` `audio_here`. Lancer le
relevé pendant l'appel. Refaire en décrochant **sur l'iPhone** : aucune ligne `voice link`, son sur
l'iPhone.

### S4. Bascule en cours d'appel

Pendant un appel de S1 (son ici), puis retour :

```sh
ll '{"command":"bt_set_call_audio","route":"phone","persist":false}' 10    # relevé, parler
ll '{"command":"bt_set_call_audio","route":"here","persist":false}' 10     # relevé, parler
```

Attendu : `call audio handed back to the iPhone (released, voice link closed in N ms)` ou
`(released_parked, …)`, `voice link idle (was active; RejectSCO true)`, `audio_route:"phone"` ; puis
`voice link pending` → `active`, `the sound of the call is on this computer`, `bt_call_progress`
`audio_here`. L'appel ne doit être coupé à aucun moment. Si le retour échoue :
`the sound of the call did not come to this computer (no_voice_link | voice_link_unused; …)` et
`bt_call_progress` `audio_failed` 12 s après — noter le motif et joindre le relevé.

### Tableau à remplir

| Essai | Son ici (o/n) | `dial accepted` → `voice link active` | États vus | Rôle (`hcitool con`) | Lien tenu jusqu'au bout | Remarques |
|---|---|---|---|---|---|---|
| S1 | | … s | | | | |
| S1 mode complet | | … s | | | | |
| S2 | sans objet | sans objet | | | | son sur l'iPhone : o/n |
| S3 | | … s après le décroché | | | | sonnerie jouée ici : o/n |
| S4 aller | sans objet | fermé en … ms | | | | |
| S4 retour | | … s | | | | |

## 10. LE fragile : essais R1 à R5 (branche `moteur-le-fragile`)

À lire avant : `moteur-reconnexion.md` §13. Préparation commune de la section 8 (mode sobre, un seul démon).
Chaque essai dure 5 à 10 min. Dans T4, pour tous : `sudo btmon -w essais/R<n>.btsnoop | tee essais/R<n>-btmon.log`.

Ce qu'on cherche dans un relevé `btmon`, une fois pour toutes :

| Ligne | Ce qu'elle dit |
|---|---|
| `LE Enhanced Connection Complete` → `Connection interval`, `Supervision timeout`, `Role: Peripheral` | les paramètres choisis par iOS (attendu : 30 ms, 720 ms) |
| `LE Connection Update Complete` | iOS a changé les paramètres ; noter l'heure et les valeurs |
| `Create Connection` (BR/EDR) … `Connect Complete` | début et fin de la *page* : sa durée est la donnée de R2 |
| `Disconnect Complete` → `Reason` sur le handle LE | `Connection Timeout (0x08)` : supervision ; `Connection Failed to be Established (0x3e)` : le lien n'a jamais échangé ; `Remote User Terminated (0x13)` : l'iPhone l'a fermé |
| `LE Set Extended Advertising Enable` | l'annonce est (re)mise ou retirée du contrôleur |
| `L2CAP: Connection Parameter Update Request` / `Response` | une demande L2CAP du noyau et la réponse d'iOS (ne devrait pas apparaître : iOS choisit 30 ms, dans la fenêtre du noyau) |
| `LE Connection Update` (commande) puis `LE Connection Update Complete` | la demande de paramètres du moteur ou de `hcitool lecup`, et ce que l'iPhone a appliqué (P1) |

### R1. Retour à la maison — le scénario de 18:05

> **Remplacé par P2** (section 0) : la pause après 3 coupures en 60 s décrite ci-dessous n'existe plus par
> défaut ; l'annonce est retirée dès le premier lien mort avant service. Le texte reste pour le contre-essai
> avec `LANDLINE_LE_STRIKES=0`.

1. iPhone verrouillé en poche, sortir jusqu'à `iPhone away` au journal, attendre 2 min, revenir à pas normal.
2. Attendu au journal : au plus quelques `Bearer.LE1 disconnected` ; après 3 en 60 s, `the advertisement is
   taken off air for 30s` ; puis `the LE link is back; the classic link is not opened before it has held for
   45s`, et **45 s plus tard seulement** `sync wanted (the iPhone is back)` ; aucune coupure LE dans les 10 s
   qui suivent ; `the LE link has held`.
3. Noter : nombre de liens LE avant le lien qui tient, délai entre le pas de la porte et `Notification
   mirroring is active`, durée de la page (`btmon`).
4. Échec : l'iPhone ne répond plus du tout → noter l'heure, `le_cause` publié, et passer à R5 sans toucher à
   l'iPhone.
5. Contre-essai, un autre jour : `LANDLINE_LE_BURST_PAUSE=0 LANDLINE_LE_STABLE_SECONDS=0` (comportement du 17).

### R2. La page fait-elle tomber le LE ? — iPhone loin mais joignable

1. iPhone posé à la limite de portée (deux pièces plus loin, là où le LE tient tout juste), LE monté depuis
   plus d'une minute.
2. Demander une lecture depuis l'interface (bail `reading`, non retenu), dix fois à 1 min d'intervalle.
3. Pour chaque ouverture, dans `btmon` : durée de la page, et le lien LE est-il tombé ? Délai entre
   `Create Connection` et le `Disconnect Complete` LE (attendu si l'hypothèse tient : la valeur de
   `Supervision timeout`, 720 ms, chaque fois que la page dure plus longtemps).
4. Même chose iPhone sur le bureau : la page dure moins de 720 ms et le LE tient (attendu).
5. Au journal, chaque coupure imputée : `the LE link dropped Ns after the classic link was opened`.

### R3. La fermeture — vingt fois

Reprendre E2 (vingt ouvertures et fermetures) et compter les coupures LE dans les 10 s d'une fermeture
(ligne `… after the classic link was closed`). Attendu : zéro. S'il y en a : motif (`Reason`) dans `btmon`.

### R4. Délai de supervision de 6 s (root, réversible)

> **Abandonné, remplacé par P1** (section 0). Ce réglage de `main.conf` ne ferait pas demander 6 s : pour un
> lien que l'iPhone ouvre, le noyau recopie dans sa demande le délai de supervision en cours (720 ms), et
> seuls les intervalles viennent du fichier (`moteur-reconnexion.md` §13.2, étude 12 §4.3). Ne pas le faire.

1. Sauvegarder `/etc/bluetooth/main.conf`, puis dans `[LE]` :
   `MinConnectionInterval=36`, `MaxConnectionInterval=48`, `ConnectionLatency=0`,
   `ConnectionSupervisionTimeout=600`. `sudo systemctl restart bluetooth`.
2. Au retour du LE, dans `btmon` : `Connection Parameter Update Request` (45–60 ms, délai 6 s) envoyée par
   l'ordinateur, et la réponse d'iOS (`Accepted` ou `Rejected`), puis `LE Connection Update Complete`.
3. Si accepté : refaire R1 et R2. Attendu : plus de coupure à l'ouverture du classique, beaucoup moins de
   battements à la limite de portée (un lien à 6 s survit à des trous de plusieurs secondes).
4. Si refusé : essayer `MinConnectionInterval=24`, `MaxConnectionInterval=36` (30–45 ms) — mais alors
   l'intervalle d'iOS (30 ms) est dans la fenêtre et le noyau ne demande rien : le dire au tableau.
5. Ces réglages valent pour tous les appareils LE de la machine (souris, clavier) : vérifier qu'ils se
   reconnectent, sinon restaurer la sauvegarde.

### R5. iPhone qui ne répond plus : qu'est-ce qui le fait repartir ?

À faire dès que l'état se présente (`le_cause` `phone_backoff`, ou `phone_away` iPhone posé à côté), **sans
toucher à l'iPhone**, `btmon` lancé, dans cet ordre, 5 min d'attente entre deux gestes, en notant l'heure :

1. Rien, 15 min : revient-il seul ? (le 17 à 13:16 : oui, après 14 h 50.)
2. Vérifier dans `btmon` que l'annonce est en ondes (`LE Set Extended Advertising Enable: Enabled`, intervalle).
3. Déverrouiller l'iPhone, écran allumé à côté de l'ordinateur.
4. Ouvrir Réglages › Bluetooth sur l'iPhone, sans rien toucher (iOS y relance une recherche).
5. Demander une lecture depuis l'interface (ouvre le classique) ; noter si le LE suit.
6. `systemctl --user restart landline-engine.service` (annonce réenregistrée).
7. En dernier : couper puis rallumer le Bluetooth de l'iPhone. Noter le délai de retour du LE.

### Tableau à remplir

| Essai | Liens LE avortés | Paramètres LE (intervalle / délai) | Durée de la page | LE tombé à l'ouverture (n/10) | à la fermeture (n/20) | Ce qui a fait repartir l'iPhone | Remarques |
|---|---|---|---|---|---|---|---|
| R1 | | | | sans objet | sans objet | | |
| R1 contre-essai | | | | | | | |
| R2 loin | | | | | sans objet | | |
| R2 près | | | | | sans objet | | |
| R3 | | | | sans objet | | | |
| R4 | | | | | | | demande acceptée : o/n |
| R5 | | | | | | | |
