# Changelog

## Finalisation Documentaire De 0.6.0-beta.2

- Livraison figee sur le modele de la 0.5.1, sans nouvelle version technique.
- Sources, exemples, binaires, tests, construction et reglages inchanges.
- Guide d'installation, reference API, commandes, debit et limites documentes.
- Resultats precedents distingues des controles locaux de fichiers/payloads.
- Provenance originale conservee ; nouveau manifeste SHA-256 du paquet final.
- Aucun nouveau flash, aucune nouvelle compilation, aucun nouvel essai RF.

## 0.6.0-beta.2

- Portage de l'adaptation 16-FSK 0.8.0-beta.4, sans portage de sa modulation.
- Reprise automatique au profil inferieur, avec trois nouvelles fenetres
  manquees requises avant une autre descente; calibration recente conservee.
- Temporisation par profil defaillant de 30/60/120/240 s, stockee sur 32 bits.
- Tests de 54 replis adjacents, 18 sequences de reessais repetes, panne totale,
  maintien des buffers, perte de confirmation, redemarrage et calibration.
- Modulation 4-FSK, tonalites, IQ, fenetres d'une seconde et API inchanges.

## 0.6.0-beta.1

- Branche experimentale 4-FSK separee de la livraison 0.5.1.
- Deux bits codes par symbole, mapping Gray et detection d'ordre inverse.
- Calibration de quatre groupes de frequences; vote sur huit phases.
- Encodeur/decodeur de trame partage entre firmware et controles sur hote.
- Identifiant TDM 0xd6; incompatible avec la 0.5.1 sur l'air.
- Duree des fenetres, buffers, CRC, FEC et acquittements conserves.
- Calibration de salve portee de 80 a 160 ms, quatre pilotes de 40 ms.
- Exemples plafonnes a 300 us/symbole; diagnostics explicites symbolUs.
- Reinitialisation des echeances RX a l'heure courante, y compris apres
  le demi-tour de l'horloge micros().
- Pas de dithering et aucune nouvelle validation RF.
