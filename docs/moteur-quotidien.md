# Moteur : trois sujets du quotidien

Branche `moteur-quotidien`. Son de l'iPhone, éloignement, batterie : ce que le moteur faisait, ce qui a
été changé et pourquoi, ce qui reste à essayer avec l'iPhone. Le contrat (noms exacts des champs,
événements, commandes) est dans `moteur-api.md`, sections 3, 7, 8 et 9. Les chemins sont relatifs à
`engine/`.

Rien de ce qui suit n'a tourné face à l'iPhone : le moteur n'a pas été lancé, seuls la compilation et
les tests unitaires l'ont été. Ce qui est observé sur la machine l'a été en lecture seule (`wpctl status`,
`wpctl inspect`, `busctl --user tree`), sans rien modifier.

## 1. Le son de l'iPhone ne part plus sur l'ordinateur tout seul

### Constat

Machine : PipeWire 1.6.8, WirePlumber 0.5.17, BlueZ 5.87, aucun fichier dans `~/.config/wireplumber`
(configuration d'usine), `pactl` absent.

- Avec WirePlumber d'usine, `bluez5.roles` contient `a2dp_sink` et `hfp_hf` : l'ordinateur s'annonce comme
  enceinte et comme kit mains-libres. iOS route son audio vers tout appareil appairé qui s'annonce
  ainsi, comme vers une voiture. Le moteur ne fait que monter le lien ; la décision est celle de l'iPhone.
- Vu sur la machine, iPhone relié : PipeWire tient une carte `bluez_card.<adresse>` de facteur de forme
  `phone`, profil actif `audio-gateway` (« A2DP Source & HSP/HFP AG »). C'est bien le chemin par lequel
  la musique arrive.
- L'iPhone **ouvre lui-même** le flux A2DP environ une seconde après chaque connexion (note amont du
  25 octobre 2026, `docs/BLUETOOTH.md`). Empêcher WirePlumber de le composer ne suffit donc pas.
- Côté appels, PipeWire accepte d'origine le lien voix (SCO) que l'iPhone lui offre : la capture
  enregistrée d'un appel réel dans les tests (`test/test_telephony.cpp`, `PIPEWIRE_ROOT`) montre
  `RejectSCO: false` et un transport `active`. Le son d'un appel vient donc ici aussi, sans rien choisir.

### Ce que le moteur faisait déjà

| Élément | État avant |
|---|---|
| `phone_audio` (`auto`, `here`, `phone`) et `PhoneAudioFollower` | existait ; **défaut `auto`** : rien n'était fait, le son venait ici |
| Mode `phone` | refermait le flux **une fois** par connexion, dans les 30 premières secondes |
| `bt_set_phone_audio` | écrivait **toujours**, hors `auto`, `~/.config/wireplumber/wireplumber.conf.d/zz-landline-phone-audio.conf` et **redémarrait WirePlumber** |
| `bt_call_action` `audio_here` / `audio_phone` (CLI `--bt-call-audio`) | existait, sans mémoire : rien ne remettait l'interrupteur en place à l'appel suivant, et rien ne le fermait par défaut |
| `51-no-phone-audio.conf` | **jamais écrit par le moteur** : c'est une recette de la documentation amont, à poser à la main, qui change `bluez5.roles` pour tous les appareils |
| `pactl` | utilisé par le seul code des AirPods (`src/core/src/audio.cpp`), désactivé par défaut. Le routage du son de l'iPhone n'en dépend pas : il passe par D-Bus (BlueZ, PipeWire) |

### Les moyens possibles, et celui retenu

| Moyen | Portée | Verdict |
|---|---|---|
| Retirer `a2dp_sink` (et `hfp_hf`) de `bluez5.roles` | **globale** : tous les appareils, redémarrage de WirePlumber | écarté. Sans `a2dp_sink`, iOS ne tient plus l'ordinateur pour une destination audio et **n'ouvre plus le mains-libres vers PipeWire** : plus de son d'appel possible ici, et le contrôle d'appel dépend alors de BlueZ. Trop intrusif pour un défaut |
| Règle WirePlumber par appareil `bluez5.auto-connect = [ ]` | l'iPhone seul, mais un fichier chez WirePlumber et un redémarrage (son coupé, casques reconnectés) | insuffisant seul : l'iPhone ouvre le flux lui-même. Gardé **en option** (`rule:true`) |
| Règle par appareil `device.profile = off`, ou profil de carte `off` | l'iPhone seul | écarté : l'iPhone croit toujours jouer ici, le son ne sort nulle part (note amont) |
| `device.disabled = true` dans une règle | — | sans effet sur le moniteur BlueZ (note amont) |
| Refuser le profil côté BlueZ (`Device1`, `MediaEndpoint1`) | — | impossible proprement : le point de terminaison A2DP appartient à PipeWire, et BlueZ n'a pas de refus par profil et par appareil (`Blocked` coupe tout l'appareil) |
| **Fermer le profil A2DP de l'iPhone** : `Device1.DisconnectProfile(0000110a…)` | **l'iPhone seul**, rien sur le disque, rien de redémarré, le lien et les autres profils restent | **retenu**. C'est ce que fait le mode `phone`, par le bus système de BlueZ |
| Son des appels : `RejectSCO` de `org.pipewire.Telephony.AudioGatewayTransport1` | le mains-libres de l'iPhone seul, par le bus de session | **retenu** |

Les autres appareils audio Bluetooth (casques, enceintes) ne sont jamais concernés : ils utilisent les
rôles `a2dp_source` et `hfp_ag` de l'ordinateur, que rien ici ne touche, et les deux appels retenus
visent l'objet BlueZ ou PipeWire de l'iPhone par son adresse.

### Ce qui a été fait

- **Défaut `phone_audio: "phone"`** (`config.hpp`). Un `auto` écrit par une version antérieure est lu
  comme `phone` (`config_version` passe à 3) : c'était l'ancien défaut, pas un choix.
- **Le mode `phone` tient mieux** (`plan_phone_audio()`, `phone_audio.cpp`) : le flux est refermé jusqu'à
  **trois fois** dans les 30 s qui suivent la connexion, au lieu d'une. Sans la règle WirePlumber, rien
  n'empêche WirePlumber de recomposer le flux une fois ; cette borne le couvre sans se battre
  indéfiniment contre un choix fait sur l'iPhone.
- **La règle WirePlumber devient une option** (`phone_audio_rule`, faux par défaut ; paramètre `rule` de
  `bt_set_phone_audio`). Par défaut le moteur n'écrit plus rien hors de `~/.config/landline/` et ne
  redémarre plus WirePlumber. La règle est retirée avec `rule:false`, avec `route:"auto"`, et au retrait
  de l'appairage (`sync_wireplumber_rule()`).
- **Bascule ponctuelle** : `persist:false` déplace le son pour la connexion en cours sans toucher au
  réglage.
- **Son des appels** : réglage `call_audio` (`phone` par défaut), commande `bt_set_call_audio`. Le moteur
  ferme `RejectSCO` dès que le mains-libres de l'iPhone apparaît et le remet sur le réglage à la fin de
  chaque appel (`CallAudioPolicy`). Répondre et raccrocher ne passent pas par là et fonctionnent dans
  tous les cas. Paramètre `audio` sur `bt_call_dial` et sur `bt_call_action answer` pour choisir appel
  par appel.
- **État et commandes sur le socket** : requête et événement `bt_audio`, résultats
  `bt_phone_audio_result` et `bt_call_audio_result`, capacité `bluetooth.audio`.
- **Mode sobre (lien classique à la demande), #38 et #43** : jusqu'au 18 novembre, `phone_audio: "here"`
  était un bail `audio` permanent ; le lien classique est resté monté plus d'une heure (17:01 à 18:26) sans
  rien jouer, et quand le LE est tombé à 18:18 il n'est plus revenu (voir plus bas). Désormais
  (`plan_audio_lease()`, `classic_demand.hpp`) :
  - **demandé** (`bt_set_phone_audio here`, à l'allumage ou redemandé : `ask_phone_audio()`) : bail de
    **120 s** (`PHONE_AUDIO_OFFER_SECONDS`), le temps que le lien s'ouvre (« classic link opening;
    connecting for hands-free (leases: audio) », 2 s environ), que `PhoneAudioFollower` ouvre le flux A2DP
    (5 s après, `PHONE_AUDIO_HERE_DELAY`) et que l'iPhone liste l'ordinateur et y joue ;
  - **le son joue** (transport A2DP `pending` ou `active`, et non plus seulement présent : `audio_playing()`) :
    bail renouvelé à chaque passe pour **60 s** (`PHONE_AUDIO_SILENCE_SECONDS`) ; aussi sur un lien que
    l'iPhone a ouvert lui-même ;
  - **silence** : le bail s'éteint 60 s après la dernière passe qui jouait, puis les 30 s de maintien, et le
    lien se ferme. Le choix reste ; rien n'est rouvert pour lui seul, ni au retour de l'iPhone ;
  - **éteint** : bail rendu tout de suite.
  Un flux A2DP connecté mais muet ne compte plus comme du son (`phone_audio_streaming` des `DemandFacts`) :
  il retenait lui aussi le lien.
- **LE tombé sous un classique ouvert (#43)** : le 18 novembre à 18:18:03 le LE tombe (délai de
  supervision), le classique reste, ANCS se réabonne par lui (« Notification mirroring is active »,
  18:18:17), mais l'iPhone ne répond plus à l'annonce (`phone_silent` dès 18:21), comme le 16 novembre
  (`moteur-reconnexion.md`, §10.3). Désormais, en mode sobre, LE absent depuis **30 s**
  (`CLASSIC_LE_CYCLE_SECONDS`) et classique monté (ouvert ou en maintien, par le moteur ou par l'iPhone) :
  le classique est fermé une fois, ses baux gardés (« no LE link for 30s while the classic link is up;
  closing the classic link once so that the iPhone comes back over LE »). Rien n'est rouvert sans LE ; le LE
  revenu, les baux restants rouvrent le lien. Jamais sous un appel, pendant que le son joue ici ou quand
  NetworkManager se sert du lien ; pas deux fois en 10 min (`CLASSIC_LE_CYCLE_INTERVAL_SECONDS`).
  Tests : `ClassicDemand.TheAudioLeaseFollowsTheAskAndTheSoundNotTheChoiceAlone`,
  `TurnedOnTheLinkIsOfferedThenLetGoWhenNothingPlays` (le cas de 17:01), `WhileTheSoundPlays…`,
  `SoundTheIPhoneSends…`, `TurnedOffTheAudioLeaseIsReturned`, `TheLeLinkGoneUnderAnOpenClassicLinkCyclesItOnce`
  (le cas de 18:18), `TheLeCycleIsHeldUnderACallTheSoundOrTethering`, `TheLeCycleIsNotRepeatedWithinTenMinutes`,
  `ALeaseHeldWhileTheIPhoneIsAwayOpensTheLinkWhenItComesBack` (`test/test_classic_demand.cpp`).
  Le « rien ne se passe » du #38 venait de l'extension : `docs/extension.md`, « Le quotidien », **Son**.
- **Ce qui peut tenir le lien classique en mode sobre** (état au 18 novembre, après #39 et #43). Principe :
  le classique n'est ouvert que pour une raison précise et bornée ; elle finie, il se ferme après un court
  maintien, et le LE reste seul. Aucun bail ne le tient « au cas où ».

  | Bail ou raison | Pris quand | Fin | Au plus | Maintien après |
  |---|---|---|---|---|
  | `sync` (automatique) | retour du LE (au plus toutes les 10 min) | relève faite | 120 s | 10 s |
  | `arrival` (automatique) | notification de Messages | relève faite | 120 s | 45 s |
  | `hourly` (automatique) | une heure sans relève | relève faite | 120 s | 0 s |
  | `send` | envoi d'un message | envoi fini (réussi ou non) | durée de l'envoi | 90 s |
  | `reading` (client) | conversation ou « New Message… » ouverte dans le menu | carte fermée (l'extension rend le bail) ; sans renouvellement | 300 s | 20 s |
  | `setup` (client) | parcours d'accueil ouvert | parcours fermé ; sans renouvellement | 300 s | 0 s |
  | `call` | sonnerie, composition, appel en cours | appel fini, ou 20 s après une sonnerie sans suite | sonnerie 90 s, puis l'appel | 15 s |
  | `audio` | son demandé ici (allumage, « Connect for Sound ») | 120 s sans son | 120 s | 30 s |
  | `audio` | le son de l'iPhone joue ici | 60 s après la fin du son | durée du son + 60 s | 30 s |
  | lien ouvert par l'iPhone | BR/EDR monté sans bail | 120 s sans appel, son ni opération | — | — |
  | appel, son qui joue, partage de connexion (`busy`) | tant qu'ils durent | leur fin ; le partage fini, refermé 120 s après | leur durée | — |
  | plafond des baux de données | baux `sync`/`arrival`/`hourly`/`reading`/`send` enchaînés | fermeture forcée (hors envoi, appel, son, partage) | 300 s | — |
  | LE tombé, classique monté | — | fermé une fois après 30 s sans LE (#43) | 30 s | — |

### Lien entre le mains-libres de PipeWire et le rôle d'enceinte

`org.pipewire.Telephony` n'existe pour l'iPhone que si l'iPhone a ouvert le mains-libres vers PipeWire,
et iOS ne le fait que vers une machine qu'il tient pour une destination audio, donc qui annonce
`a2dp_sink`. C'est pour cela que la solution retenue **garde les rôles d'usine** et ferme le flux après
coup, au lieu de retirer le rôle : retirer `a2dp_sink` ferait disparaître le mains-libres de PipeWire, et
avec lui le son des appels à la demande. Au moment de l'étude, iPhone relié, `busctl --user tree
org.pipewire.Telephony` ne montrait aucune passerelle : le service est publié mais le mains-libres de
l'iPhone n'était pas ouvert vers PipeWire (à regarder au premier essai, voir 5.1).

Ce que la fermeture du flux A2DP fait au mains-libres n'est pas mesuré : en principe rien (profils
distincts, lien conservé), à vérifier.

## 2. Éloignement

### Ce qui existait

`supervise_away_lock()` verrouillait par logind quand tous les liens étaient tombés depuis
`lock_away_seconds` avec le motif `org.bluez.Reason.Timeout`. Aucun événement ; `bt_set_lock_on_away`
avec `enabled` seul remettait le délai à 30 s ; activer le réglage alors que l'iPhone était déjà parti
verrouillait aussitôt.

### Ce qui a été fait

- **Suivi de présence pur** (`PresenceTracker`, `presence.hpp`), alimenté une fois par seconde, qui
  publie `bt_presence` : `state` (`here`, `away`), `reason` (`out_of_range`, `switched_off`,
  `computer_suspended`, `bluetooth_off`, `local`, `unknown`), `lock` (`none`, `off`, `spared`, `pending`,
  `locked`, `failed`), `away_seconds`, `lock_in_seconds`, `wake`.
- **Absence confirmée après 3 s** sans aucun lien, et pas pendant la coupure BR/EDR que le moteur fait
  lui-même pour le mains-libres : pas de faux « parti ».
- **Verrouillage inchangé dans son principe, resserré** : seulement `out_of_range`, seulement pour une
  absence commencée réglage actif, une fois par absence, résultat publié (`locked` ou `failed`).
- **Réglages** : `lock_on_away` (faux par défaut), `lock_away_seconds` (30, borné de 1 à 3600),
  `wake_on_return` (faux par défaut). `bt_set_lock_on_away` ne change que les champs fournis.
- **Aucun déverrouillage.** `wake_on_return` ajoute `wake:true` au message de retour et lance
  `wake_command` s'il est renseigné ; rien d'autre.

### Verrouillage : moteur ou extension ?

**Moteur.** Une mesure de sécurité ne doit pas dépendre d'un client connecté : l'extension peut être
désactivée, le Shell redémarré, et le réglage resterait actif sans effet. `Session.Lock` de logind est
l'appel que GNOME attend, sans droit particulier, et le moteur détient déjà le motif BlueZ et l'horloge.
L'extension affiche l'événement.

**Réveil de l'écran : extension.** Il n'existe pas d'appel D-Bus public qui allume l'écran de connexion
de GNOME sans toucher au verrou : `org.gnome.ScreenSaver.SetActive(false)` le lève (écarté),
`org.gnome.Mutter.IdleMonitor.ResetIdletime` n'est ouvert qu'aux tests de Mutter (de mémoire du code, à
confirmer). Le moteur signale donc
`wake:true` ; à l'extension, si elle tourne sur l'écran verrouillé, d'allumer l'écran.

## 3. Batterie et état du téléphone

| Information | Publiée ? | Détail |
|---|---|---|
| Niveau de batterie | oui : `battery`, plus `battery_source` et `battery_state` | service Batterie LE par BlueZ ; à défaut indicateur mains-libres de BlueZ (pas de 20 %) |
| En charge | **non, indisponible** | ni `org.bluez.Battery1`, ni le mains-libres ne le portent |
| Opérateur, signal, réseau, itinérance | seulement si le mains-libres est tenu par **BlueZ** | avec PipeWire (cette machine) : `calls.indicators:false`, valeurs vides |
| « WiFi-Calling » | `calls.wifi_calling`, **déduit du nom du réseau** | pas d'indicateur dédié ; ne vaut que si l'opérateur est publié |

Fréquence : aucune interrogation de l'iPhone. BlueZ reçoit les notifications ; le superviseur relit son
instantané chaque seconde et ne publie `bt_connection_changed` que sur changement.

Ajout : événement **`bt_battery_low`** à 20 % puis 10 %, une fois par franchissement, avec réarmement
5 points au-dessus du seuil (`BatteryWatch`, `phone_state.hpp`). Champ `initial` quand la première
lecture est déjà sous le seuil.

Obtenir opérateur et signal demanderait de rendre le mains-libres à BlueZ (retirer `hfp_hf` de
`bluez5.roles`) : réglage global de WirePlumber, et le son des appels ne pourrait plus venir ici. Non
fait ; à décider si ces informations comptent plus que le son des appels sur l'ordinateur.

## 4. Tests

`scripts/build-engine.sh` : 846 tests unitaires passaient à la fin de cette branche (781 avant) ; 909 après la passe 5 (section 6). Nouveaux fichiers :
`test/test_presence.cpp`, `test/test_phone_state.cpp`, `test/test_phone_audio.cpp` ; ajouts dans
`test/test_telephony.cpp` et `test/test_net.cpp`. Tout ce qui décide est une fonction pure testée sans
bus : `plan_phone_audio()`, `parse_audio_request()`, `CallAudioPolicy`, `PresenceTracker`,
`apply_lock_on_away()`, `BatteryWatch`, `operator_says_wifi_calling()`. Aucun test n'écrit la règle
WirePlumber ni ne redémarre quoi que ce soit.

Non couvert par les tests : les appels D-Bus eux-mêmes (`DisconnectProfile`, écriture de `RejectSCO`,
`Session.Lock`) et l'enchaînement dans le superviseur.

## 5. À vérifier avec l'iPhone

Installer le moteur de la branche, relancer le service, puis suivre le journal du moteur et le flux du
socket.

### 5.1 Son : musique

1. **Défaut.** `bluetooth.json` existant (avec `phone_audio: "auto"`, version 2). Lancer une musique sur
   l'iPhone, relier. Attendu : journal `phone audio: back on the iPhone` 3 à 5 s après la connexion ;
   `bt_audio` avec `phone_audio_route:"phone"` ; la musique sort de l'iPhone. Noter si elle se met en
   pause (attendu : oui, comme un casque débranché).
2. **Tenue dans le temps.** Laisser 10 minutes, lancer une autre musique, verrouiller et réveiller
   l'iPhone. Le flux se rouvre-t-il seul passé les 30 s ? Si oui, le défaut ne tient pas et il faut
   refermer à chaque ouverture non demandée (à décider alors).
3. **Sans la règle.** Compter les fermetures dans le journal : une seule attendue. Deux ou trois veut
   dire que WirePlumber recompose le flux, et que la règle (`rule:true`) est utile.
4. **Bascule ponctuelle.** `{"command":"bt_set_phone_audio","route":"here","persist":false}` : le son
   vient ici en quelques secondes ; couper puis rallumer le Bluetooth de l'iPhone : il repart sur
   l'iPhone.
5. **Réglage.** `route:"here"` : le son vient ici à chaque connexion. `route:"phone"` : retour.
6. **Choix sur l'iPhone.** Après 30 s, choisir l'ordinateur dans le Centre de contrôle : le son vient
   ici et y reste ; `bt_audio` le dit.
7. **Autres appareils.** Un casque Bluetooth relié à l'ordinateur pendant tout cela ne doit ni se couper
   ni changer de profil.
8. **Règle.** `rule:true` : le fichier apparaît, WirePlumber redémarre une fois ; `rule:false` : il
   disparaît. Vérifier `phone_audio_rule_installed`.
9. **Mains-libres après fermeture du flux.** `busctl --user tree org.pipewire.Telephony` montre-t-il
   toujours la passerelle de l'iPhone une fois le flux A2DP fermé ? Sinon le contrôle d'appel est perdu
   en mode `phone` et il faudra un autre moyen.

### 5.2 Son : appels

1. **Appel entrant, répondu sur l'iPhone**, `call_audio:"phone"` : le son reste sur l'iPhone ;
   `call_audio_reject:true` avant l'appel.
2. **Appel entrant, répondu depuis l'ordinateur** (`bt_call_action answer`) : où va le son ? La
   documentation amont rapporte qu'iOS offre le son à l'appareil qui a agi et ne se ravise pas : avec le
   lien voix refusé, **personne n'entendrait rien** jusqu'à ce qu'on choisisse l'iPhone sur l'iPhone. Si
   c'est confirmé, l'extension doit proposer « Répondre ici » (`audio:"here"`) et prévenir pour l'autre
   cas, ou le moteur doit accepter le son d'un appel pris ici. *(Essai du 16 novembre 2026 : « Answer
   here » fonctionne ; le retour vers l'iPhone en cours d'appel ne fonctionnait pas, voir section 6.)*
3. **Même essai pour un appel composé depuis l'ordinateur** (`bt_call_dial`).
4. **`audio:"here"` sur `answer`** : le son vient ici pour cet appel ; l'appel suivant reste sur
   l'iPhone (journal `telephony: the sound of calls stays on the iPhone` à la fin du premier).
5. **`call_audio:"here"`** : tous les appels sonnent ici.
6. **Valeur d'origine de `RejectSCO`** à chaque reconnexion : le moteur la referme-t-il bien à chaque
   fois (une ligne de journal par connexion) ?

### 5.3 Éloignement

1. **Partir avec l'iPhone**, `lock_on_away` actif, délai 30 s. Attendu : `away`/`out_of_range`/`pending`
   20 à 40 s après le départ, `away`/`locked` 30 s plus tard, session verrouillée ; au retour `here` avec
   `away_seconds`, **session toujours verrouillée**.
2. **Couper le Bluetooth sur l'iPhone** : `away`/`switched_off`/`spared`, pas de verrouillage.
3. **Mettre l'ordinateur en veille puis le réveiller** : `away`/`computer_suspended` au réveil (ou
   aucun message si le lien remonte en moins de 3 s), pas de verrouillage.
4. **Éteindre le Bluetooth de l'ordinateur** : `bluetooth_off`, pas de verrouillage.
5. **Mode avion sur l'iPhone, iPhone éteint, batterie vide** : quel motif BlueZ ? Un `Timeout`
   verrouillerait (acceptable : l'iPhone n'est plus joignable), à constater.
6. **Faux départs** : une journée de travail iPhone posé à côté, réglage actif. Aucun `away` attendu.
   Noter tout `away` parasite et son motif (c'est le risque principal de la fonction).
7. **Activer le réglage iPhone déjà parti** : pas de verrouillage immédiat.
8. **`wake_on_return`** : le `here` porte `wake:true` seulement après un verrouillage fait par le moteur.
   L'extension reçoit-elle le message sur l'écran verrouillé ?
9. **Motif en retard** : le premier `away` porte-t-il déjà `out_of_range`, ou d'abord `unknown` puis
   `out_of_range` ?

### 5.4 Batterie et état

1. `battery` et `battery_source:"le"` présents quelques secondes après la montée du lien LE ; suivre une
   heure : un message par point perdu ?
2. Brancher et débrancher le chargeur : rien ne doit l'indiquer (confirmer l'absence).
3. Laisser descendre sous 20 % puis 10 % (ou relier un iPhone déjà bas) : un `bt_battery_low` par seuil,
   `initial:true` si déjà bas à la connexion. Recharger au-delà de 25 % puis redescendre : nouvel
   événement.
4. Couper et rallumer le Bluetooth sous 20 % : pas de second événement.
5. Opérateur, signal, itinérance, « WiFi-Calling » : seulement visibles en rendant le mains-libres à
   BlueZ. Si l'essai est fait un jour : relever `calls.operator` en Wi-Fi seul pour confirmer que le nom
   contient bien « WiFi-Calling » (hypothèse de `wifi_calling`).

## 6. Passe 5 : rendre le son d'un appel en cours à l'iPhone

### Constat (essai réel du 16 novembre 2026)

« Answer here » fonctionne : le son de l'appel passe par l'ordinateur (journal du moteur, appels de
19 h 57, 19 h 58 et 20 h 12 : `call: answered`). Choisir ensuite « iPhone » dans la carte d'appel ne
renvoie pas le son : c'était la limite annoncée en section 5.2, `RejectSCO` ne refuse que le lien voix
**suivant**.

### Ce que PipeWire et BlueZ offrent (lu sur la machine, sans rien modifier)

- `org.pipewire.Telephony.AudioGatewayTransport1` (interface lue dans `libspa-bluez5.so` de PipeWire
  1.6.8) : propriétés `State`, `Codec`, `RejectSCO`, et **une seule méthode, `Activate`**. Il n'existe
  ni `Deactivate`, ni `Release`, ni commande AT exposée pour fermer le lien voix.
- `AudioGateway1` : `Dial`, `SwapCalls`, `HangupAll`, `SendTones`, volumes. Rien sur le son.
- BlueZ n'expose pas le lien voix (SCO) : pas de transport à fermer de ce côté, et
  `Device1.DisconnectProfile` du mains-libres couperait aussi le contrôle de l'appel.
- En HFP, il n'y a pas de commande « rends le son au téléphone » : le transfert se fait en **fermant
  le lien voix**. PipeWire le ferme lui-même (`sco_release_cb` : « Transport released », le socket SCO
  est fermé) quand plus aucun nœud n'utilise le transport.
- `wpctl set-profile ID INDEX` et `pw-cli set-param ID Profile …` sont présents. Sur le profil `off`,
  PipeWire retire les nœuds de la carte, donc relâche le transport.

### Moyen retenu

Mettre la carte PipeWire de l'iPhone sur `off` **le temps que le lien voix se ferme**, puis la remettre
sur son profil ; `RejectSCO` à vrai d'abord, pour que rien ne le rouvre. Détail et contrat dans
`moteur-api.md`, section 9. Écartés : suspendre le nœud (WirePlumber le relance tant qu'il est relié),
détruire le nœud (laisse la carte dans un état incohérent), couper le profil mains-libres côté BlueZ
(perd le contrôle de l'appel), laisser la carte sur `off` tout l'appel par principe (inutile si le lien
reste fermé ; gardé seulement comme repli, `audio_parked`).

L'appel n'est jamais raccroché par la bascule : aucune commande d'appel n'est envoyée, seul le lien
voix est fermé. Si PipeWire perdait aussi le lien de commande (non attendu), l'iPhone garderait l'appel,
sur son propre son.

### État réel

`calls.audio_route` et `bt_audio.call_audio_route` sont lus sur l'état du transport de PipeWire à chaque
lecture. Le résultat de `audio_phone` dit si le lien s'est fermé, pas si la demande est partie.
L'extension affiche « Couldn’t move the call to the iPhone » dès ce refus, ou au bout de 3 s si l'état
n'a pas changé.

### Tests

`test/test_call_audio_handover.cpp` (20 tests) : lecture de la carte dans `pw-dump`, choix du profil,
séquence de bascule (lien fermé, lien qui reste, lien qui se rouvre à la remise du profil, pas de
carte, carte déjà sur `off`), et les mêmes cas à travers `TelephonyClient` (`audio_phone`,
`audio_here`, remise de la carte à la fin de l'appel). Aucun test ne lance `pw-dump`, `pw-cli` ni
`wpctl`.

### Non vérifiable sans appel réel

Rien de ceci n'a tourné face à un appel : le moteur n'a pas été lancé, et l'iPhone n'était pas relié
à PipeWire au moment de l'étude (pas de carte `bluez_card` à lire).

1. Que le profil `off` ferme bien le lien voix d'un appel en cours sur PipeWire 1.6.8 (déduit du code
   de PipeWire, pas mesuré), et en combien de temps.
2. Que le mains-libres (`org.pipewire.Telephony/ag0`) survit au passage par `off` : sinon le contrôle
   de l'appel est perdu jusqu'à la reconnexion (l'appel, lui, continue sur l'iPhone).
3. Que l'iPhone reprend le son sur son écouteur quand le lien voix se ferme (comportement HFP
   attendu), sans rouvrir le lien ni mettre l'appel en attente.
4. Que la remise du profil `audio-gateway` ne rouvre pas le lien voix (sinon : repli `audio_parked`,
   prévu et testé, à constater dans le journal : `released_parked`).
5. Que `pw-cli set-param … save: false` est accepté tel quel, et que WirePlumber ne retient pas `off`
   comme profil de la carte à la connexion suivante.
6. Le retour : « This computer » en cours d'appel après un passage sur l'iPhone (`Activate()`,
   `AT+BCC`) ramène-t-il le son ici, et en combien de temps.
7. Le nom exact du profil de la carte de l'iPhone (`audio-gateway` attendu ; le moteur reprend de toute
   façon celui qui était actif).

À regarder dans `~/.local/state/landline/landlined.log` au prochain appel : la ligne `telephony: call audio
handed back to the iPhone (…)` ou `telephony: the call audio is still on this computer (…)`.


## 7. Le son d'un appel passé depuis l'ordinateur (branche `moteur-son-appels`)

### Constat (essai réel du 17 novembre 2026, 16 h 05 à 16 h 08, mode sobre)

Trois appels passés par « Call… », aucun avec du son sur l'ordinateur. Reconstitution à partir de
`landlined.log`, du journal de `bluetoothd`, de `obexd` et de WirePlumber (`journalctl`), et du code.

| Heure | Ce qui s'est passé | Certitude |
|---|---|---|
| 16:05:31 | Premier appel. Le lien classique est ouvert pour lui. L'extension **n'a pas envoyé `audio`** (ou a envoyé `phone`) : aucune ligne `Activate`, et `the sound of calls stays on the iPhone` vient d'être écrit (`RejectSCO` vrai, réglage `call_audio` = `phone`). L'iPhone accepte la composition (33.505) : le son est sur l'iPhone, comme demandé. | établi |
| 16:05:33 → 45 | L'appel **n'est jamais vu** par le moteur. Les sessions OBEX sont ouvertes une seconde après la composition, sur le même lien, et leurs lectures tiennent le fil de supervision : la liste des appels n'est pas relue entre 33.4 et 45.0. À 45.0 l'appel n'existe déjà plus. Le bail `call` est rendu à 45.013 : 10 s d'attente + 1,5 s, sans appel vu. | établi pour le fil retenu et le bail ; la fin de l'appel (raccroché à la main ?) n'est pas dans le journal |
| 16:05:54 | Deuxième appel, cette fois avec `audio:"here"`. Le moteur écrit `RejectSCO` faux puis appelle `Activate` **avant** la composition : PipeWire envoie `AT+BCC`, l'iPhone, qui n'a pas d'appel, répond `ERROR` → `Activate failed … Method call failed`. Sans conséquence : le lien voix est de toute façon ouvert par l'iPhone. L'appel est vu (`dialing`, 55.108) et se termine une seconde plus tard. | établi pour `Activate` ; la fin de l'appel n'est pas expliquée par le journal |
| 16:05:56 → 16:06:06 | La relève du journal d'appels, lancée à la fin de l'appel, n'obtient pas de réponse de l'iPhone en 10 s (`Timed out waiting for response`). | établi |
| 16:06:46 | Troisième tentative, annulée par l'utilisateur au bout d'une seconde (`dial not placed: cancelled`). | établi |
| 16:07:13 | Quatrième appel, sans `audio`. Le lien est rouvert ; la passerelle est vue pour la première fois **avec l'appel déjà dessus** (composition 0,6 s après l'ouverture), donc `RejectSCO` n'est pas écrit (« jamais pendant un appel ») et reste à la valeur d'origine de PipeWire : faux. L'iPhone ouvre le lien voix, PipeWire l'accepte : les nœuds `bluez_input.A0_B1_C2_D3_E4_F5.0` et `bluez_output.….1` existent et tournent (journal de WirePlumber à 16:07:47 : `running -> error`). **Le chemin du son était donc monté sur l'ordinateur**, sans que personne l'ait demandé. | établi |
| 16:07:17 → 47 | À partir de 16:07:17 l'iPhone ne répond plus à rien sur le lien classique : `SetFolder` OBEX sans réponse (10 s), fermeture AVDTP sans réponse (`bluetoothd` : `Close: Connection timed out`, puis `Abort`), plus aucune nouvelle de l'appel. Le lien tombe à 16:07:47 avec `Reason.Unknown`, 30 s après : ce n'est **pas** une fermeture du moteur (aucune ligne `closed on purpose`). L'état `dialing` affiché pendant ces 30 s est celui de la dernière lecture : le fil de supervision était retenu par OBEX. | établi pour les faits ; la cause du gel est une hypothèse (voir plus bas) |

Réponses aux six constats :

- **(a) « stays on the iPhone » à chaque appel** : c'est le réglage `call_audio` (`phone` par défaut) remis
  à chaque nouvelle passerelle. L'extension n'envoie `audio` que si `call_audio_switchable` est vrai, ce
  qui est faux lien classique fermé ; et son choix par défaut est le réglage, donc `phone`. Un appel passé
  lien fermé part donc toujours sans `audio`.
- **(b) `Activate` échoue** : appelé avant que l'appel existe. L'iPhone refuse `AT+BCC` sans appel.
- **(c) l'état reste `dialing`** : la liste des appels est lue sur le fil qui fait aussi les lectures OBEX,
  et celles-ci l'ont retenu 30 s. À quoi s'ajoute, à 16:07, que le lien ne passait plus rien.
- **(d) appel vu 22 s après** : non. Le premier appel n'a jamais été vu ; celui de 16:05:55 est le deuxième,
  vu 0,6 s après sa propre composition.
- **(e) bail rendu avant la fin** : rendu parce qu'aucun appel n'a été vu dans les 10 s, le fil étant retenu.
- **(f) le lien tombe** : ni le moteur ni une commande de fermeture. Le lien a cessé de répondre au moment
  où le lien voix s'établissait, sur une connexion vieille de trois secondes où s'ouvraient en même temps
  deux sessions OBEX, l'AVRCP et un flux A2DP (que le moteur refermait).

### Comment PipeWire 1.6.8 porte le son d'un appel (lu dans `spa/plugins/bluez5/backend-native.c`)

Rôle mains-libres, iPhone en passerelle, négociation de codec (mSBC) :

1. **Le lien voix (SCO) est ouvert par l'iPhone, jamais d'ici.** `sco_listen_event` n'accepte une
   connexion entrante que pour un transport en rôle passerelle ; `sco_do_connect` n'est atteint que par
   `sco_acquire_cb`, que seuls appellent des nœuds qui n'existent qu'une fois le lien voix déjà là.
2. **`Activate`** (`hfp_hf_transport_activate`) envoie `AT+BCC` : « ouvre-moi le lien voix ». L'iPhone
   répond `OK` puis `+BCS` et ouvre le SCO s'il a un appel ; sans appel il répond `ERROR`, que PipeWire rend
   en `org.pipewire.Telephony.Error.Failed: Method call failed`.
3. **Le transport n'existe pas à la connexion** : il est créé au premier `+BCS` de l'iPhone
   (`rfcomm_new_transport`), et c'est à ce moment que le profil mains-libres est déclaré connecté
   (`spa_bt_device_connect_profile`) et que la carte `bluez_card.<adresse>` gagne le profil
   `audio-gateway`. Lien classique ouvert sans appel, la carte n'a que `off` (lu sur la machine).
4. **`RejectSCO` vrai** : la connexion SCO entrante est refermée sans être acceptée
   (`rejecting SCO, AudioGatewayTransport1.RejectSCO=true`). Il ne ferme rien d'ouvert. PipeWire crée
   chaque passerelle avec `RejectSCO` faux.
5. **SCO accepté** : transport `pending`. Les nœuds `bluez_input.<adresse>.0` (voix distante) et
   `bluez_output.<adresse>.1` (micro vers l'iPhone) sont des nœuds « dynamiques »
   (`bluez5-device.c`, `emit_dynamic_node`) : créés quand le transport passe `pending`, **seulement si la
   carte est sur `audio-gateway`**. WirePlumber les relie à la sortie et au micro par défaut ; le nœud
   prend le transport et celui-ci passe `active`.
6. **Carte sur `off`** (bascule « rendre le son à l'iPhone » restée garée, ou profil pas encore choisi) :
   le SCO est quand même accepté, le transport reste `pending`, aucun nœud : l'iPhone croit le son ici et
   **personne ne le joue**. C'est le motif `voice_link_unused`.
7. **États de l'appel** : l'iPhone envoie `+CIEV` (callsetup, call) ; PipeWire répond par `AT+CLCC` et
   publie `dialing` → `alerting` → `active`. Rien d'autre ne les fait avancer : un lien qui ne passe plus
   rien fige l'état.

Donc, pour que le son d'un appel soit ici : `RejectSCO` faux **avant** que l'iPhone offre le lien voix
(avant la composition, avant de décrocher), la carte pas sur `off`, et rien d'autre. `Activate` ne sert
qu'à redemander le lien voix pendant un appel.

### Ce qui a été fait

- **Le son d'un appel passé d'ici va ici par défaut.** Quand `bt_call_dial` ne porte pas `audio`, le
  moteur prend `here` : qui compose depuis l'ordinateur est devant. `LANDLINE_DIAL_AUDIO_DEFAULT=setting`
  rend l'ancien comportement (le réglage `call_audio`), `=phone` force l'iPhone. Le journal dit ce qui a été
  demandé : `dial asked, sound on this computer (client said: nothing)`.
- **Le souhait dure le temps de l'appel** (`CallAudioPolicy::set_once`, `telephony.hpp`). Une passerelle
  qui apparaît entre le choix et l'appel n'est plus remise sur le réglage ; une passerelle vue pour la
  première fois avec un appel dessus est tout de même réglée (cas de 16:07, et de la sonnerie qui se
  jouait ici sur un lien ouvert pour elle).
- **`Activate` au bon moment** (`CallAudioFollowUp`, `call_audio_follow.hpp`). Plus jamais avant l'appel.
  Si l'appel porte du son (`dialing`, `alerting`, `active`, `held`) et que le lien voix n'est pas là 3 s
  après, le moteur vérifie la carte, rouvre `RejectSCO` et appelle `Activate` ; trois fois, à 3 s
  d'intervalle. Puis il publie l'échec : `bt_call_progress` `audio_failed`, `reason` `no_voice_link` ou
  `voice_link_unused`. L'arrivée du son est publiée aussi (`audio_here`).
- **Un appel d'abord, OBEX ensuite.** Pendant une composition, une sonnerie ou un appel : aucune session
  OBEX n'est ouverte (mode sobre), aucune lecture longue ne tourne (les deux modes), le flux A2DP n'est
  pas refermé (il l'est à la fin). La liste des appels est lue quatre fois par seconde au lieu d'une. À la
  fin de l'appel un bail `sync` relit le journal d'appels sur le lien encore ouvert. Un message envoyé
  pendant l'appel passe quand même. Retour arrière : `LANDLINE_OBEX_DURING_CALLS=1`.
- **Un appel accepté et jamais vu est dit** : `bt_call_progress` `no_call` après 20 s (10 s avant), puis le
  bail est rendu. Le bail `call` tient de la demande de composition à la fin de l'appel ; rien dans la
  machine des baux ne ferme le lien sous un appel listé, même bail rendu (testé).
- **Journal** : `telephony: voice link pending (was idle; RejectSCO false)` à chaque mouvement du
  transport, `the sound of the call is on this computer`, `the sound of the call did not come to this
  computer (…)`.

### Tests

`test/test_call_audio_follow.cpp` (32 tests, plus un dans `test_classic_demand.cpp` ; 1 106 au total) : choix par défaut, souhait suivi (arrivée seule, demande
après attente, abandon après trois demandes, son reparti non combattu), interrupteur avec choix d'un appel
(les deux courses du 17 novembre), séquence complète à travers `TelephonyClient` (sortant ici, sortant
iPhone, entrant décroché ici, lien voix non joué, souhait oublié), OBEX qui attend, bail tenu.

### Non vérifiable sans appel réel

1. **Pourquoi le lien a gelé à 16:07:17** (et probablement à 16:05:56). Hypothèse retenue : trop
   d'échanges sur une connexion toute neuve au moment où le lien voix s'établit. Autre hypothèse : la
   topologie du mode sobre — c'est l'ordinateur qui appelle l'iPhone en classique (il y est maître) alors
   que l'iPhone est maître du lien LE ; le contrôleur (Intel 9560) sert alors deux horloges plus un lien
   voix. Le 16 novembre (mode complet, lien ouvert par l'iPhone depuis longtemps) le son passait. Si le
   gel revient avec OBEX à l'arrêt, c'est la seconde : relever `hcitool con` pendant l'appel (protocole
   S1 de `essai-reconnexion.md` §9) et une trace `btmon`.
2. Que l'iPhone offre bien le lien voix dès la composition (`dialing`), et pas seulement une fois l'appel
   décroché ; sinon le son arrive après la première demande, 3 s plus tard.
3. Que `AT+BCC` est accepté par l'iPhone pendant `dialing` / `alerting`.
4. Que WirePlumber met la carte sur `audio-gateway` de lui-même quand le profil apparaît (vu une fois, à
   16:07 ; pas vu avec une carte garée sur `off`).
5. Les premiers appels de 16:05 : par qui et pourquoi ils ont pris fin.

### Ce que l'extension doit changer

- Envoyer `audio` avec `bt_call_dial` **même quand `call_audio_switchable` est faux** (lien fermé, appels
  `on_demand`) : aujourd'hui le choix de la carte de composition n'est pas transmis dans ce cas.
- Proposer « This computer » par défaut dans la carte de composition, quel que soit `call_audio`.
- Lire `bt_call_progress` `audio_here`, `audio_failed` (+ `reason`) et `no_call` : aujourd'hui les étapes
  inconnues sont ignorées, ce qui ne casse rien.
- `calls.audio_wanted` (`here`, `phone`, `null`) dit ce qui a été demandé pour l'appel ; `audio_route`
  reste la vérité.
