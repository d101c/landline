# Crédits et licences

## Moteur (`engine/`)

Le dossier `engine/` reprend le code source de **Tether**, écrit par **Zack Bartel**.

- Projet d'origine : <https://github.com/zackb/tether>
- Licence : MIT — texte conservé tel quel dans [`engine/LICENSE`](engine/LICENSE)
  (« Copyright (c) 2026 Zack Bartel »)
- Commit d'origine : `9ca3c46f7f3a8f1c63cadb9bd7016716a305bd69` (15 novembre 2026),
  version `0.3.5-9-g9ca3c46`
- Copie faite le 16 novembre 2026, sans l'historique git d'origine et sans les dossiers de compilation.
  Aucun fichier n'a été modifié ni renommé lors de la copie.

Les modifications apportées au moteur après cette copie sont suivies dans l'historique git de ce dépôt.
La licence MIT impose de conserver la mention de droit d'auteur et le texte de la licence dans toute copie
ou partie substantielle du logiciel : `engine/LICENSE` doit donc rester en place, y compris dans les paquets
distribués.

Le journal de bord Bluetooth de Tether (`engine/docs/BLUETOOTH.md`) cite lui-même ses sources dans sa
section « Credits » : BlueFerry (Erik Bourget et contributeurs), la spécification ANCS d'Apple, les
spécifications MAP et PBAP du Bluetooth SIG et l'API D-Bus de BlueZ. L'annonce de sollicitation ANCS suit
la convention établie par ancs4linux (`engine/src/core/src/bluetooth/advert.cpp`).

## Dépendances récupérées à la compilation

Le moteur télécharge à la configuration CMake, sans les copier dans ce dépôt :

- nlohmann/json 3.12.0 (MIT)
- GoogleTest 1.17.0 (BSD 3 clauses), pour les tests seulement
