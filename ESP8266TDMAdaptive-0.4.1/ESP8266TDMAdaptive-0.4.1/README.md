# ESP8266TDMAdaptive 0.4.1 - liaison validee en proximite

Les essais materiels 0.4.1 ont reussi avec les deux ESP rapproches, sans
modifier ni reflasher le firmware de l'essai eloigne. Sur 180 secondes de
trafic : 151 messages acquittes dans chaque sens, aucune perte ni
retransmission, montee automatique a 3333 bit/s brut. Les messages manuels
"Bonjour A" et "Bonjour B" ont aussi ete verifies et acquittes.

La portee reste une limite : a leur ancienne position eloignee, les cartes
restaient a 333 bit/s avec des pertes. Ce resultat ne garantit pas le
fonctionnement a n'importe quelle distance. Voir VALIDATION.md pour les
mesures et la distinction entre debit brut et debit utilisateur.

Le sous-dossier firmware contient les binaires 0.4.1 actuellement flashes,
avec le correctif de conservation des trames corrigees et MODE IQ=1.
Les anciens rapports 0.4.0 sont conserves comme historique uniquement.

Liaison 2-FSK half-duplex autonome entre deux ESP8266. A rythme les
cellules TDM; B repond seulement a une cellule A validee par CRC.
Les cellules continuent sans donnees utilisateur. Aucun PC ni fil de
synchronisation n'est necessaire apres programmation.

## Cartes et versions

- VM : /home/kaboom/lab_tdm_adaptive_041.
- A : chip 230ef4, MAC b4:e6:2d:23:0e:f4, /dev/ttyUSB1 pendant les essais.
- B : chip d60ee8, MAC 68:c6:3a:d6:0e:e8, /dev/ttyUSB0 pendant les essais.
- Wemos D1 mini, ESP8266 core 3.1.2, CPU 160 MHz, flash 4 Mo.
- Programmer LES DEUX cartes : le format 0.4.0 est incompatible avec 0.3.0.
- La version precedente reste dans /home/kaboom/lab_tdm_dynamic.
- Voir VALIDATION.md pour les essais reels et leurs limites.

## Vitesse negociee

Les profils vont de 3000 a 300 microsecondes par bit :
3000, 2000, 1250, 800, 500, 300. Ce sont des candidats explores, pas une
garantie que chaque vitesse fonctionne dans chaque environnement.

A propose le prochain profil dans sa cellule. B confirme ou demande un
profil plus lent, en repondant a la vitesse actuelle. B bascule apres sa
reponse; A bascule apres reception de cette confirmation. Le prochain
rendez-vous conserve sa date prevue; les suivants utilisent la nouvelle duree.

Une confirmation perdue peut temporairement separer leurs vitesses.
Apres trois rendez-vous manques, retour au profil commun 3000 us, calibration
oubliee et reconstitution automatique. A laisse un cycle silencieux avant
de relancer; B n'emet jamais tout seul pour rechercher A. Les files restent
en RAM pendant cette reprise.

Chaque cellule porte la qualite de reception de son emetteur. La decision
utilise le pire des deux nombres de corrections Hamming, lisse sur environ
huit echanges. Quatre bons echanges autorisent une tentative plus rapide.
Deux pertes dans les huit derniers evenements, douze corrections dans une cellule, ou une moyenne
d'environ quatre corrections demandent un ralentissement. La montee exige
une moyenne d'au plus deux corrections. Chaque profil qui echoue attend
5, 10, 20, 40 puis au maximum 60 secondes avant un nouvel essai. L'attente
utilise l'horloge, pas un nombre de trames lentes. Les profils inferieurs
restent accessibles; 32 bons echanges sur le profil effacent sa penalite.
Un simple plafond manuel n'ajoute pas de penalite radio.

Le decodeur compare les phases de synchronisation voisines pendant au plus
un symbole avant de retenir le candidat CRC valide demandant le moins de
corrections. Le calcul IQ evite les fonctions trigonometriques pour chaque
paire; un seul atan2 est conserve par estimation. L'intervalle entre les
mesures IQ est chronometre en cycles CPU, sans quantification a la microseconde.
Pas de tampon de capture
de 42 ko ni de recherche exhaustive d'une longue capture.

## Debit

Cellule de 32 octets, dont 14 utiles, CRC16 et Hamming(7,4) entrelace.
Les bits inutilises sont remplis avec 0x55. Le calcul de duree inclut
650 bits codes, 80 ms de calibration, 80 ms de preparation TX et
150 ms de garde par sens.

| Profil | Bit, us | Debit brut, bit/s | Maximum utile par sens, octets/s |
| --- | ---: | ---: | ---: |
| 0 | 3000 | 333 | 3.10 |
| 1 | 2000 | 500 | 4.35 |
| 2 | 1250 | 800 | 6.24 |
| 3 | 800 | 1250 | 8.43 |
| 4 | 500 | 2000 | 11.02 |
| 5 | 300 | 3333 | 13.86 |

Ce sont des maxima calcules sans pertes et sans transitions, pas des mesures.
Les ACK sont inclus dans les cellules; les retransmissions reduisent le debit.
Aucune vitesse minimale ni aucune portee n'est garantie.

F0/F1 sont recalibres a la reception, avec polarite automatique et valeurs
signees 32 bits. Canal 6, puissance et codes de tonalite TX 16/1016 restent
fixes. Il n'y a pas de changement automatique de canal ou de puissance.
Les valeurs f0/f1 du diagnostic sont des estimations repliees, pas des
frequences RF absolues ni une mesure directement utilisable pour regler la PLL.
Les acquisitions IQ sont cadencees en IRAM, interruptions preservees/restaurees
autour d'une courte rafale; ce changement ne constitue pas une AFC de la PLL.

RX revient au mode AGC apres TX, avec restauration explicite du chemin
analogique PBUS(2,1)=0x1fe. Le mode manuel de diagnostic ne modifie que le
chemin RX et les gains RF/VGA, jamais APWR=0 ni ASK=0.

MODE IQ=1 est utilise avec le champ N=4 et la meme cadence courte que MODE=0.
Ce choix vient d'une comparaison materielle 0/1/0, pas d'une assimilation de
MODE=1 a une mesure d'amplitude seule. Les deux modes restent selectionnables.
Une trame CRC valide en attente de comparaison des phases est maintenant
protegee contre les temporisateurs de recherche et de duree de trame.

## Utilisation

Installer le ZIP depuis le gestionnaire de bibliotheques ZIP de l'IDE Arduino.
Choisir la carte LOLIN/WEMOS D1 mini (ESP8266), CPU 160 MHz, flash 4 Mo;
le coeur utilise pour les essais est ESP8266 3.1.2. Ouvrir l'exemple TDM_A
pour la premiere carte et TDM_B pour la seconde. Les deux cartes doivent
utiliser cette meme bibliotheque. Ce n'est pas une bibliotheque ESP32.

Les exemples TDM_A et TDM_B envoient un compteur binaire verifiable.
Port serie 115200 bauds, lignes terminees par LF.

Pour envoyer ses propres messages avec les exemples fournis :

1. Envoyer `/auto off` sur LES DEUX cartes et attendre `queue=0` dans leurs
   etats. Seules les donnees de demonstration s'arretent; le TDM continue.
2. Envoyer `/send Bonjour B` sur A. B affiche un RXDATA de 9 octets,
   `hex=426f6e6a6f75722042`. Le compteur `ack` de A augmente lorsque B
   confirme la reception.
3. Envoyer `/send Bonjour A` sur B. A affiche
   `hex=426f6e6a6f75722041`; le compteur `ack` de B augmente.
4. `/stats` demande un etat; `/auto on` reactive la demonstration.

La limite est de 14 OCTETS par message, pas 14 caracteres Unicode. Les
messages plus longs doivent etre decoupes par l'application. C'est un
transport par messages, pas un pont serie transparent ni du Wi-Fi/IP.
Les sorties serie du suiveur attendent une garde TDM; elles peuvent donc
etre differees s'il n'a pas encore recu de trame valide.

Dans la VM du banc, A utilise /dev/ttyUSB1 et B /dev/ttyUSB0. Les ports USB
sont actuellement affectes a la VM; cela ne definit pas leurs numeros COM
sous Windows. Un seul programme doit ouvrir chaque port serie a la fois.

| Commande | Action |
| --- | --- |
| /stats | Etat, debit choisi et compteurs. |
| /gain agc | Revenir a l'AGC materiel RX (defaut au demarrage). |
| /gain RF VGA | Diagnostic manuel : RF 0..6, VGA 0..7; persiste entre TX/RX, pas au reboot. |
| /iq 0 ou /iq 1 | Comparer les modes IQ a N=4 identique; oublie la calibration. Defaut : 1. |
| /limit 0 | Demander le profil lent, par negotiation radio. |
| /limit 5 | Autoriser tous les profils; valeur par defaut. |
| /limit N | Plafond local N de 0 a 5, jamais un changement unilateral. |
| /auto off | Arreter les donnees de demonstration; cellules vides maintenues. |
| /auto on | Reprendre les donnees de demonstration. |
| /send Bonjour | Mettre 1 a 14 octets en file. |
| /pause | Suspendre TX en conservant la file. |
| /run | Reprendre au profil commun lent. |
| /recal | Oublier les centres F0/F1. |
| /reset | Redemarrer; les files en RAM sont perdues. |

bitUs indique le profil actif; up/down comptent les changements et recovery
les retours au rendez-vous commun. corr/peerCorr concernent les cellules
acceptees. crc/reject comptent aussi les hypotheses des differents decodeurs :
ce ne sont PAS directement des taux de perte. Utiliser miss/retry/ack pour
la fiabilite des echanges.
ADAPT indique waitMs, good, qualityQ4 (corrections moyennes multipliees par 16),
bad et losses. RF donne les mots PBUS reellement lus, l'etat manuel, le chemin
RX et le champ ASK TX relu. APWR est la consigne passee a set_ana_scale, pas
une mesure de puissance rayonnee. IQ indique la cadence, sa variation et le mode.
rxStop=1 est normal en gain manuel : il arrete le decodeur Wi-Fi numerique,
pas necessairement l'estimateur IQ ni le chemin analogique.
FRAME distingue les candidats CRC valides (plusieurs phases pour une trame),
les trames retenues, les rejets d'adresse et les candidats abandonnes. Une
commande explicite de reinitialisation peut aussi abandonner un candidat.

Les files TX et RX contiennent chacune quatre messages. send() signifie
mise en file, pas acquittement. Les doublons sont acquittes sans nouvelle
livraison; une file RX pleine n'acquitte pas une donnee non conservee.

## Integration

Inclure ESP8266TDM.h, construire RH_ESP8266FSK radio puis ESP8266TDM link(radio).
Appeler link.begin(ESP8266TDM::MASTER) sur A et FOLLOWER sur B.
Appeler link.poll() continuellement; send()/recv() gerent les messages.
bitUs(), rate(), statistics(), queued() et maintenanceWindow() donnent
l'etat. setRateLimit(N) impose un plafond negocie; setAdaptive(false)
desactive les propositions automatiques locales, sans protocole radio manuel.

A haute vitesse, toute operation longue dans loop() peut faire perdre des
symboles. Reserver les sorties serie et traitements longs aux gardes signalees
par maintenanceWindow(). Ne pas utiliser le Wi-Fi ordinaire simultanement.

CRC et sessions ne sont ni du chiffrement ni de l'authentification. Files et
deduplication ne survivent pas a une coupure electrique. Pour une commande
a effet durable, utiliser un identifiant applicatif persistant et une action
idempotente.

## Compilation et essais

Depuis /home/kaboom/lab_tdm_adaptive_041 :

```sh
arduino-cli compile --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M --library . --build-path build_a examples/TDM_A
arduino-cli compile --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M --library . --build-path build_b examples/TDM_B
arduino-cli upload -p /dev/ttyUSB1 --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M --input-dir build_a examples/TDM_A
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M --input-dir build_b examples/TDM_B
sh tests/run_host_tests.sh
python3 tests/test_bench.py
python3 tests/hardware_adaptive.py --cells 100 --output adaptive-validation.json
```

Le banc materiel utilise pyserial, redemarre les cartes, verifie leurs
donnees, demande un ralentissement via B, coupe temporairement les reponses,
puis verifie la reprise. Il ferme les ports et laisse la demonstration active.

Les documents *_COMPAT_0.2.0.md et exemples historiques ne valident pas
cette version adaptative. Ne pas installer deux bibliotheques qui definissent
les memes classes RH dans un meme sketch; utiliser explicitement --library.
Dans l'IDE Arduino, retirer l'ancienne installation de cette bibliotheque
avant d'installer ce ZIP pour eviter une selection ambigue des en-tetes.
