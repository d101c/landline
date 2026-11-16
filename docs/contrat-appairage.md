# Contrat d'appairage : ce que l'extension doit lire et envoyer

Branche `moteur-appairage-sain`. Une page, à jour avec le code du moteur. Le détail est dans
`moteur-api.md` (sections 3 et 6) ; le récit de l'incident dans `moteur-reconnexion.md` (section 8).

## 1. Santé de l'appairage : trois champs, partout les mêmes

Présents dans **`bt_connection_changed`** (événement et réponse à `bt_connection`) et dans **`bt_status`**.

| Champ | Type | Valeurs |
|---|---|---|
| `bond_state` | chaîne | `"none"`, `"ok"`, `"partial"`, `"broken"` |
| `bond_detail` | chaîne | `""`, `"bredr_only"`, `"le_only"`, `"auth_failures"`, `"key_missing"` |
| `needs_repair` | booléen | `true` : il faut retirer l'appairage des deux côtés et recommencer |

`bt_status` ajoute **`bond_address`** (chaîne) : l'adresse de l'appareil dont parlent ces trois champs.
C'est l'iPhone sélectionné ; s'il n'y en a pas, c'est le premier iPhone que BlueZ connaît déjà comme
appairé (cas d'un appairage créé hors du moteur). Chaîne vide si aucun.

| `bond_state` | Sens | `bond_detail` | `needs_repair` |
|---|---|---|---|
| `none` | pas d'appairage côté ordinateur | `""` | `false` |
| `ok` | appairage complet, ou impossible à juger (BlueZ sans `Bearer.LE1`) | `""` | `false` |
| `partial` | il manque une moitié : constat lu dans BlueZ, sans attendre un échec | `bredr_only` (pas de clés LE : pas de notifications) | `true`, sauf si l'ordinateur ne peut de toute façon pas porter le LE |
| `partial` | idem, cas inverse ; signalé seulement | `le_only` | `false` |
| `broken` | l'iPhone refuse l'authentification à répétition : constat observé en marche | `auth_failures` (5 coupures `org.bluez.Reason.Authentication` en 120 s) ou `key_missing` (2 refus « key-missing ») | `true` |

Règles pour l'extension :

- **Ne jamais afficher « connecté » quand `needs_repair` est vrai**, quels que soient `classic_connected`,
  `le_connected`, `map_open`. Afficher « appairage à refaire » et proposer le bouton de la section 3.
- Se fier à `needs_repair`, pas à `bond_state` : un `partial` peut être sans remède (`needs_repair:false`).
- `broken` l'emporte sur `partial`. `broken` ne retombe que si le lien LE tient 60 s d'affilée, ou après
  un nouvel appairage ou un `bt_unpair`.
- Dans l'état `broken`, le moteur cesse de solliciter l'iPhone : une seule fenêtre d'essai de 30 s toutes
  les 10 minutes. Ne pas s'attendre à des changements d'état fréquents.
- `link_reason` porte la phrase lisible correspondante (en anglais, comme le reste).

## 2. `bt_pair` sur un appareil déjà appairé

Requête : `{"command":"bt_pair","address":"…","operation_id":"…","fresh":false}` — **`fresh`** est
nouveau, facultatif, faux par défaut.

| Cas | Étapes `bt_pair_progress` | `bt_pair_result` |
|---|---|---|
| appairage sain, `fresh` absent | `already_paired`, `settling`, `soliciting` | `success:true`, `status:"already_paired"` (inchangé) |
| appairage `partial` ou `broken`, `fresh` absent | **`stale_bond`** (`detail` = `bond_detail`) et rien d'autre | **`success:false`, `status:"stale_bond"`, `reason:"stale_bond"`** ; rien n'est modifié |
| `fresh:true`, appareil appairé | **`removing_bond`**, `discovering`, puis le déroulé normal (`connecting`, `confirm`, …) | celui d'un appairage neuf (`paired`, `rejected`, `timeout`, …) |
| `fresh:true`, appareil non appairé | déroulé normal | idem |

Champs ajoutés à `bt_pair_result` (toujours présents) :

| Champ | Sens |
|---|---|
| `reason` | motif stable de l'échec, **chaîne vide en cas de succès**. Valeurs : `stale_bond`, `not_found`, `rejected`, `timeout`, `busy`, `error`. Toujours égal à `status` quand `success` est faux |
| `bond_state`, `bond_detail`, `needs_repair` | santé de l'appairage à la fin de l'opération, mêmes valeurs qu'en section 1 |

Avec `fresh:true`, l'appareil disparaît de BlueZ puis doit être redécouvert : **l'iPhone doit être
déverrouillé, Réglages > Bluetooth ouvert**. Et l'ordinateur doit avoir été oublié sur l'iPhone
(« Oublier cet appareil »), sinon l'iPhone refuse et le résultat est `timeout` ou `rejected`.
Si la redécouverte échoue : `status:"not_found"` — l'ancien appairage est alors déjà retiré.
`bt_devices` porte aussi `bond_state` et `bond_detail` par appareil (lecture des clés seule, jamais `broken`).

## 3. Parcours conseillé quand `needs_repair` est vrai

1. Dire à l'utilisateur d'oublier l'ordinateur sur l'iPhone (Réglages > Bluetooth > (i) > Oublier).
2. Lui faire laisser l'écran Bluetooth de l'iPhone ouvert.
3. Envoyer `bt_pair` avec `"fresh": true` (abonné avant, pour recevoir `bt_pair_confirm_request`).
4. Suivre le résultat ; relire `bond_state` dans le `bt_status` diffusé juste après.

## 4. `bt_unpair` : retrait complet côté ordinateur

Requête : `{"command":"bt_unpair","address":"…","operation_id":"…"}` → `OK`, puis `bt_unpair_result` :
`{success, status, reason, message, address, removed, selection_cleared, operation_id?}`.

- `removed` : l'appareil a été retiré de BlueZ (clés comprises).
- `selection_cleared` : l'adresse était l'iPhone sélectionné ; la sélection est effacée, la supervision
  arrêtée (sessions MAP/PBAP fermées, client ANCS et appels libérés, annonce LE retirée).
- `status` : `unpaired` (succès, même si BlueZ ne connaissait plus l'appareil mais qu'il était encore
  sélectionné), `not_found` (ni dans BlueZ ni sélectionné), `error`.
- Suivi d'un `bt_status` et d'un `bt_connection_changed` (`device_present:false`, `bond_state:"none"`).

L'iPhone garde sa propre fiche : l'utilisateur doit toujours l'oublier à la main.
