# Validation et gel de ESP8266TDMAdaptive 0.5.1

## Perimetre de cette livraison

Cette archive fige le firmware deja en place, sans changer les fenetres de
1 seconde, les profils, la radio, les exemples, les scripts ou les binaires.
Aucun test logiciel ou radio, aucune compilation et aucun flash n'a ete
relance pour finaliser la livraison. Les controles de livraison portent
uniquement sur les fichiers, les empreintes et la structure de l'archive.

La demande de finalisation ne vaut pas reussite des essais non termines.
**Le dernier banc materiel 0.5.1 porte passed=false**, pour expiration
de l'attente du profil 6. La livraison est figee et installable, sans
qualification de portee ni garantie de debit maximal.

## Construction et tests logiciels anterieurs : 0.5.1

Journal existant : [validation/host-tests-v051.txt](validation/host-tests-v051.txt).
Il a ete recupere en lecture seule sur la VM lors de la finalisation.

Resultats deja obtenus :

- Codec : CRC16, Hamming et erreurs en rafale entrelacees, PASS.
- Gestionnaire fiable historique : identite des ACK, reprises, doublons,
  broadcast et rebouclage des compteurs, PASS.
- Adaptation : plafonds, penalites temporelles, qualite et rebouclage, PASS.
- Calcul de phase entier : quadrants, dynamique et erreur bornee, PASS.
- TDM : ACK cumulatifs, file de 448 octets, pertes de debut/milieu de salve,
  creneaux lents/rapides, backpressure, redemarrages simules et absence de
  chevauchement TX dans le modele, PASS.
- Journalisation : budget, ecritures nulles/courtes, rebouclage du tampon
  et signalement de saturation, PASS.

Ce sont des tests logiciels, pas des mesures RF.

Compilation A/B deja terminee avec ESP8266 Arduino core 3.1.2,
`esp8266:esp8266:d1_mini:xtal=160,eesz=4M` :

| Ressource | Utilisation |
| --- | ---: |
| RAM statique | 36464 / 80192 octets |
| IRAM, cache reserve inclus | 60347 / 65536 octets |
| Code flash | 258448 / 1048576 octets |
| Taille de chaque binaire distribue | 294144 octets |

Les deux derniers televersements avaient confirme la verification du hash.
Aucune relecture de la flash physique n'a ete faite lors de cette livraison.

## Dernier essai materiel : 0.5.1

Journal : [validation/buffered-200-v051.json](validation/buffered-200-v051.json).
Horodatage du journal : 2026-10-07T21:40:15.99255-04:00.
Duree : 164,260 s. Erreur finale : `timeout: target_rate_reached`.

Etapes reussies : disponibilite serie, redemarrages demandes, verrouillage
initial au profil 0, et verification a bas debit d'un paquet utile par
fenetre avec conservation du surplus et sans remplissage de fin de fenetre.

| Dernier etat complet | A | B |
| --- | ---: | ---: |
| Etat | LOCKED | LOCKED |
| Profil / periode | 5 / 300 us | 5 / 300 us |
| Messages livres, compteur embarque | 187 | 165 |
| Livraisons verifiees independamment dans les traces | 186 | 165 |
| Messages acquittes, compteur embarque | 165 | 183 |
| Messages encore en file TX | 32 | 32 |
| Rendez-vous manques | 7 | 7 |
| Tentatives de retransmission | 45 | 0 |
| Echecs TX / abandons PHY TX | 0 / 0 | 0 / 0 |
| Plus grand retard TX rapporte | 8 us | 9 us |

Les dernieres lignes des deux cartes ne sont pas simultanees. Le producteur
continuait a remplir les files; les comptes livraison/ACK ne doivent donc
pas etre interpretes comme un test de vidage final reussi.

Le banc n'a pas atteint le profil 6 dans le delai prevu. Le palier de
60 secondes a 200 us/bit, le vidage complet, la comparaison finale de tout
le flux et les textes manuels places apres ce palier n'ont pas ete executes
jusqu'a leur validation dans cette execution.

Une ligne TDM tronquee est signalee a t=32,711 s et contient une ligne
RXDATA accolee. Cela explique la difference entre compteurs et livraisons
verifiables dans le journal; ce n'est pas une preuve de corruption radio.
Le probleme de diagnostics serie ne peut pas etre declare elimine.

Aucune exception/watchdog inattendu n'est consigne. Les deux demarrages
comptes par carte correspondent a l'ouverture/reinitialisation du banc,
pas a une mesure de fiabilite a long terme.

### Debit utile observe en fin d'essai

A : 167 -> 187 livraisons entre t=151315 et 163321 ms embarques.
B : 145 -> 165 livraisons entre t=150265 et 162271 ms embarques.
Tous ces messages de demonstration font 14 octets.

Pour chaque sens : 20 * 14 / 12,006 = environ **23,32 octets/s**,
soit **186,6 bits utiles/s**, pertes et alternance incluses.
Il s'agit de courts intervalles a 300 us/bit, pas d'une garantie permanente.

La cadence IQ estimee pendant le travail RX est d'environ 36,5 k/s sur A
et 36,7 k/s sur B. Elle ne correspond pas au debit utile transmis.

## Essai anterieur reussi : 0.5.0 uniquement

Journal : [validation/buffered-200-v050.json](validation/buffered-200-v050.json).
Horodatage : 2026-10-07T21:32:10.29248-04:00.
Duree totale : 159,402 s. Resultat global : PASS.

Le banc a valide le bas debit, la montee au profil 6, une observation
de 60 secondes a 200 us/bit, puis le vidage des files et la concordance
des ACK/livraisons. 289 livraisons vers A et 280 vers B ont ete controlees
independamment, dans l'ordre, sans doublon applicatif ni trou dans le flux.
Le plateau montre six paquets de 14 octets par fenetre TX, sans remplissage.

Les instantanes de compteurs encadrant ce plateau ne couvrent pas exactement
60 secondes sur chaque carte : les diagnostics sont periodiques.

| Mesure des compteurs | Vers A | Vers B |
| --- | ---: | ---: |
| Nouveaux messages | 168 | 159 |
| Octets utiles | 2352 | 2226 |
| Intervalle des horloges embarquees | 56,028 s | 56,971 s |
| Debit utile sur cet intervalle | 41,98 octets/s | 39,07 octets/s |

Des retransmissions ont eu lieu; ce PASS ne signifie pas une radio sans pertes.
Cadences IQ rapportees au plateau : 36526 et 36606 estimations/s.
Ces resultats concernent 0.5.0, avant le changement de journalisation de 0.5.1.
Ils ne constituent pas un PASS global de la version livree ici.

## Essai manuel anterieur interrompu : 0.5.0

Journal : [validation/buffered-manual-initial.json](validation/buffered-manual-initial.json).
Duree : 29,881 s. Resultat global : FAIL.
Le lecteur a rencontre une ligne RXDATA incomplete et s'est arrete sur
la cle absente `valid`, pendant l'attente de vidage des files.
Les deux textes manuels n'ont donc pas ete valides par cet essai.
Le journal est conserve, sans requalification de son resultat.

## Conditions et limites d'interpretation

L'utilisateur a indique que les cartes etaient eloignees et que davantage
d'interferences etaient presentes le soir. Ces conditions peuvent affecter
la liaison, mais distance, attenuation, spectre et temperature n'ont pas ete
mesures dans une comparaison controlee. Les journaux seuls ne permettent
pas d'attribuer avec certitude chaque baisse a la radio ou a la journalisation.

Les fenetres, les reprises et la garde sont testees dans le modele logiciel;
il n'existe pas ici de mesure HackRF de leurs frontieres RF.
Aucune qualification industrielle, garantie de debit minimal ou de portee
n'est revendiquee.

## Identification des binaires 0.5.1

Ces fichiers sont les copies des binaires utilises pour le dernier flash
confirme dans cette conversation, sans reconstruction lors de la livraison.

| Fichier | SHA-256 |
| --- | --- |
| firmware/TDM_A.ino.bin | c5b252178ada1adfe2d9445811c1b0a6d85e7f781f7428debddab57b4d8e4443 |
| firmware/TDM_B.ino.bin | f732d13458ab8aeb514689bc782f05a78a515203ea7c84d405ca15ba812d0c99 |

A : MAC b4:e6:2d:23:0e:f4, port du banc /dev/ttyUSB1.
B : MAC 68:c6:3a:d6:0e:e8, port du banc /dev/ttyUSB0.

RELEASE.json identifie les fichiers d'origine figes. SHA256SUMS.txt couvre
aussi la documentation de cette livraison. Les anciens journaux sont
reproduits a l'identique.
