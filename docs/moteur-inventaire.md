# Moteur : inventaire du code repris

> **Document d'origine, gardé tel quel.** Il décrit le code de Tether au moment de la reprise, sous ses
> noms d'alors (`tetherd`, `tether`, dossiers `tether/`). Depuis, le moteur est installé sous les noms
> de Landline (`landlined`, `landlinectl`, dossiers `landline/`, variables `LANDLINE_…` : voir
> `engine/src/core/include/tether/identity.hpp` et la section 4.7), les interfaces GTK, terminal et la
> fenêtre d'appairage ne sont plus compilées, la caméra non plus.

État de `engine/` à la reprise (Tether, commit `9ca3c46`, version 0.3.5). Chemins relatifs à `engine/`.
Rien n'a été supprimé ni renommé. Tailles en lignes, arrondies ; elles servent à situer, pas à mesurer.

## 1. Vue d'ensemble

| Dossier | Contenu | Fichiers | Lignes | Sort |
|---|---|---|---|---|
| `src/core/` | bibliothèque statique `tether` : tout le moteur | 108 | ≈ 40 000 (dont 10 300 de table de données et 6 000 de code Wayland généré) | cœur à garder, à élaguer |
| `src/daemon/` | `tetherd` : assemblage, bulles de bureau, caméra | 7 | 1 850 | à garder, à alléger |
| `src/cli/` | `tether` : ligne de commande | 4 | 2 100 | à garder (outil de diagnostic) |
| `src/gtk/` | `tether-gtk` : interface GTK 3 | 71 | 7 000 | à retirer |
| `src/tui/` | `tether-tui` : interface ncurses | 11 | 4 600 | à retirer |
| `src/dialog/` | `tether-dialog` : fenêtre de confirmation | 2 | 225 | à retirer |
| `src/ui_common/` | mise en forme partagée par GTK et TUI | 5 | 460 | à retirer avec elles |
| `test/` | tests unitaires (GoogleTest), 627 cas | 43 | 10 900 | à garder, à élaguer en parallèle |
| `apple/` | application iOS (Swift, Xcode) | 63 | 6 500 | à retirer |
| `extension/` | extensions navigateur et courrielleur + hôte natif | 40 | 12 600 | à retirer |
| `po/` | traductions gettext (une vingtaine de langues) | 21 | 52 900 | à trier : beaucoup de chaînes disparaîtront |
| `packaging/` | AppImage, Flatpak, RPM, conteneur, pare-feu, unités systemd | 16 | 300 | à retirer, sauf `packaging/systemd/` |
| `docs/` | documentation amont | 21 | 5 700 | à garder comme référence |
| `scripts/` | sonde Bluetooth, remise à zéro, publication | 7 | 970 | garder `bt-probe.sh` et `tether-reset.sh` |
| `nix/`, `flake.*`, `PKGBUILD`, `.copr/`, `.github/`, `Makefile`, `CMakePresets.json` | empaquetage et intégration continue amont | — | — | à retirer |

`docs/BLUETOOTH.md` (4 068 lignes) est le journal de bord de l'auteur : chaque panne rencontrée, sa cause
mesurée et sa correction. C'est la partie la plus précieuse de la reprise après le code lui-même ; à lire
avant de toucher à la supervision des liens.

## 2. Le cœur Bluetooth, à garder

Tout est dans `src/core/src/bluetooth/` (29 fichiers, 12 600 lignes) et ses en-têtes
`src/core/include/tether/bluetooth/`.

| Groupe | Fichiers | Lignes | Rôle |
|---|---|---|---|
| Supervision des liens | `connection.cpp` (1 721), `bearer_supervisor.cpp` (284), `profile_supervisor.cpp` (253) | ≈ 2 600 avec en-têtes | boucle principale, maintien de BR/EDR et LE, sessions OBEX. Voir `moteur-reconnexion.md` |
| Observation de BlueZ | `monitor.cpp` (629), `objects.cpp` (815) | ≈ 1 800 | instantané des objets BlueZ, calcul de ce que la machine sait faire, **prérequis système** |
| Appairage | `pairing.cpp` (970), `agent.cpp` (312), `advert.cpp` (225) | ≈ 1 700 | transaction d'appairage, agent, annonce LE de sollicitation |
| Notifications (ANCS) | `ancs/client.cpp` (747), `ancs/notifications.cpp` (254), `ancs/sequencer.cpp` (153), `ancs/parser.cpp` (144) | ≈ 1 700 avec en-têtes | client GATT, file de requêtes, registre des notifications |
| Messages (MAP) | `map_session.cpp` (413), `bmessage.cpp` (542), `messages.cpp` (248), `journal.cpp` (335), `removed_threads.cpp` (165), `groups.cpp` (356), `obex_transfer.cpp` (82) | ≈ 2 700 | lecture, envoi, fils, journal chiffré sur disque, conversations de groupe |
| Contacts (PBAP) | `pbap_session.cpp` (159), `vcard.cpp` (190), `contacts.cpp` (356) | ≈ 870 | carnet, favoris, journal d'appels |
| Appels | `telephony.cpp` (324), `telephony_pipewire.cpp` (236) | ≈ 700 | pilotage du mains-libres de BlueZ ou de PipeWire |
| Son de l'iPhone | `phone_audio.cpp` (266) | ≈ 320 | où joue la musique de l'iPhone (A2DP) |
| Réglages et diagnostic | `config.cpp` (196), `diagnostics.cpp` (298) | ≈ 600 | `bluetooth.json`, rapport anonymisé |

Socle dont ce cœur dépend, à garder aussi (≈ 1 400 lignes) : `event_loop.cpp` (boucle epoll),
`paths.cpp`, `secret_store.cpp` (clé de chiffrement dans le trousseau, via libsecret), `session_lock.cpp`
(verrouillage et détection de session inactive), `service.cpp` (unité systemd utilisateur), `i18n.cpp`,
`base64.cpp`, `log.hpp`.

Détection de codes à usage unique : `otp.cpp` (363 lignes) est utile seul (codes extraits des messages et
des notifications). Sa table `psl_data.hpp` (10 260 lignes, liste des suffixes publics) ne sert qu'à
rapprocher un code du site ouvert dans le navigateur : elle part avec les extensions navigateur.

## 3. Ce qui est à retirer plus tard

| Élément | Où | Lignes | Pourquoi |
|---|---|---|---|
| Interface GTK | `src/gtk/` | 7 000 | remplacée par l'extension GNOME |
| Interface terminal | `src/tui/` | 4 600 | idem |
| Fenêtre de confirmation | `src/dialog/` | 225 | la confirmation passera par l'extension ; inutilisable proprement sous Mutter (pas de `layer-shell`) |
| Mise en forme commune | `src/ui_common/` | 460 | ne sert qu'à GTK et TUI |
| Partie Wi-Fi / mTLS | `src/core/src/net.cpp` (classe `TcpServer`, lignes 1881-2849, et une partie du début), `crypto.cpp`, `discovery.cpp`, `file_transfer.cpp`, `client.cpp` (moitié TLS) | ≈ 3 500 | canal réseau vers l'application iOS : appairage TLS, mDNS, transfert de fichiers, ouverture de liens |
| Service GATT « adresses réseau » | `src/core/src/bluetooth/lan_info.cpp` | 385 | sert seulement à dire à l'application iOS où joindre l'ordinateur en Wi-Fi |
| Application iOS | `apple/` | 6 500 | hors sujet : on n'installe rien sur l'iPhone |
| Extensions navigateur et courrielleur | `extension/`, `src/core/src/extension_host.cpp`, `psl_data.hpp` | 12 600 + 190 + 10 260 | remplissage automatique des codes dans le navigateur |
| Presse-papiers | `src/core/src/wayland.cpp`, `src/core/src/wayland_protocols/`, `clipboard.hpp` | 600 + 6 000 | ne fonctionne pas sous GNOME (protocole `wlr-data-control` absent) et suppose le canal Wi-Fi |
| Caméra | `src/daemon/camera.cpp`, `camera_frame.hpp` | 350 | flux vidéo venant de l'application iOS |
| AirPods | `src/core/src/bluetooth/airpods.cpp` (1 547), `audio.cpp` (231), `mpris.cpp` (307), et 330 lignes de `src/daemon/main.cpp` | ≈ 2 800 | batterie et passage de relais d'AirPods reliés à l'ordinateur. Hors périmètre ; à décider (voir 4.6) |
| Bulles de bureau du démon | `src/daemon/notification.cpp` | 610 | l'extension affichera les notifications |
| Empaquetage amont | `packaging/` (sauf `systemd/`), `nix/`, `flake.*`, `PKGBUILD`, `.copr/`, `.github/` | — | propre à Tether |

Ordre de grandeur : sur environ 67 000 lignes de C++ (table de données et code généré compris), le cœur
utile à l'extension en représente à peu près 16 000, tests non comptés.

Tests à retirer en même temps que leur module : `test_net.cpp`, `test_crypto.cpp`, `test_discovery.cpp`,
`test_file_transfer.cpp`, `test_extension_host.cpp`, `test_clipboard.cpp`, `test_camera_frame.cpp`,
`test_lan_info.cpp`, `test_tui_util.cpp`, `test_display_width.cpp`, `test_message_format.cpp`,
`test_airpods.cpp`, `test_audio.cpp`. `test/CMakeLists.txt` compile aussi directement
`src/ui_common/*.cpp`, `src/tui/util.cpp` et `src/cli/verbs.cpp` (lignes 39-42).

## 4. Dépendances qui compliquent le retrait

Les interfaces (`gtk`, `tui`, `dialog`) sont des exécutables séparés : les retirer, c'est enlever une ligne
`add_subdirectory` dans `src/CMakeLists.txt` — à une exception près (4.2). Le reste est moins simple, parce
que **tout le moteur est une seule bibliothèque** (`src/core/CMakeLists.txt`) sans frontière interne entre
Bluetooth et Wi-Fi.

### 4.1 `net.cpp` mélange le socket local et le réseau

`src/core/src/net.cpp` (2 849 lignes) contient à la fois :

- le serveur du socket Unix et **toutes les commandes `bt_*`** (`UnixServer`, lignes 1188-1880) — à garder ;
- le serveur TCP/TLS, l'appairage réseau, la diffusion vers les pairs (`TcpServer`, lignes 1881-2849) — à
  retirer ;
- des fonctions communes aux deux : écritures sur socket, diffusion locale (`broadcast_local_event`,
  ligne 296), répertoires (`get_runtime_dir`, ligne 1136), verrou d'instance unique, état des codes à usage
  unique, pare-feu.

Liens à défaire :

- `UnixServer` prend une référence à `TcpServer` dans son constructeur (ligne 1190) et l'appelle pour
  `accept_device`, `forget_device`, `pair_request` (lignes 1789-1810).
- `tetherd` **refuse de démarrer** si l'initialisation TLS échoue ou si le port TCP ne s'ouvre pas
  (`src/daemon/main.cpp:100-103`, `:132-135`).
- Le cœur Bluetooth appelle `net.hpp` pour diffuser ses événements : `connection.cpp:286`
  (`broadcast_local_event`), `obex_transfer.cpp`, et tout `src/daemon/main.cpp`.
- `build_local_state_snapshot()` (ligne 931), envoyé à chaque abonnement, ne décrit que la partie réseau.

Façon de faire conseillée : scinder d'abord `net.cpp` en deux fichiers sans changer le comportement
(`local_server.cpp` pour le socket et les commandes, `tcp_server.cpp` pour le réseau), faire passer les
tests, puis seulement retirer le second.

Conséquence visible à la reprise : même sans iPhone configuré, `tetherd` créait une paire de clés TLS dans
`~/.config/tether/`, écoutait sur le port TCP 5134 (toutes interfaces), se publiait en mDNS sous
`_tether._tcp` et enregistrait un service GATT sur l'adaptateur. **Coupé depuis la branche
`moteur-bluetooth-seul`** : la partie réseau ne démarre plus sans `network_enabled` ou `TETHER_NETWORK=1`
(`moteur-api.md`, section 11). Le code est toujours là ; la scission de `net.cpp` reste à faire.

### 4.2 L'appairage lance `tether-dialog`

`pairing.cpp:483-548` lance le programme `tether-dialog` (cherché à côté de `tetherd`, puis dans le `PATH`)
et attend sa réponse avant de se tourner vers les clients du socket (`pairing.cpp:649-656`). Si le
programme est absent, le repli vers l'événement `bt_pair_confirm_request` fonctionne déjà. Retirer
`src/dialog/` ne casse donc rien, mais il vaut mieux inverser l'ordre dans le code que compter sur l'absence
d'un fichier. `net.cpp:2697-2772` lance la même fenêtre pour l'appairage réseau : elle part avec `TcpServer`.

### 4.3 Le presse-papiers est appelé depuis le serveur local

**Fait (issue #19)** : le client Wayland (`wayland.cpp`, `wayland_protocols/`, `clipboard.hpp`) est retiré,
`session_inactive()` réduit à logind, le bouton « Copy Code » des bulles du démon supprimé. Ce qui suit
décrit l'état d'avant.

`net.cpp` utilise le global `g_wayland` (commandes `clipboard_*`, lignes 1715-1744 ; capacité `clipboard`,
ligne 415 ; instantané, ligne 986) et `src/daemon/main.cpp:188-204`, `:219-224` l'initialise. Le bouton
« copier le code » des bulles du démon passe aussi par lui.

**Attention : `session_lock.cpp` dépend du même code Wayland** pour savoir si la session est inactive
(`session_lock.cpp:123-129`). Sous GNOME ce chemin ne sert pas (le protocole d'inactivité est absent, le
code se rabat sur logind), mais la dépendance de compilation existe. Retirer `wayland.cpp` demande de
réduire `session_inactive()` à sa branche logind, et de retirer `wayland-client` des dépendances CMake
(`CMakeLists.txt`, ligne 32).

### 4.4 mDNS, TLS et secrets dans les dépendances de construction

**État après l'issue #19** : `wayland-client` n'est plus exigé. OpenSSL reste (libcrypto chiffre le journal
des messages dans `secret_store.cpp` ; libssl sert la partie réseau) et Avahi aussi (partie réseau) ; dans
l'archive portable, OpenSSL est lié statiquement et Avahi embarqué (`docs/release.md`).

`CMakeLists.txt` exige `avahi-client`, OpenSSL, `wayland-client` et `libsecret-1` pour **toute** la
bibliothèque (lignes 31-36). Avahi et OpenSSL partent avec le Wi-Fi ; `libsecret` reste (chiffrement du
journal des messages et du cache des contacts). `src/core/src/client.cpp` inclut OpenSSL jusque dans
l'en-tête public (`client.hpp:2`) : la ligne de commande en dépend tant que la moitié TLS du client n'est
pas retirée.

### 4.5 Le démon affiche ses propres bulles

`src/daemon/main.cpp` crée un `DesktopNotifier` (libnotify) et l'utilise à une dizaine d'endroits : fichier
reçu, message reçu, notification iPhone, échec de réponse, alerte mDNS. Les actions des bulles (répondre,
copier un code, couper une application) rappellent le moteur. Le réglage `set_desktop_popups` les coupe
déjà sans rien modifier. Le retrait est mécanique mais touche tout `main.cpp`.

### 4.6 AirPods : entrelacés avec les appels

Un tiers de `src/daemon/main.cpp` (lignes 343-675) gère le passage de relais des AirPods, et le
gestionnaire d'appels l'appelle à chaque changement (`main.cpp:771-772`, `run_handoff`). `audio.cpp` lance
`pactl`, absent de cette machine. La fonction est désactivée par défaut (`airpods_enabled = false`), donc
inerte ; la retirer demande de défaire ces appels croisés et de supprimer les commandes `bt_airpods*` de
`net.cpp`. Décision à prendre séparément : ce n'est pas urgent.

### 4.7 Le nom « tether » est partout

Le renommage, quand le nom sera choisi, touchera : l'espace de noms C++ `tether::`, le dossier d'en-têtes
`tether/`, les cibles CMake, les noms de binaires, les chemins (`~/.config/tether/`,
`~/.local/state/tether/`, `$XDG_RUNTIME_DIR/landline/landlined.sock`), l'attribut du trousseau
(`application = tether`, `secret_store.cpp:99`), le chemin D-Bus de l'annonce (`/org/tether/bt_advert`,
`advert.cpp:17`), le nom diffusé par l'annonce LE (`LocalName = "Tether"`, `advert.cpp:101` — visible sur
l'iPhone), l'unité `tether-btclass@`, le domaine gettext et le fichier de règle WirePlumber
(`zz-tether-phone-audio.conf`). Un changement de chemin fera perdre la configuration et la clé de
chiffrement d'une installation existante, sauf migration.

**Fait (branche du nom définitif).** Renommés : binaires, dossiers, socket, verrou, journal, entrée du
trousseau (avec reprise de l'ancienne), chemins D-Bus de l'agent et de l'annonce, nom de l'annonce LE,
unités, domaine gettext, règle WirePlumber, variables d'environnement, messages. Les noms sont réunis
dans `include/tether/identity.hpp`. **Gardés** parce que les changer toucherait tous les fichiers sans
rien apporter à l'utilisateur : l'espace de noms C++ `tether::`, le dossier d'en-têtes `tether/`, les
cibles CMake (`tether`, `tetherd`, `tether_cli`, `tether_test`), les macros de compilation `TETHER_*`.
**Gardés** parce qu'ils appartiennent au protocole de l'application iOS de Tether, que nous n'utilisons
pas (partie réseau, éteinte) : `_tether._tcp`, le nom du certificat, les UUID du service GATT, l'hôte
natif de navigateur, les noms d'icônes `tether-app-*`. La migration est décrite dans `systeme.md`,
section 5.

### 4.8 Divers

- La version est calculée par `git describe` dans `engine/` (`CMakeLists.txt`, lignes 8-22). Dans ce dépôt
  sans étiquette, elle vaut `0.3.5-unknown` ou un simple identifiant de commit.
- La configuration CMake télécharge nlohmann/json et GoogleTest depuis GitHub (`FetchContent`) : il faut le
  réseau à la première compilation.
- `tetherd` ne lit pas ses arguments (`src/daemon/main.cpp:88`) : `tetherd --help` ou `tetherd --version`
  **démarrent le démon**. Seul `tether --help` / `tether --version` est sans effet.
- `test_headless_runtime.cpp` (cible `tether_headless_test`) démarre un vrai `tetherd` : à ne lancer que
  sur une machine de test.

## 5. Les trois prérequis système

Le moteur ne modifie jamais le système : il **constate** et **imprime la commande**. Le calcul est fait par
`resolve_capability()` (`src/core/src/bluetooth/objects.cpp:585-729`), appelé à chaque rafraîchissement du
moniteur BlueZ (`src/core/src/bluetooth/monitor.cpp:359-361`, `:464`). Le résultat est le tableau
`capability.setup` de `bt_status`, chaque élément portant une phrase et une commande
(`objects.cpp:782-788`). `tether bt setup` se contente de l'afficher (`src/cli/main.cpp:396-440`, appelé en
`:1777`) ; l'interface GTK propose de copier les commandes.

| Prérequis | Comment le moteur le vérifie | Commande imprimée | Où |
|---|---|---|---|
| `bluetoothd --experimental` | présence du gestionnaire de surveillance d'annonces sur l'adaptateur, que BlueZ ne crée qu'en mode expérimental (`objects.cpp:467-469`) ; confirmé quand un appareil expose un `Bearer.LE1` rempli (`:612-627`) | fichier d'appoint systemd `bluetooth.service.d/experimental.conf` (`enable_experimental_command`, `:504-523`) | étape ajoutée en `objects.cpp:657-663` |
| Classe d'appareil `0x..0408` | `(classe & 0x1fff) == 0x0408` sur l'adaptateur choisi (`include/tether/bluetooth/objects.hpp:34-35`, `:59`) | `systemctl enable --now tether-btclass@hci0`, après écriture de l'unité si elle manque (`set_class_command`, `objects.cpp:526-558`) | étape ajoutée en `objects.cpp:716-720` |
| ERTM L2CAP actif | lecture de `/sys/module/bluetooth/parameters/disable_ertm` : `Y` ou `1` = désactivé (`l2cap_ertm_disabled`, `objects.cpp:733-737`) | `echo N \| sudo tee /sys/module/bluetooth/parameters/disable_ertm`, valable jusqu'au redémarrage | étape ajoutée en `objects.cpp:721-728` |

Un script indépendant fait les mêmes contrôles sans le démon : `scripts/bt-probe.sh` (lecture seule sauf
option `--set-class`).

Effet de chaque manque : sans le mode expérimental, pas de moitié LE dans le lien, donc pas de
notifications — et il doit être actif **avant** l'appairage. Sans la bonne classe, l'iPhone ne propose pas
les réglages « Afficher les notifications » et « Synchroniser les contacts » (un accord déjà donné survit).
Sans ERTM, obexd ne peut pas ouvrir MAP ni PBAP, et l'erreur ressemble à tort à un service introuvable
(`profile_supervisor.cpp:94-100`).

### État de cette machine au 16 novembre 2026 (lecture seule)

| Prérequis | État | Tient après redémarrage ? |
|---|---|---|
| Mode expérimental | actif ; `/etc/systemd/system/bluetooth.service.d/experimental.conf` en place | oui |
| Classe | `0x7c0408`, correcte | **non** : `bluetoothd` la réécrit à chaque démarrage |
| ERTM | actif (`disable_ertm = N`) | **non** : `/etc/sysfs.conf` ligne 24 remet `disable_ertm=1` à chaque démarrage (service `sysfsutils` activé), pour la manette Xbox |

### Façons persistantes de les appliquer

Rien de ce qui suit n'a été appliqué.

**Mode expérimental**

| Méthode | Remarque |
|---|---|
| Fichier d'appoint systemd qui redéfinit `ExecStart=… --experimental` | méthode de Tether, déjà en place ici. Le chemin de `bluetoothd` dépend de la distribution (`/usr/libexec/bluetooth/` sous Debian et Ubuntu, `/usr/lib/bluetooth/` sous Arch) : à résoudre à l'installation, jamais en dur |
| `Experimental = true` dans `[General]` de `/etc/bluetooth/main.conf` | même interrupteur, sans toucher à systemd, indépendant du chemin du binaire. Modifie un fichier de configuration de paquet (question posée aux mises à jour) |

**Classe d'appareil**

| Méthode | Remarque |
|---|---|
| Unité systemd liée à `bluetooth.service` qui lance `btmgmt class 4 8` et relit | méthode de Tether : `packaging/systemd/tether-btclass@.service`. `PartOf=bluetooth.service` la relance à chaque redémarrage de BlueZ ; elle réessaie 30 s et survit à un changement d'indice de l'adaptateur. Limite connue : un adaptateur qui se réinitialise sans redémarrage de `bluetoothd` n'est pas couvert |
| `Class = 0x000408` dans `[General]` de `main.conf` | **ne suffit pas sur cette machine.** Le greffon `hostname` de BlueZ réécrit la classe d'après le type de châssis donné par `hostnamed` (ici « convertible »). Tether l'a constaté et réserve cette méthode aux systèmes sans systemd (`docs/BLUETOOTH.md`, 10 septembre 2026) |
| `Class =` dans `main.conf` **et** `--noplugin=hostname` dans le fichier d'appoint | supprime la réécriture à la source, sans unité supplémentaire. Contrepartie : le nom Bluetooth de la machine ne suit plus le nom d'hôte (à fixer par `Name =` dans `main.conf`). Non essayé : à vérifier sur BlueZ 5.87 avant de s'y fier |
| Règle udev sur l'apparition de l'adaptateur, qui déclenche l'unité ci-dessus (`SYSTEMD_WANTS`) | complément suggéré par Tether pour couvrir la réinitialisation de l'adaptateur |

**ERTM**

| Méthode | Remarque |
|---|---|
| Retirer ou commenter la ligne 24 de `/etc/sysfs.conf` | la plus simple et la seule qui ne laisse pas deux réglages se contredire. Fait perdre le contournement de la manette Xbox |
| Règle modprobe `options bluetooth disable_ertm=N` dans `/etc/modprobe.d/` | **sans effet tant que la ligne de `sysfs.conf` existe** : `sysfsutils` écrit après le chargement du module et l'emporte. Utile seulement sur une machine où le réglage vient de la ligne de commande du noyau ou de modprobe |
| Unité systemd `oneshot` ordonnée après `sysfsutils.service`, qui écrit `N` | laisse `sysfs.conf` intact, mais annule quand même le contournement de la manette |
| Bascule à la demande : `N` quand l'iPhone est en service, `Y` sinon | seule façon de garder les deux, si la manette en a encore besoin. Demande un auxiliaire privilégié (voir ci-dessous) |

Le paramètre s'écrit à chaud, mais il ne vaut que pour les connexions ouvertes ensuite. Avant de choisir, il
faut savoir si la manette a encore besoin du contournement : il date des anciens microprogrammes Xbox et
des anciens noyaux, et le noyau de cette machine est récent (7.3). Essai à faire : ERTM actif, connecter
la manette, jouer quelques minutes. Si elle fonctionne, la première ligne du tableau suffit.

### Une seule demande de mot de passe

Les trois réglages demandent les droits administrateur, et aujourd'hui ce sont trois séries de commandes
`sudo` à copier. Deux façons de les ramener à une seule demande polkit :

1. **Un script d'installation unique lancé par `pkexec`.** L'extension détecte les manques (tableau
   `capability.setup`), affiche ce qui va être fait, puis lance une fois
   `pkexec /usr/libexec/<nom>/setup-system`. Le script, exécuté en administrateur : écrit le fichier
   d'appoint (ou `Experimental = true`), installe et active l'unité de classe, règle l'ERTM selon le choix
   retenu, recharge systemd et redémarre `bluetooth.service` **une seule fois**, à la fin. Une fiche
   d'action polkit (`/usr/share/polkit-1/actions/`) donne un message clair dans la fenêtre de mot de passe.
   Un seul mot de passe, rien de résident.
2. **Un petit service D-Bus système activé à la demande, protégé par polkit.** Nécessaire seulement si l'on
   retient la bascule ERTM à la demande, ou si l'on veut réappliquer la classe sans redémarrer BlueZ. Avec
   une autorisation mémorisée (`auth_admin_keep`) ou une règle polkit pour l'utilisateur actif, la demande
   n'a lieu qu'une fois.

La première suffit tant que les trois réglages sont permanents. Dans les deux cas le programme privilégié
doit être installé par un paquet ou par l'administrateur (une extension GNOME ne peut pas déposer de
fichier hors du dossier de l'utilisateur), rester minuscule, ne prendre aucun argument libre, et savoir
tout défaire (option de désinstallation). Deux précautions héritées de Tether : résoudre le chemin de
`bluetoothd` sur place, et activer le mode expérimental avant tout appairage.

Redémarrer `bluetooth.service` coupe brièvement tous les appareils Bluetooth de la machine (clavier,
souris, casques) : à annoncer dans l'assistant avant la demande de mot de passe.
