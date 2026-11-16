# Ce qui manque encore avant de publier

État au 16 novembre 2026, branche de travail du renommage, nom « Landline ». Rien n'a été installé, poussé ni envoyé.

## 1. Bloquants

1. **Le moteur n'est pas distribuable.** Il se compile depuis le dépôt (`scripts/build-engine.sh`) et
   s'installe par `scripts/install-local.sh`. Il n'y a ni release, ni paquet (.deb, .rpm, AUR), ni binaire.
   Tant que c'est le cas, l'extension affiche « Engine not running » chez quiconque l'installe depuis le
   site. Au minimum : une release GitHub 0.1 avec les sources et une page d'installation qui marche chez
   un inconnu ; mieux, un paquet qui pose aussi `landline-setup` et la fiche polkit chez root (plus
   besoin du premier `sudo … apply` à la main).
2. **Le dépôt est privé.** `url` et la description pointent vers `github.com/d101c/landline`, qui
   n'existe pas : le dépôt distant s'appelle encore `iphone-link` (le dossier local aussi). À créer ou
   renommer, puis rendre public — après un regard sur l'historique (une centaine de commits datés du
   même jour, messages en français).
3. **La relecture par l'auteur n'est pas faite** : `docs/RELECTURE.md`. Environ 24 heures pour
   l'extension, plus l'assistant système.
4. **Aucun essai réel sous le nouveau nom.** Tout ce qui a tourné : tests du dépôt, compilation et tests
   unitaires du moteur, Shell isolé contre le faux moteur. N'ont **pas** tourné : `landlined` (jamais
   lancé), la migration sur la vraie installation (`install-local.sh` n'a tourné que dans un HOME
   factice), la reprise de la clé du trousseau (code écrit, compilé, jamais exécuté contre un vrai
   trousseau), `landline-setup` en root, la vraie fenêtre polkit, l'extension dans la vraie session.

## 2. À faire avant l'envoi, sans être bloquant

- **GNOME 50 retiré de `shell-version`** (contre-vérification du 16 novembre). Rien n'a tourné sous 50,
  et les menus n'y ont pas la même interface : `open()`/`close()` y prennent une valeur de
  `BoxPointer.PopupAnimation`, sous 51 un objet `{animate, fadeOnly}` (seule forme employée par
  `lib/indicator.js`). `MessageTray` et `ModalDialog` sont identiques dans les sources de 50.0. Pour
  remettre « 50 » : choisir la forme d'appel selon la version dans `indicator.js`, puis dérouler
  `tools/shell-test/run.sh` dans un GNOME Shell 50 (machine virtuelle).
- **Instructions du README non vérifiées sur une machine propre.** La liste des paquets `apt` est déduite
  des dépendances CMake ; la compilation n'a été faite qu'avec la chaîne locale du banc d'essai (cmake et
  ninja dans un venv, en-têtes dans un sysroot). À dérouler dans une machine virtuelle Debian ou Ubuntu,
  et à compléter pour Fedora et Arch.
- **Les scripts et l'assistant système parlent français** : messages de `install-local.sh`,
  `uninstall-local.sh`, `build-engine.sh`, messages et commentaires de `system/landline-setup` (883
  lignes). Le README le dit. Pour un dépôt public, à passer en anglais (les essais de
  `system/tests/run.sh` cherchent certains de ces messages : ils sont à adapter en même temps). Seuls les
  en-têtes des fichiers écrits sur le système et la fiche polkit sont déjà en anglais.
- **Docs internes en français** (`docs/*.md`) : acceptable, mais le README y renvoie.
- **Traductions** : aucune (domaine gettext `landline` prêt, pas de `.po`). Pas exigé. Les catalogues du
  moteur (`engine/po`) ont suivi le renommage par remplacement de texte ; ils n'ont pas été régénérés
  (`po/update-pot.sh --check` non lancé), le modèle s'appelle encore `tether.pot`, et une chaîne (le titre de
  l'aide de `landlinectl`) y est sans traduction dans chaque langue : `po/check-catalogs.sh` le signale.
- **Captures de la fiche** : quatre images dans `docs/ego/captures/`, issues du Shell isolé (données
  inventées, 1280×720). À refaire après toute retouche visuelle : `tools/shell-test/run.sh`.
- **Icône de la fiche** : aucune.
- **`shellcheck`** n'est pas installé sur cette machine : l'analyse statique des scripts n'a pas été faite.
- **Le nom ne dit pas « iPhone ».** « Landline » seul n'évoque ni le téléphone ni Apple : la première
  phrase de la description (« Landline — your iPhone's calls, messages and notifications on the GNOME
  desktop ») et les mots de la fiche portent toute la découvrabilité. À vérifier dans la recherche du
  site une fois publié (« iphone », « sms », « imessage », « calls », « bluetooth »). En contrepartie, plus
  aucune marque dans le nom ; la ligne de non-affiliation reste dans le README et la description.
- **Une extension homonyme ?** Non vérifié : chercher « Landline » sur extensions.gnome.org avant l'envoi.
- **Cinq fiches de l'auteur attendent déjà une revue** (audit `08-audit-ego.md`, section 7) : l'avis du
  relecteur sur `pkexec` (Scroll Speed) et sur un composant externe (Voice Hold) dirait ce qu'il accepte
  ici.

## 3. Ce qui n'a pas été renommé, et pourquoi

- Dans le moteur : l'espace de noms C++ `tether::`, le dossier d'en-têtes `include/tether/`, les cibles
  CMake (`tether`, `tetherd`, `tether_cli`, `tether_test`, `tether_headless_test`), les macros de
  compilation `TETHER_*`. Les changer toucherait presque tous les fichiers du moteur sans rien changer
  pour l'utilisateur, et rendrait toute comparaison avec Tether illisible.
- Ce qui appartient au protocole de l'application iOS de Tether (partie réseau, éteinte par défaut) :
  type mDNS `_tether._tcp`, nom du certificat, UUID du service GATT, hôte natif de navigateur
  (`com.tether.extension`, `tether-native-host`), noms d'icônes `tether-app-*`.
- Les fichiers de Tether que nous n'utilisons pas (`apple/`, `extension/`, `src/gtk`, `src/tui`,
  `src/dialog`, `docs/`, `packaging/`, `README.md`) : laissés tels quels, plus compilés ni installés.
- Les noms de fichiers `packaging/systemd/tetherd.service.in`, `scripts/tether-reset.sh`, `po/tether.pot`
  (leur contenu, lui, est à jour).
- `engine/LICENSE`, `NOTICE.md` : intacts.
- Le dossier local `~/DEVELOPEMENT/iphone-link`, le dépôt distant et le banc `iphone-link-study`.

## 4. Changer de nom : `scripts/renommer.sh`

Le nom a changé trois fois en une journée. Tout est fait pour qu'un changement de plus coûte une commande :

```sh
scripts/renommer.sh "<Nom affiché>" <identifiant-long> <préfixe-court> [--sans-tests|--sans-moteur|--force]
```

- **Nom affiché** : ce que l'utilisateur lit (« Landline »).
- **Identifiant long** : uuid (`<id>@d101c.github.io`), schéma GSettings, chemin dconf, domaine gettext,
  nom du dépôt, préfixe du journal de l'extension.
- **Préfixe court** : service, binaires (`<p>d`, `<p>ctl`), dossiers, socket, assistant, fiche polkit,
  unités système, variables (`<P>_…`), classes de style, icônes, nom affiché du moteur (`<P>…`).

Le script lit les noms courants dans `scripts/noms.sh`, remplace, renomme les fichiers, puis lance les
tests (modèle, client, assistant système, compilation et tests du moteur, scripts d'installation). Il ne
valide rien : relire `git diff --cached --stat`, refaire les captures, valider.

**Il a servi pour de bon** : le passage du nom provisoire précédent (nom affiché en deux mots,
identifiant long et préfixe court différents) à « Landline » / `landline` / `landline` a été fait par lui (101 fichiers modifiés, 8 renommés, tous les
tests verts derrière, moteur recompilé compris), après deux essais dans une copie du dépôt. Il a tout
couvert sauf ce qui n'est pas du texte suivi par git : les captures (refaites), les binaires déjà compilés
(recompilés par le script), et les tournures françaises autour du nom (« d'Landline », deux endroits,
corrigés à la main).

Limites connues, à garder en tête :

- **Le préfixe court ne doit figurer nulle part dans le dépôt**, même au milieu d'un mot (un essai avec
  `truc` a touché `constructor`). Le script le vérifie et refuse.
- **Identifiant et préfixe identiques** (le cas actuel) : au renommage suivant, l'identifiant n'est
  reconnu que par son contexte (uuid, schéma, chemin dconf, `github.com/d101c/…`, domaine gettext,
  `NAME_ID`, `LOG_PREFIX`). Une mention de l'identifiant ailleurs, dans un texte, prendrait le préfixe.
  Le script le rappelle. Essayé dans la copie : `zorglub`/`zorglub` vers `bidule-x`/`bidule`.
- Il ne migre pas une installation faite sous le nom courant : `install-local.sh` ne connaît que les
  noms d'avant (`iphone-link`, `tether`). **Après une installation réelle sous « Landline », un nouveau
  changement de nom demanderait d'ajouter ce nom aux anciens** dans `scripts/noms.sh`,
  `install-local.sh` et `system/landline-setup`. Et après publication, l'uuid ne change plus du tout.
- Dans `engine/`, il ne touche que ce que nous avons renommé (liste ci-dessous).

### Tous les endroits qui portent le nom

Sources uniques (tout le reste en dérive ou est réécrit par le script) :

| Où | Quoi |
|---|---|
| `scripts/noms.sh` | `NAME_DISPLAY`, `NAME_ID`, `NAME_PREFIX` ; lu par tous les scripts et par `system/tests/install.sh` |
| `extension/lib/names.js` | préfixe du journal, unité du moteur, socket, chemins de l'assistant |
| `system/landline-setup` | `NAME=` en tête : unités, fichier d'appoint, dossier libexec, fiche polkit, en-tête des fichiers |
| `engine/src/core/include/tether/identity.hpp` | nom affiché, dossiers, démon, outil, socket, verrou, journal, unités, entrée du trousseau |

Le reste, écrit en toutes lettres et réécrit par le script :

- **Extension** : `metadata.json` (uuid, nom, description, url, domaine gettext, schéma) ; le nom et le
  contenu de `schemas/org.gnome.shell.extensions.<id>.gschema.xml` (identifiant, chemin, domaine) ; les
  trois icônes `icons/<préfixe>-*.svg` et leurs appels ; les classes de style `<préfixe>-…` dans les deux
  feuilles de style, `cards.js`, `indicator.js`, `setup.js`, `extension.js` ; le titre de la notification
  de `extension.js` (`_forget()`).
- **Assistant système** : le nom du fichier `system/<préfixe>-setup` ; `system/io.github.d101c.<préfixe>.policy`
  (nom, identifiant d'action, chemin annoté, éditeur, adresse) ; `system/<préfixe>-engine.service` (nom,
  chemins, variable `<P>_LOCALEDIR`, motif de `pkill`, adresse du dépôt) ; `system/tests/run.sh`.
- **Moteur** : chaînes de `src/core`, `src/daemon`, `src/cli` (messages, chemins D-Bus
  `/io/github/d101c/<préfixe>/…`, schéma du trousseau, règle WirePlumber `zz-<préfixe>-phone-audio.conf`,
  fichiers temporaires, variables `<P>_…`) ; `test/` ; `po/*.po` et `po/tether.pot` ; `CMakeLists.txt`
  (option `<P>_UPSTREAM_FRONTENDS`, chemin du démon), `src/CMakeLists.txt`, `src/cli/CMakeLists.txt` et
  `src/daemon/CMakeLists.txt` (noms des binaires), `src/core/CMakeLists.txt` (domaine gettext, dossier de
  données), `po/CMakeLists.txt` (nom du catalogue compilé) ; `packaging/systemd/tetherd.service.in` ;
  `scripts/tether-reset.sh`.
- **Scripts** : commentaires et messages de `install-local.sh`, `uninstall-local.sh`, `build-engine.sh`,
  `pack-extension.sh`, `reset-essai.sh`, `chrono-moteur.py` (chemin du socket).
- **Essais** : `tools/faux-moteur.py`, `tools/faux-<préfixe>-setup`, `tools/shell-test/run.sh`,
  `inner.sh`, `pilote@test/extension.js` (uuid, classes de style), `tools/shell-test/bin/*`,
  `tests/client.test.js`, `tests/model.test.js`.
- **Textes** : `README.md`, `docs/ego/description.txt`, `docs/ego/note-de-revue.txt`,
  `docs/RELECTURE.md`, ce fichier, `docs/*.md`.

Ce que le script ne peut pas faire : les images (`docs/captures/`, `docs/ego/captures/` : relancer
`tools/shell-test/run.sh`), le zip de `dist/` (relancer `scripts/pack-extension.sh`), le nom du dossier
et du dépôt distant, une installation existante.

## 5. Mentions restantes des anciens noms

`git grep -i "iphone-link\|iphone link"` et `git grep -i tether` hors `engine/` ne renvoient plus que :

- **Migration** : `scripts/noms.sh` (noms d'avant), `scripts/install-local.sh` et
  `scripts/uninstall-local.sh` (commentaires), `system/landline-setup` (`OLD_NAME`, commentaires),
  `system/tests/run.sh` et `system/tests/install.sh` (cas de migration), `docs/systeme.md` (sections 2 et 5).
- **Attribution à Tether** : `NOTICE.md`, `README.md`, `docs/ego/description.txt`,
  `docs/ego/note-de-revue.txt`, `docs/RELECTURE.md`, ce fichier, `docs/moteur-*.md`,
  `docs/essai-reconnexion.md` (ce que Tether a mesuré ; `docs/moteur-inventaire.md` est l'inventaire
  d'origine, gardé sous ses noms d'alors avec une note en tête).
- **Chemins réels du code** : `include/tether/…`, `tether_test`, `tether-reset.sh`, `TETHER_CAMERA`,
  `_tether._tcp`, `tether-dialog` (programme de Tether, plus compilé) dans les docs et dans
  `scripts/build-engine.sh`, `scripts/reset-essai.sh`.
- **Chemins réels de la machine** : `~/DEVELOPEMENT/iphone-link` et `iphone-link-study` dans
  `scripts/build-engine.sh` et les docs.
- **Sans rapport** : la tuile « Tether » de GNOME et le mot « tethering » (partage de connexion) dans
  `extension/lib/hotspot.js`, `extension/lib/cards.js` et `docs/extension.md` ; « iPhone linked »,
  « links again » dans les textes d'interface.
