# Réglages système et installation locale

Ce que le moteur exige du système, comment `system/landline-setup` le met en place et l'enlève, et
comment installer moteur et extension dans le dossier de l'utilisateur. Complète
`moteur-inventaire.md` (section 5) et `moteur-api.md` (section 11).

Cette version de l'assistant (3) n'a pas été appliquée sur la machine : tout a été écrit et essayé à
blanc (racine factice, HOME factice, `--dry-run`) ; seul `status` y a été lu. La procédure d'essai réelle est en fin de document.

## 1. Les deux réglages, en clair

| Réglage | Ce que c'est | Sans lui | Tient tout seul ? |
|---|---|---|---|
| Mode expérimental | `bluetoothd` lancé avec `--experimental` | pas de moitié Bluetooth LE dans le lien avec l'iPhone, donc **pas de notifications**. À activer **avant** l'appairage. **Non requis à partir de BlueZ 5.88**, où l'API Bearer sort du mode expérimental | oui, une fois écrit dans la configuration de systemd |
| ERTM | un mode de transport L2CAP du noyau (`disable_ertm = N`) | `obexd` ne peut ouvrir ni les messages (MAP) ni les contacts (PBAP) | dépend de la machine : ici `/etc/sysfs.conf` le coupe à chaque démarrage |

**La classe d'appareil n'est plus exigée** (passe 4, 16 novembre 2026). Constat avec l'iPhone de l'essai
(iOS 26) : appairage fait avec la classe d'origine de l'ordinateur (`0x7c0000`), obexd actif ; l'iPhone a
proposé et accordé contacts, messages, notifications et appels. L'assistant ne la pose plus ; le moteur
ne la compte plus parmi les étapes (`capability.setup`) et ne prévient plus à l'appairage. Elle reste
publiée pour information : `device_class` dans `status`, `class_ok` dans `capability`.

Le moteur ne règle rien lui-même : il constate et le signale dans `capability.setup`. L'assistant fait
les mêmes constats sans le démon. Écart connu : le moteur exige encore le mode expérimental quelle que
soit la version de BlueZ (il juge sur la présence de `Bearer.LE1`, ce qui reste juste).

## 2. L'assistant `landline-setup`

Un seul fichier bash, sans dépendance hors BlueZ et systemd.

| Commande | Droits | Effet |
|---|---|---|
| `status` | aucun | une ligne JSON sur la sortie standard, code 0 |
| `apply [--dry-run]` | administrateur | rend les réglages exigés effectifs maintenant et persistants ; retire ce qu'une version précédente avait posé pour la classe d'appareil |
| `revert [--dry-run]` | administrateur | retire tout ce qu'`apply` a créé |
| `version` | aucun | numéro de version de l'assistant (3 depuis le nom « landline » ; 2 quand la classe n'a plus été posée) |

Codes de sortie : 0 réussi ; 1 droits manquants ou erreur ; 2 fichiers écrits mais une étape
d'activation a échoué (ou `revert` a laissé un fichier qu'il ne reconnaît pas) ; 64 mauvais usage.

`--dry-run` fonctionne sans droits : il lit l'état réel et affiche chaque fichier qui serait écrit (avec
son contenu), chaque lien, chaque commande. Rien n'est modifié.

### `status`

```json
{"experimental":true,"device_class":false,"ertm":true,"ready":true,
 "persistent":{"experimental":true,"device_class":true,"ertm":true},
 "required":{"experimental":true,"ertm":true},"bluez_version":"5.87"}
```

Le premier niveau est l'état **courant** ; `persistent` dit si chaque réglage tiendra au prochain
démarrage ; `required` dit ce que cette machine exige. L'exemple est la lecture de cette machine le
16 novembre 2026 (lecture seule) : classe d'origine, tout le reste en place, et l'ancienne unité de
classe encore installée (`persistent.device_class`), qu'un prochain `apply` ou `revert` retirera.

| Clé | Courant | Persistant |
|---|---|---|
| `experimental` | ligne de commande du `bluetoothd` en cours (`/proc/<pid>/cmdline`), ou `Experimental = true` dans `main.conf` | `ExecStart` effectif de `bluetooth.service` (unité et fichiers d'appoint, lus dans l'ordre de systemd), ou `main.conf` |
| `ertm` | `/sys/module/bluetooth/parameters/disable_ertm` ni `Y` ni `1` | unité `landline-ertm.service` activée, **ou** aucune configuration ne coupe l'ERTM (`/etc/sysfs.conf`, `/etc/sysfs.d/`, `modprobe.d`, ligne de commande du noyau) |
| `device_class` (information) | `btmgmt info` : `(classe & 0x1fff) == 0x0408` sur tous les adaptateurs allumés | une unité de classe d'une version précédente (ou celle de Tether) est encore installée |

- `required.experimental` : faux quand la version de BlueZ lue vaut 5.88 ou plus (`experimental_required()`).
  La version est lue auprès des programmes (`bluetoothctl --version`, à défaut `bluetoothd --version`),
  jamais supposée ; illisible, le mode expérimental reste exigé. `bluez_version` la publie (`""` si
  illisible). Sur cette machine : 5.87, donc exigé.
- `required.ertm` : toujours vrai.
- `ready` = ERTM actif **et** (mode expérimental actif **ou** non requis). La classe n'y entre plus.

L'extension propose l'assistant quand, pour un réglage **exigé**, la valeur courante ou la valeur
persistante est fausse. Une machine où l'ERTM est actif et persistant et où le mode expérimental n'est
pas requis donne `ready:true` et rien n'est demandé.

### Ce que crée `apply`, fichier par fichier

Tout porte le préfixe `landline` et l'en-tête « Managed by landline-setup » (en anglais : ces fichiers
restent sur le système de l'utilisateur). Le préfixe vient d'une seule variable en tête du script
(`NAME`). Aucun fichier existant n'est modifié.

| Fichier | Rôle |
|---|---|
| `/usr/local/libexec/landline/landline-setup` | copie de l'assistant, appartenant à root : c'est elle que la fiche polkit désigne, et elle reste disponible pour `revert` même si l'extension est désinstallée |
| `/usr/share/polkit-1/actions/io.github.d101c.landline.policy` | fiche polkit : message de la fenêtre de mot de passe |
| `/etc/systemd/system/bluetooth.service.d/landline-experimental.conf` | **seulement si le mode expérimental est requis et que rien ne l'active déjà.** Reprend la ligne `ExecStart` existante de `bluetooth.service` (chemin de `bluetoothd` et options compris, jamais en dur) et ajoute `--experimental` |
| `/etc/systemd/system/landline-ertm.service` | unité `oneshot` ordonnée après `sysfsutils.service` : charge le module `bluetooth` si besoin et écrit `N` |
| `/etc/systemd/system/multi-user.target.wants/landline-ertm.service` | lien d'activation |

**Sur cette machine**, le fichier d'appoint `experimental.conf` posé à la main suffit déjà : `apply` ne
crée pas `landline-experimental.conf` et **ne redémarre pas** `bluetoothd`.

**Installation sous l'ancien nom (`iphone-link`).** Les premières installations écrivaient
`iphone-link-experimental.conf`, `iphone-link-ertm.service` et son lien, la copie
`/usr/local/libexec/iphone-link/iphone-link-setup`, son témoin `.parent-created` et la fiche
`io.github.d101c.iphone-link.policy`, avec l'en-tête « géré par iphone-link ». `status` les compte tant
qu'ils sont là (l'ancienne unité ERTM vaut la nouvelle : pas de « Permission needed » après la
migration). `apply` écrit les fichiers au nouveau nom **puis** retire les anciens (section « Installation
sous l'ancien nom » de sa sortie) : le fichier d'appoint est jugé sans l'ancien dès le début, si bien
que le nouveau le remplace sans que `bluetoothd` soit redémarré. `revert` retire les deux générations.
Un fichier de l'ancien nom qui ne porte pas l'en-tête est laissé et signalé (code 2).

**Restes de la classe d'appareil.** `apply` retire, s'ils existent et portent l'en-tête, l'unité
`iphone-link-btclass.service`, son lien d'activation, la règle udev `iphone-link-btclass.rules` et
l'auxiliaire `iphone-link-btclass` (après `systemctl stop` de l'unité). `bluetoothd` n'est pas
redémarré pour cela : la classe d'origine revient à son prochain démarrage.

Commandes lancées, dans l'ordre : `systemctl stop iphone-link-btclass.service` (seulement s'il y a des
restes), `systemctl daemon-reload` et `udevadm control --reload` (si des fichiers ont changé),
`systemctl restart landline-ertm.service`, puis `systemctl restart bluetooth.service` seulement si le
mode expérimental est requis et que le `bluetoothd` en cours ne l'a pas. La dernière ligne de la sortie
est le JSON de `status`.

`apply` est idempotent : relancé, il n'écrit rien, ne recharge rien, ne redémarre rien.

**Redémarrage de `bluetoothd`.** Il coupe clavier, souris et casques Bluetooth quelques secondes.
`apply` ne le fait que si le mode expérimental est requis et pas déjà en service, et l'annonce sur la
sortie (« REDÉMARRAGE DU BLUETOOTH… »). L'extension le dit **avant** la demande de mot de passe quand
`status.experimental` est faux et `required.experimental` vrai.

### `revert`

Retire les liens, l'unité ERTM, le fichier d'appoint, la copie de l'assistant et la fiche polkit, ainsi
que les restes de classe d'une version précédente (unité, lien, règle udev, auxiliaire) ; retire les dossiers qu'`apply` avait créés s'ils sont vides. Un fichier du même nom
qui ne porte pas l'en-tête est laissé en place et signalé (code 2).

- **ERTM** : remis à la valeur que dicte la configuration du système (`/etc/sysfs.conf` d'abord, puis
  `modprobe.d`, puis la ligne de commande du noyau). Ici : `1`, comme avant. Si rien ne la fixe, elle
  est laissée telle quelle.
- **Mode expérimental** : `bluetoothd` n'est redémarré que si c'est `iphone-link` qui l'avait activé. Ici
  le fichier `experimental.conf` de l'utilisateur n'est pas touché, donc pas de redémarrage.
- **Classe** : rien n'est réécrit. Si une version précédente l'avait posée, BlueZ reprend sa classe
  d'origine à son prochain démarrage.

`revert` peut être lancé depuis la copie installée : elle se retire elle-même.

### Racine factice

`LANDLINE_ROOT=<dossier>` redirige toutes les lectures et écritures sous ce dossier, n'exige plus les
droits administrateur et ne lance aucune commande système : seules les doublures exécutables placées
dans `<dossier>/.stubs/` sont appelées, et les commandes demandées sont consignées dans
`<dossier>/.commands.log`. C'est ce qu'utilise `system/tests/run.sh`.

## 3. Fenêtre de mot de passe : polkit et `pkexec`

`pkexec <programme> …` cherche une action polkit dont l'annotation
`org.freedesktop.policykit.exec.path` vaut exactement le chemin du programme. S'il en trouve une, la
fenêtre affiche le message de cette action ; sinon il retombe sur l'action générique
`org.freedesktop.policykit.exec` (« Authentification requise pour exécuter … en tant que
super-utilisateur », avec le chemin).

La fiche `system/io.github.d101c.landline.policy` déclare l'action
`io.github.d101c.landline.setup`, message « Change Bluetooth settings so this computer can link to
an iPhone », annotée pour `/usr/local/libexec/landline/landline-setup`. Elle s'installe dans
`/usr/share/polkit-1/actions/` (seul dossier lu par toutes les versions de polkit). L'assistant en porte
une copie et l'installe lui-même ; un essai vérifie que les deux sont identiques.

| Cas | Commande de l'extension | Fenêtre |
|---|---|---|
| Première fois, rien d'installé chez root | **aucune** : l'extension n'élève jamais un fichier modifiable par l'utilisateur. Le parcours montre la commande à lancer soi-même, `sudo ~/.local/lib/landline/landline-setup apply`, et avance seul quand c'est fait | celle de `sudo`, dans le terminal |
| Assistant déjà installé (après un premier `apply`, ou par un paquet) | `pkexec /usr/local/libexec/landline/landline-setup apply` | message propre |

Le premier `apply` s'installe lui-même (copie root + fiche) : les fois suivantes, le message est propre.
`revert` retire les deux.

**Pour l'extension** (`extension/lib/setupHelper.js`) : `pkexec` n'est lancé que sur
`/usr/local/libexec/landline/landline-setup`, et seulement si ce fichier appartient à root et n'est
inscriptible ni par le groupe ni par les autres. L'assistant n'est pas dans l'archive de l'extension.
`status` est lu sur cette copie, à défaut sur `~/.local/lib/landline/landline-setup`.

Pourquoi un emplacement root : ni polkit ni l'extension ne doivent élever un fichier que
l'utilisateur (ou un programme à son nom) peut réécrire.

Un paquet de distribution installerait directement les deux fichiers ; l'annotation devrait alors suivre
le chemin choisi par le paquet.

## 4. La manette Xbox et l'ERTM

La ligne 24 de `/etc/sysfs.conf` (`/module/bluetooth/parameters/disable_ertm=1`) coupe l'ERTM à chaque
démarrage : c'était le contournement classique pour appairer une manette Xbox en Bluetooth. Le moteur a
besoin du contraire. Les deux réglages portent sur le même interrupteur du noyau, commun à tous les
appareils : on ne peut pas avoir l'un pour la manette et l'autre pour l'iPhone en même temps.

Choix fait : **ne pas toucher à `/etc/sysfs.conf`**. L'unité `landline-ertm.service` passe après
`sysfsutils` et écrit `N`. Tant qu'elle est installée, l'ERTM est actif ; `revert` rend la main à
`sysfs.conf`.

Conséquence à dire à l'utilisateur, avant `apply` quand `status.persistent.ertm` est faux :

> Les messages et les contacts de l'iPhone demandent un réglage Bluetooth (ERTM) que cette machine
> désactive, sans doute pour une manette Xbox. Landline va le réactiver. Les manettes Xbox récentes
> ou à jour fonctionnent avec ; une manette ancienne peut ne plus se connecter en Bluetooth. Si cela
> arrive : mettre à jour le microprogramme de la manette, la brancher en USB, ou annuler ce réglage
> depuis les préférences de Landline.

Le contournement date d'anciens microprogrammes et d'anciens noyaux ; le noyau d'ici est récent (7.3).
L'essai à faire est dans la procédure ci-dessous (étape 7). Le paramètre ne vaut que pour les
connexions ouvertes après son écriture.

## 5. Installation locale (sans droits administrateur)

`scripts/install-local.sh` puis, pour défaire, `scripts/uninstall-local.sh`. Options : `--dry-run`,
`--no-activate` ; `--purge` pour la désinstallation. Les noms viennent de `scripts/noms.sh`.

| Posé | Contenu |
|---|---|
| `~/.local/lib/landline/engine/` | `landlined`, `landlinectl`, `LICENSE` (MIT, Tether) |
| `~/.local/lib/landline/engine/lib/` | bibliothèques hors système, s'il y en a |
| `~/.local/lib/landline/engine/locale/` | traductions du moteur |
| `~/.config/systemd/user/landline-engine.service` | unité utilisateur |
| `~/.local/share/gnome-shell/extensions/<uuid>/` | copie de `extension/`, uuid lu dans `metadata.json` |
| `~/.local/lib/landline/landline-setup` | l'assistant, **hors du dossier de l'extension** : `status` y est lu sans droits ; `apply` se lance à la main avec `sudo` la première fois |

Puis `systemctl --user enable --now landline-engine.service` et `gnome-extensions enable <uuid>`.
Sous Wayland, GNOME Shell ne voit une extension nouvellement copiée qu'à la session suivante : le
script le dit si l'activation échoue. Si `HOME` n'est pas le vrai dossier de l'utilisateur, rien n'est
démarré, arrêté ni activé.

### Migration d'une installation sous les anciens noms

Avant de poser quoi que ce soit, `install-local.sh` reprend une installation faite quand l'extension et
le service s'appelaient `iphone-link` et que le moteur gardait les noms de Tether :

| Ancien | Devient |
|---|---|
| service `iphone-link-engine.service` | arrêté et désactivé (`systemctl --user disable --now`), unité et lien retirés |
| extension `iphone-link@d101c.github.io` | désactivée, réglages recopiés (`dconf dump` de `/org/gnome/shell/extensions/iphone-link/` vers le nouveau chemin, puis remise à zéro de l'ancien), dossier retiré |
| `~/.config/tether`, `~/.local/share/tether`, `~/.local/state/tether` | déplacés vers `…/landline` ; jamais par-dessus un dossier existant (l'ancien est alors laissé, et c'est dit) |
| `~/.local/state/…/tetherd.log` | renommé `landlined.log` |
| `~/.config/wireplumber/wireplumber.conf.d/zz-tether-phone-audio.conf` | renommé `zz-landline-phone-audio.conf` |
| `~/.local/lib/iphone-link` | retiré |

L'ancien moteur est arrêté **avant** le déplacement de ses dossiers ; avec `--no-activate` dans le vrai
dossier de l'utilisateur, le script refuse de continuer si `tetherd` tourne encore.

**La clé du trousseau** (celle qui chiffre les messages et les contacts gardés) n'est pas déplacée par
le script : `secret-tool` ne sait pas écrire une entrée sous un nom de schéma donné de façon fiable.
C'est le moteur qui la reprend, à son premier démarrage sous le nouveau nom
(`engine/src/core/src/secret_store.cpp`) : s'il ne trouve rien sous `io.github.d101c.landline.Store` /
`application=landline`, il lit l'entrée `com.tether.Store` / `application=tether`, la recopie sous le
nouveau nom et laisse l'ancienne en place. Si cette reprise échouait (trousseau verrouillé : le moteur
attend ; entrée absente : il crée une clé neuve), la conséquence serait limitée : les messages et
contacts gardés sur le disque deviendraient illisibles et seraient relus depuis l'iPhone. **L'appairage
ne dépend d'aucun de ces noms** : il vit dans BlueZ (`/var/lib/bluetooth`), et l'iPhone choisi est dans
`bluetooth.json`, qui suit le dossier déplacé. Cette reprise n'a pas été essayée avec un vrai trousseau
(les tests du moteur coupent le bus de session).

Les réglages système posés sous l'ancien nom restent valables ; `sudo ~/.local/lib/landline/landline-setup
apply` les reprend sous le nouveau (section 2).

### Bibliothèques liées

`scripts/build-engine.sh` compile avec un `RUNPATH` vers le sysroot du banc d'essai quand il existe. Le
script d'installation passe chaque binaire à `ldd`, copie dans `engine/lib/` toute bibliothèque prise
hors de `/lib` et `/usr/lib` (aucune aujourd'hui : `landlined` et `landlinectl` ne dépendent que du
système ; la seule qui l'exigeait, `libgtk-layer-shell`, servait à `tether-dialog`, qui n'est plus
compilé), puis vérifie que les copies ne dépendent plus que du système et de `engine/lib/`. Le `RUNPATH`
reste inscrit dans les binaires : avec `patchelf` il est remplacé par `$ORIGIN/lib` ; sinon l'unité pose
`LD_LIBRARY_PATH`, que l'éditeur de liens consulte avant le `RUNPATH`.

Ne sont plus compilés ni installés : les interfaces de Tether (`tether-gtk`, `tether-tui`,
`tether-dialog` ; option CMake `LANDLINE_UPSTREAM_FRONTENDS`, éteinte), la caméra (`TETHER_CAMERA=OFF`),
les extensions de navigateur. L'hôte natif de navigateur (`tether-native-host`, un script) est encore
généré par CMake mais jamais installé.

### L'unité utilisateur

`system/landline-engine.service` lance `%h/.local/lib/landline/engine/landlined`, liée à
`graphical-session.target`, `Restart=on-failure`. Ce dont le démon a besoin au lancement (lu dans
`engine/src`) : `XDG_RUNTIME_DIR` (socket et verrou d'instance unique dans `$XDG_RUNTIME_DIR/landline/`),
le bus de session (obexd) et le bus système (BlueZ), `btmgmt` dans le `PATH`
(`bluetooth/monitor.cpp`), `HOME`. Il ne lit aucun argument et écrit lui-même son journal dans
`~/.local/state/landline/landlined.log`.

Avant de démarrer, l'unité arrête un éventuel `landlined` lancé à la main, qui tiendrait le verrou
(`landlinectl` n'en démarre plus de lui-même quand l'unité est activée : `client.cpp`). Si le verrou reste pris, le démon sort en erreur et systemd
renonce après cinq essais en une minute.

### Désinstallation

`uninstall-local.sh` arrête et désactive l'unité et l'extension, retire les fichiers ci-dessus.
`--purge` retire aussi `~/.config/landline`, `~/.local/share/landline` et `~/.local/state/landline` (appareils
appairés, messages, journal). Les réglages système ne sont pas touchés : le script rappelle la commande
`sudo /usr/local/libexec/landline/landline-setup revert`.

## 6. Essais à blanc

| Commande | Ce qu'elle couvre |
|---|---|
| `system/tests/run.sh` | 136 vérifications : `status`, `apply`, `revert`, `--dry-run` dans des racines factices, avec doublures de `systemctl`, `btmgmt`, `udevadm`, `bluetoothctl` : machine vierge, machine déjà réglée à la main, idempotence, redémarrage simulé, retour à l'arborescence d'origine, cas particuliers (chemin Arch, options existantes, `main.conf`, fichier d'appoint tiers, `modprobe.d`, Bluetooth éteint), classe publiée sans effet sur `ready`, restes d'une version précédente retirés par `apply` et par `revert`, installation sous l'ancien nom reprise par `apply` et retirée par `revert`, versions de BlueZ (5.87 exigé, 5.88 et après non, illisible exigé, lecture de secours par `bluetoothd`), machine où il n'y a rien à demander, `systemd-analyze verify` |
| `system/tests/install.sh` | 70 vérifications : installation et désinstallation dans un HOME factice, migration d'une installation sous les anciens noms (à blanc, pour de bon, deux fois, dossier déjà présent), relevé `ldd` des copies, `systemd-analyze verify --user` de l'unité. Demande un moteur compilé (`BUILD_DIR`) |

Les deux lancent `shellcheck` s'il est installé, et le signalent sinon.

## 7. Limites connues

- **Rien n'a tourné en root.** Les unités et la règle udev sont vérifiées en syntaxe, pas en
  conditions réelles : ordre réel par rapport à `sysfsutils`, relance par `PartOf=` après un plantage de
  `bluetoothd`, déclenchement de la règle udev, fenêtre polkit.
- **`shellcheck` n'est pas installé sur cette machine** : l'analyse statique n'a pas été faite.
- **BlueZ 5.88** : la règle « mode expérimental non requis » repose sur l'annonce que l'API Bearer y est
  stable ; elle n'a été essayée sur aucune machine en 5.88 (celle-ci est en 5.87), seulement avec une
  version simulée. Le moteur, lui, n'a pas cette règle.
- **`Experimental = true` dans `main.conf`** est compté comme actif même si `bluetoothd` n'a pas été
  redémarré depuis l'édition : l'assistant ne peut pas le savoir.
- **Fichiers d'appoint** : les lignes `ExecStart` coupées par `\` ne sont pas comprises. Si un fichier
  d'appoint trié après `landline-experimental.conf` redéfinit `ExecStart` sans `--experimental`,
  `apply` s'arrête avec une erreur plutôt que de prétendre avoir réussi.
- **Systèmes sans systemd** : non couverts.
- **Mode expérimental activé après l'appairage** : l'iPhone doit être oublié et appairé de nouveau.

## 8. Procédure d'essai réelle, à dérouler avec l'utilisateur

À faire clavier et souris filaires à portée, ou en acceptant quelques secondes de coupure.

1. **Relevé avant.** `system/landline-setup status` ; `system/landline-setup apply --dry-run`.
   Vérifier que la liste des fichiers est celle de la section 2, que le retrait de l'ancienne unité de
   classe est annoncé et qu'aucun redémarrage de `bluetoothd` ne l'est.
2. **Application.** `sudo system/landline-setup apply`. Attendu : code 0, dernière ligne avec
   `ready:true`, `persistent.experimental` et `persistent.ertm` à `true`, `persistent.device_class` à
   `false`, pas de coupure du clavier ni de la souris.
3. **Contrôles.** `systemctl status landline-ertm.service` (actif, sorti en 0) ;
   `cat /sys/module/bluetooth/parameters/disable_ertm` (`N`) ; `pkaction --action-id
   io.github.d101c.landline.setup --verbose` ; plus aucun fichier `iphone-link-btclass*`.
4. **Redémarrage de BlueZ seul.** `sudo systemctl restart bluetooth` puis `bluetoothctl show | grep
   Class` : la classe d'origine (`0x7c0000` ici), et l'iPhone se relie toujours.
5. **Redémarrage de la machine.** Puis `status` sans rien toucher : `ready:true`. C'est l'essai qui
   compte : il confirme que l'unité ERTM passe bien après `sysfsutils`.
6. **iPhone.** Relier, ouvrir la fiche ⓘ : les interrupteurs de messages, contacts et notifications
   sont proposés avec la classe d'origine (constaté le 16 novembre 2026 ; à refaire après le point 4).
7. **Manette Xbox.** ERTM actif, connecter la manette en Bluetooth et jouer quelques minutes. Si elle
   fonctionne, la ligne 24 de `/etc/sysfs.conf` n'a plus de raison d'être (à l'utilisateur de la
   retirer) ; sinon, garder le message de la section 4 et envisager la bascule à la demande décrite dans
   `moteur-inventaire.md`.
8. **Fenêtre polkit.** `pkexec /usr/local/libexec/landline/landline-setup apply` : la fenêtre doit
   afficher le message de la fiche, et la commande ne rien changer (idempotence).
9. **Installation locale.** `scripts/install-local.sh --dry-run`, puis sans l'option ; fermer et rouvrir
   la session ; `systemctl --user status landline-engine.service` ; essai de bout en bout avec
   l'iPhone (`essai-reconnexion.md`).
10. **Retour arrière.** `sudo /usr/local/libexec/landline/landline-setup revert`, puis `status` :
    `persistent.ertm` à `false`, `disable_ertm` à `Y`, aucun fichier
    `iphone-link*` sous `/etc`, `/usr/local/libexec` ni `/usr/share/polkit-1/actions`
    (`find /etc /usr/local /usr/share/polkit-1 -name '*iphone-link*'`). Réappliquer ensuite si l'on
    garde l'installation.
