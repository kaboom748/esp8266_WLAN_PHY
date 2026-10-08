# Outils de test conserves

Les scripts et sources de test de cette livraison sont conserves sans
modification. Aucun n'a ete execute pour preparer le ZIP final.

## Outils associes a la branche buffered

- run_host_tests.sh : codecs, gestionnaire fiable, adaptation, phase,
  modele TDM et journalisation; compile les tests dans un repertoire temporaire.
- hardware_buffered.py : bas/haut debit, flux bidirectionnel ordonne,
  vidage/ACK puis textes manuels. Ouvre et reinitialise les cartes.
- hardware_live.py : vidage de la demonstration puis messages manuels.
- hardware_tdm.py : lecteur et primitives communes; son scenario autonome
  contient aussi des attentes heritees des anciennes files.

Les resultats deja obtenus sont identifies par version dans ../VALIDATION.md.
La presence d'un script n'implique pas qu'il ait entierement reussi sur 0.5.1.

## Outils historiques

hardware_iq35.py attend notamment du remplissage de fin de fenetre :
ce critere n'est plus celui de 0.5.x.
hardware_adaptive.py, hardware_bench.py, hardware_distance.py,
hardware_gain.py, hardware_manual.py, hardware_negative.py,
probe_adaptive.py et test_bench.py sont conserves comme outils historiques.
Leurs profils, temporisations ou hypotheses de protocole doivent etre
examines avant toute utilisation sur 0.5.1.

Ne pas lancer de test materiel en parallele d'un moniteur serie ou d'un autre
processus tenant les ports. L'ouverture USB peut redemarrer les ESP.
La finalisation du produit n'a lance aucun de ces outils.
