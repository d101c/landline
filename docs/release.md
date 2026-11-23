# Archive portable du moteur

Une seule compilation du moteur, qui tourne sur tout bureau GNOME récent (Ubuntu, Debian, Fedora, Arch),
x86_64. C'est elle que l'installateur de l'extension télécharge (issue #10) ; elle remplace la compilation
locale (`scripts/build-engine.sh` puis `scripts/install-local.sh`), qui reste le chemin du développement.
Suivi : issue #19.

## Construire

```sh
scripts/build-release.sh 0.2.0            # compile, lance les tests, empaquette
scripts/build-release.sh 0.2.0 --no-test  # sans la suite de tests
scripts/check-release.sh dist/landline-engine-0.2.0-x86_64.tar.zst
```

Il faut Docker (le démon système, ou Docker sans droits root : `DOCKER_HOST=unix:///run/user/$UID/docker.sock`).
Rien n'est compilé ni installé sur la machine hôte. La source est prise par `git archive HEAD` : seul ce qui
est validé part dans l'archive ; le script refuse un `engine/` ou un `system/` modifié, sauf `ALLOW_DIRTY=1`
(archive d'essai, à ne pas publier). Compter 7 à 8 minutes, tests compris (la première fois, plus
l'installation des outils dans l'image `landline-engine-builder`, gardée ensuite).

Le conteneur est lancé avec `--security-opt seccomp=unconfined` : un Docker plus ancien que l'image (20.10
ici) refuse des appels système récents, et `apt-get` échoue alors sur Ubuntu 24.04. Il ne sert qu'à compiler
une source connue.

Sortie, dans `dist/` (ignoré par git) :

| Fichier | Contenu |
|---|---|
| `landline-engine-<version>-x86_64.tar.zst` | l'archive |
| `landline-engine-<version>-x86_64.tar.zst.sha256` | sa somme, au format de `sha256sum` |
| `SHA256SUMS` | l'archive, plus `landline-engine-<version>/system/landline-setup` et `landline-engine-<version>/engine/landline-le-params`, chemins relatifs au dossier où l'archive est extraite : l'installateur de l'extension épingle ces deux sommes, parce que ce sont les deux fichiers qui reçoivent des droits (root, `cap_net_raw`) |

`sha256sum -c SHA256SUMS`, lancé dans le dossier qui contient l'archive et son contenu extrait, vérifie les
trois.

## Ce que contient l'archive

```
landline-engine-<version>/
  engine/  landlined landlinectl landline-le-params lib/ locale/ LICENSE VERSION
  system/  landline-setup io.github.d101c.landline.policy landline-engine.service obex-landline.conf
```

`engine/` a la disposition du moteur installé (`~/.local/lib/landline/engine/`, voir `systeme.md` §5) ;
`VERSION` contient le numéro seul (`0.2.0`), que `landlinectl --version` affiche aussi. `system/` est la
copie, telle quelle, des fichiers du dépôt. `lib/` contient les bibliothèques embarquées (ci-dessous).
`locale/` : les catalogues du moteur (`LANDLINE_LOCALEDIR`, posé par l'unité).

Pour 0.2.0 : 6,4 Mo compressée (zstd -19), 21 Mo extraite ; `landlined` et `landlinectl` font chacun
10 Mo, OpenSSL et le runtime C++ compris.

## Bibliothèques : ce qui est pris au système, embarqué ou lié

Option CMake `LANDLINE_PORTABLE=ON` (éteinte par défaut ; `build-release.sh` l'allume).

| Bibliothèque | Sort | Pourquoi |
|---|---|---|
| glibc (`libc`, `libm`) | système | voir « glibc minimale » |
| GLib, GObject, GIO | système | partout où GNOME tourne |
| `libsecret-1` | système | clé du trousseau qui chiffre les messages et contacts gardés |
| `libnotify` | système | bulles du démon |
| `libdbus-1`, `libsystemd` | système (indirectes) | tirées par Avahi et libsecret |
| `libstdc++`, `libgcc_s` | **liées statiquement** (`-static-libstdc++ -static-libgcc`) | la compilation (GCC 13) ne doit pas dépendre du `libstdc++` de la machine |
| OpenSSL (`libcrypto`, `libssl`) | **liées statiquement** | `libcrypto` sert au moteur Bluetooth (AES-256-GCM du journal des messages, `secret_store.cpp`) ; `libssl` à la partie réseau facultative. Les distributions ne s'accordent pas sur la version : Ubuntu 24.04 a OpenSSL 3 (`libssl.so.3`), Fedora 45 et Ubuntu 26.10 OpenSSL 4 (`libssl.so.4`) |
| Avahi (`libavahi-client`, `libavahi-common`) | **embarquées** dans `engine/lib/` | mDNS de la partie réseau facultative (éteinte par défaut). Absentes de certains bureaux ; liées dynamiquement (pas de version statique chez Debian/Ubuntu). Elles ne dépendent que de la glibc et de `libdbus-1` |
| `wayland-client` | **retirée** | le presse-papiers et l'inactivité passaient par des protocoles que Mutter n'offre pas (`ext/wlr-data-control`, `ext-idle-notify`) : code mort sous GNOME, retiré du moteur |
| `landline-le-params` | glibc seule | lié statiquement au runtime C++ depuis toujours, sans RPATH : un programme qui reçoit `cap_net_raw` ne charge rien d'un dossier qu'un autre peut écrire (la glibc ignore d'ailleurs `$ORIGIN` pour un tel programme) |

Les binaires portent un `RPATH` (et non `RUNPATH` : `-Wl,--disable-new-dtags`) `$ORIGIN/lib`, consulté avant
les dossiers du système et valable aussi pour les dépendances des bibliothèques embarquées ; celles-ci
portent `RUNPATH $ORIGIN`. Plus besoin de `LD_LIBRARY_PATH` : l'unité le pose encore, sans effet sur cette
archive (le `RPATH` passe avant).

Le script ne regarde que les dépendances **directes** (`NEEDED`) des binaires : celles des bibliothèques du
système (`libpcre2`, `libffi`, `libgcrypt`, `gdk-pixbuf`…) sont l'affaire de la distribution. Toute
dépendance directe hors de la liste « système » ci-dessus est copiée dans `lib/` ; une dépendance à
`libstdc++`, `libgcc_s`, `libssl` ou `libcrypto` arrête la construction (ce serait une option perdue).

Hors bibliothèques, le moteur a besoin à l'exécution de BlueZ, d'obexd et de `btmgmt` dans le `PATH`
(`systeme.md` §5) : paquets de la distribution, que l'archive ne contient pas (chez Fedora 45, `btmgmt` est
dans le paquet `bluez`).

## glibc minimale : 2.38

Base de construction : `ubuntu:24.04` (glibc 2.39, GCC 13). Les binaires exigent `GLIBC_2.38` au plus
(relevé par `objdump -T` à chaque construction, affiché par le script) ; les bibliothèques Avahi
embarquées aussi. Donc : Ubuntu 24.04 et suivantes, Debian 13, Fedora 39 et suivantes, Arch.

Pourquoi pas plus bas : le moteur emploie `std::format` (C++20), que GCC n'a qu'à partir de la version 13.
Debian 12 (glibc 2.36) n'a que GCC 12, Ubuntu 22.04 GCC 11/12. Descendre demanderait un compilateur
installé à part dans l'image, sans profit aujourd'hui : l'extension ne vise que GNOME Shell 51
(`metadata.json`), que seules des distributions bien plus récentes portent (Fedora 45, Ubuntu 26.10, Arch).
La marge est donc large ; elle servira si l'extension s'ouvre à des Shell plus anciens.

## Reproductibilité

- horodatages fixés à la date du dernier commit qui touche `engine/` ou `system/` (`SOURCE_DATE_EPOCH`) :
  un commit de documentation ne change pas l'archive ;
- `tar --sort=name`, propriétaire `0:0`, permissions normalisées, `zstd -19 -T1` ;
- chemins de compilation constants (`/src`, `/build`, et `-ffile-prefix-map`), binaires dépouillés
  (`strip --strip-unneeded`) ;
- variables qui restent : le contenu de l'image de base si elle n'est pas épinglée (le script affiche son
  empreinte ; `BASE_IMAGE=ubuntu@sha256:…` la fixe), et les paquets installés dans l'image d'outils (le
  compilateur suit les mises à jour d'Ubuntu 24.04 tant que l'image `landline-engine-builder` n'est pas
  refaite).

Vérifié pour 0.2.0 : deux constructions successives donnent la même archive, octet pour octet
(première archive `48ab4e3e…`, jamais publiée, base
`ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8`). Reconstruite le
18 novembre 2026 après le correctif de `landline-setup` (issue #25), même base et même image d'outils :
`931d192f564342813dfbe78a38be50076bc127fd6dcf27d9a270e0fbfa3c9f34` (6 621 417 octets) ; comparée à la
première, seul `system/landline-setup` diffère, les binaires sont identiques octet pour octet.

## Distributions vérifiées

`scripts/check-release.sh` lance chaque image avec les seules bibliothèques d'un bureau GNOME (GLib,
libsecret, libnotify, systemd, D-Bus ; ni Avahi, ni OpenSSL, ni BlueZ), extrait l'archive, puis :
`ldd` des trois binaires (rien ne manque), `landlinectl --help` et `--version`, et `landlined` lancé
isolé (HOME et `XDG_RUNTIME_DIR` jetables, sans BlueZ) : il doit tenir 4 s et écrire sa version dans son
journal.

| Distribution | glibc | Résultat (0.2.0, 18 novembre 2026) |
|---|---|---|
| Ubuntu 24.04 (conteneur) | 2.39 | ldd complet, `--help`, démon en marche |
| Ubuntu 26.04 (conteneur) | 2.43 | idem |
| Debian 13 (conteneur) | 2.41 | idem |
| Fedora 45 (conteneur `fedora:latest`) | 2.44 | idem |
| Arch (conteneur `archlinux:latest`, 8 novembre 2026) | 2.44 | idem |
| Fedora 45 Cloud (VM, copie de `/data/vm-test/fedora`) | 2.44 | `sha256sum -c SHA256SUMS`, ldd complet, `--help` (aussi en français), démon lancé à la main (HOME jetable) : en marche, `landlinectl status` lui répond |

Pas vérifié : le moteur avec un vrai iPhone sur une autre machine que celle du développement, et un
bureau GNOME complet hors de cette machine (la VM Fedora est une image Cloud, sans GNOME Shell).

## Publier

L'étiquette prévue pour la première archive est `engine-v0.2.0`. Rien n'est publié par les scripts.

```sh
scripts/build-release.sh 0.2.0
scripts/check-release.sh dist/landline-engine-0.2.0-x86_64.tar.zst
git tag -a engine-v0.2.0 -m "Moteur 0.2.0, archive portable"
git push origin engine-v0.2.0
gh release create engine-v0.2.0 -R d101c/landline --title "Engine 0.2.0" --notes-file <notes> \
    dist/landline-engine-0.2.0-x86_64.tar.zst \
    dist/landline-engine-0.2.0-x86_64.tar.zst.sha256 \
    dist/SHA256SUMS
```

L'étiquette doit pointer le commit dont l'archive a été construite (`build-release.sh` l'affiche).
