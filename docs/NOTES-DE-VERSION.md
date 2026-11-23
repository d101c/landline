# Notes de version

Ce qui change pour l'utilisateur, lot par lot. Le détail technique est dans les messages de commit et dans
`docs/moteur-reconnexion.md`.

## 0.2 — « Sobre et fiable » (17–18 novembre 2026)

Installée sur la machine de développement ; pas envoyée sur EGO (la 0.1 y est en revue).

### Reconnexion
- **Mode sobre** (A-01) : au repos, seul le lien basse consommation reste ouvert ; le lien classique s'ouvre
  quand il sert (relève des messages, appel, envoi) puis se referme. Réglage « Light / Always connected ».
- **Lien plus tolérant** (D-02) : l'ordinateur demande à l'iPhone une tolérance de 6 secondes aux pertes
  radio, comme l'exige Apple pour les accessoires. Un petit auxiliaire système, posé une fois avec le mot de
  passe (« Keep the Link Steady… »), le permet.
- **Retour plus rapide après une sortie** (D-04) : après des liens ratés en limite de portée, l'ordinateur ne
  se tait plus que 15 s à 2 min, au lieu de 2 à 10 min.
- **Réveil** (D-06, D-07) : après une longue veille, les messages sont relus dès le retour du lien ; la
  tolérance est réappliquée en quelques secondes.

### Messages et contacts
- **Service des messages relancé tout seul** (D-03) : si le service Bluetooth des messages et contacts du
  système plante ou se fige (cas fréquent à l'ouverture de session), il est relancé et la relève reprend,
  sans bloquer le reste.

### Interface
- **Icône animée** (A-02) : deux styles, Phone (défaut) et Handset, à choisir avec aperçu dans Phone
  Settings ; plus de point à côté de l'icône.
- **Notifications en un seul système** (A-03) : tout passe par la pastille, plus de bulles GNOME en double ;
  section « On your iPhone » ; résumé au retour (« While you were away… ») ; historique « Earlier » en
  option.
- **« Call… » et « New Message… »** en haut du menu (A-04).
- **« Link again »** n'apparaît plus quand l'iPhone est simplement hors de portée, ni sur un lien rétabli
  (D-05).

### À savoir
- Prouvé en réel : réveil, sortie et retour sans geste, relance du service des messages, relève au réveil.
- Pas encore prouvé : délai de retour après la correction D-04, son des appels en mode sobre, consommation
  sur une nuit.
