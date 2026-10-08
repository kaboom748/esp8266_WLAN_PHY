# Historique de la livraison

## Gel 0.5.1

- Livraison Arduino complete : sources existantes, exemples A/B, deux binaires,
  documentation d'installation/API, bilan de validation, journaux et SHA-256.
- Aucun changement du code, des exemples, des tests, de library.properties
  ou des binaires pendant cette finalisation.
- Aucun nouveau test, aucune compilation, aucun flash.
- Fenetres conservees a 1 seconde; aucune variante 250 ou 500 ms.
- Rapport de validation conservant les echecs et distinguant 0.5.0 de 0.5.1.

## 0.5.1, avant le gel

- Journalisation tamponnee par lignes completes lorsqu'elles tiennent dans
  le budget de silence, avec marge de temps et gestion des ecritures partielles.
- Tests logiciels de journalisation ajoutes.
- Une ligne serie tronquee subsiste dans le dernier essai materiel :
  ne pas lire cette evolution comme une resolution demontree de tous les
  problemes de diagnostics.
- Dernier banc arrete sur attente du profil 6; pas de PASS global de ce banc.

## 0.5.0

- Enchainement de paquets distincts dans les creneaux fixes d'une seconde.
- Suppression du remplissage de fin de fenetre.
- Conservation du chemin TX entre les trames d'une salve.
- ACK cumulatifs, reception contigue et reprise des donnees non acquittees.
- Files TX/RX de 32 messages, API write(), availableForWrite() et queuedBytes().
- Marqueur radio 0xd5, FIRST/LAST; incompatible sur l'air avec 0.4.x.
- Evaluation de qualite et changements de vitesse aux limites des salves.
- Essai materiel buffered-200-v050.json reussi; essai manuel suivant interrompu.

## Difference avec la livraison 0.4.1

L'archive 0.4.1 reste une version distincte et conservee. Elle utilisait
des cellules cadencees selon leur duree et une file de quatre messages.
La branche actuelle utilise des creneaux fixes de 1 seconde et une file
plus grande. Les mesures et validations de 0.4.1 ne sont pas transferees
a 0.5.1 par simple changement de numero.
