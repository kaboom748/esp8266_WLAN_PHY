# Diagnostic ESP8266TDMAdaptive 0.4.1

Essais du 6 octobre 2026 dans les deux positions indiquees par l'utilisateur.
Distance, orientation et niveau RF absolu non mesures. La liaison est
validee dans la position rapprochee testee; la position eloignee reste
problematique. Le firmware est identique entre ces deux derniers essais.

## Dernier Resultat : PASS En Proximite

Journal `validation/close-same-firmware.json`, fin le 6 octobre 2026
a 20:33:04 EDT. Duree 351,664 s. Aucun flash ni modification du firmware
entre l'essai eloigne et l'essai rapproche. Meme banc hardware_distance.py,
meme observation de 180 secondes apres verrouillage initial.

| Mesure | Eloignes | Rapproches, meme firmware |
| --- | --- | --- |
| Verrouillage initial | 54,55 s | 20,653 s |
| Acquittements supplementaires pendant les 180 s, A / B | 31 / 32 | 151 / 151 |
| Rendez-vous manques pendant ces 180 s, A / B | 8 / 8 | 0 / 0 |
| Profil maximal observe | 3000 us/bit | 300 us/bit |
| Debit brut correspondant | 333 bit/s | 3333 bit/s |
| Acceleration autonome | Non | Oui, six profils parcourus |

Le profil maximal apparait sur les deux cartes vers 69,9 s apres lancement.
Les 151 acquittements par sens incluent la montee en vitesse; le debit utile
maximal du protocole est environ 13,86 octets/s PAR SENS, pas 3333 octets/s.

Autres controles reussis : ralentissement demande sur B et negocie par RF,
reacceleration en 21,952 s apres retrait du plafond, maintien des cellules
vides, retour au debit commun apres interruption, conservation et livraison
du message en file, redemarrage de chaque carte, et recalibration a froid.
Le total comprend 176 et 175 livraisons independamment verifiees. Les pertes
provoquees par les interruptions/redemarrages sont hors de la fenetre de
trafic sain de 180 s. Une ligne de diagnostic serie tronquee a ete ignoree
par le banc; aucune erreur de validation applicative n'a ete detectee.

## Communication Manuelle : PASS

Journal `validation/manual-close-same-firmware.json`, banc
`tests/hardware_manual.py`, duree 27,732 s. Apres `/auto off` sur les deux
cartes et vidage des files :

- `/send Bonjour B` depuis A : texte exact recu sur B et acquitte sur A.
- `/send Bonjour A` depuis B : texte exact recu sur A et acquitte sur B.

La demonstration automatique est reactivee a la sortie du banc. Aucun
changement du code ou des parametres RF pour cet essai.

## Historique De L'Investigation A Distance

## Defaut RX identifie et corrige

Le temporisateur de recherche (600 ms depuis la synchronisation) pouvait
effacer un candidat deja valide par CRC, pendant l'attente d'un symbole
destinee a comparer les phases de decodage. Le chemin sans correction
livrait immediatement, ce qui masquait ce defaut sur un signal plus propre.
Les temporisateurs de recherche et de duree de trame respectent maintenant
ce candidat jusqu'a la fin de la comparaison bornee.

Comparaison materielle, meme gain RF=2/VGA=2, MODE=0, N=4, 3000 us/bit,
APWR=0 et ASK=0 :

- Avant : B observe 13 candidats CRC valides, abandonne 3 trames en attente,
  et ne livre aucune cellule TDM. Journal `validation/gain-frame-diagnostic.json`.
- Apres : deux cellules recues et verifiees dans chaque sens sur environ
  25 secondes; corrections observees 5 et 9 sur B, aucun candidat abandonne.
  Journal `validation/gain-frame-fixed.json`.
- Ce correctif ne supprime pas les pertes RF restantes.

## Comparaison IQ

Trois passages de 36 secondes : MODE 0, MODE 1, MODE 0; N=4, cadence mesuree
3025 ns, AGC actif, meme debit lent et memes reglages TX.

| Mode | Cellules recues A / B | Rendez-vous manques A / B |
| --- | --- | --- |
| 0, premier passage | 4 / 4 | 2 / 2 |
| 1 | 6 / 7 | 0 / 0 |
| 0, second passage | 4 / 5 | 2 / 2 |

Journal : `validation/iq-modes-fixed.json`. La pause de fin peut couper un
dernier echange en cours; ces comptes ne sont pas des mesures de debit utile.
MODE=1 est retenu pour l'essai prolonge, sans garantie generale sur ce bit
non documente officiellement. Les deux modes donnent des trames valides.

Les journaux de comparaison ont `passed=false` car ce sont des diagnostics,
pas des validations completes du protocole.

## Essai Long : Echec Du Critere D'Acceleration

Journal `validation/distance-candidate-fixed.json`, fin le 6 octobre 2026
a 20:20:47 EDT. Duree 234,552 s, MODE=1, AGC actif, six profils autorises.

- Verrouillage des deux roles en 54,55 s, puis observation pendant 180 s.
- 38 livraisons independamment verifiees sur A et 40 sur B. Les compteurs
  embarques indiquent 39 et 40; deux lignes serie tronquees sont signalees
  par le banc et ne sont pas prises pour des mesures valides.
- 39 acquittements sur chaque carte; rendez-vous manques : A=11, B=10.
- Aucun candidat CRC valide abandonne : A=0, B=0. Pas de rejet d'adresse.
- Aucun changement de vitesse : profil 0, 3000 us/bit, soit environ
  333 bit/s brut. Le niveau de corrections sur B bloque la montee.
- Aucun echec TX ni watchdog observe. Les tests de plafond de debit,
  panne simulee et redemarrages n'ont PAS ete atteints dans cette execution.

Conclusion : le correctif RX est confirme par les compteurs et les donnees,
mais la liaison a cette distance n'est pas validee comme fiable et rapide.
Les tests logiciels CRC/Hamming, ACK, adaptation et protocole TDM passent;
ils ne remplacent pas la validation RF.

## Derniere Comparaison De Gain

Apres correction, MODE=1, trois passages de 36 s a 3000 us/bit, gain relu
sur les deux cartes avant chaque passage, VGA manuel=2 :

| Gain | Cellules recues A / B |
| --- | --- |
| RF manuel 0 | 4 / 5 |
| RF manuel 1 | 2 / 3 |
| AGC | 5 / 6 |

Journal : `validation/gain-low-fixed.json`. Ce petit echantillon ne montre
pas d'amelioration en augmentant le gain et ne prouve pas que l'AGC est
optimal. Les essais RF=2/4/6 anterieurs au correctif etaient confondus par
l'abandon logiciel des trames corrigees : ils ne permettent pas de classer
objectivement ces gains.

Etat laisse sur les cartes : firmware 0.4.1 experimental, MODE IQ=1 par
defaut, AGC materiel, APWR=0, ASK=0 et plafond de debit 5. Pas d'adaptation
logicielle automatique du gain ajoutee, ni de correction PLL/AFC.
Le banc `tests/hardware_gain.py` permet de reproduire ces comparaisons;
`--require-corrected` ajoute les assertions de livraison avec corrections
et d'absence d'abandon de candidat.

## Archive 0.4.0 - Ne Valide Pas 0.4.1

Le rapport ci-dessous est conserve uniquement comme historique des cartes
proches et de l'ancienne version.

# Validation ESP8266TDMAdaptive 0.4.0

Essai complet termine le **6 octobre 2026 a 19:16:27 EDT**, soit 23:16:27 UTC.
Duree : 334,941 secondes. Resultat : **PASS**. Cartes proches; portee non mesuree.

Journal : `validation/adaptive-validation.json`.
Banc : `tests/hardware_adaptive.py --cells 100`.

## Resultats materiels

| Verification | Resultat |
| --- | --- |
| Donnees acquittees | 100 messages de 14 octets dans chaque sens en 177,142 s, montee en vitesse comprise |
| Pertes pendant cette serie de 100 | 0 rendez-vous manque, 0 retransmission, 0 echec TX sur les deux cartes |
| Montee autonome | Six profils parcourus, 3000 / 2000 / 1250 / 800 / 500 / 300 us par bit |
| Profil maximal atteint | Environ 121,6 s apres le lancement du banc |
| Ralentissement demande uniquement sur B | A et B passent de 300 a 500 us/bit par negotiation radio, en 2,15 s |
| Remontee apres retrait de cette limite | Retour a 300 us/bit en 31,62 s, sans intervention sur A |
| Files applicatives vides pendant 22 s | Cellules de synchronisation maintenues, sans nouvelle livraison utilisateur |
| Reponses de B arretees | A detecte la perte et revient au profil commun 3000 us/bit en 5,53 s |
| Message en attente pendant cet arret | Texte `retained` observe et acquitte apres reprise; file conservee |
| Reprise apres /run sur B | Deux cartes LOCKED et message acquitte en 14,33 s |
| Redemarrage logiciel de B | Nouvelle session et retour a LOCKED en environ 20,34 s depuis la commande |
| Redemarrage logiciel de A | Nouvelle session et retour a LOCKED en environ 20,74 s depuis la commande |
| Calibration F0/F1 effacee sur les deux cartes | Quatre nouvelles receptions par carte et LOCKED en 18,08 s |
| Livraisons controlees independamment dans les traces | 137 vers A et 140 vers B, sur l'ensemble du banc |
| Corruptions / doublons parmi les livraisons verifiables | 0 / 0 |
| Redemarrages pendant le banc | Seulement les redemarrages demandes : deux demarrages observes par carte |

Les temps ci-dessus sont des observations, pas des limites garanties.
L'arret simule concerne les emissions de B, pas une coupure physique de son
alimentation. Les files et la deduplication ne survivent pas a une coupure electrique.

## Debit mesure

Mesure par variation du compteur d'acquittements, messages de 14 octets,
sur des intervalles a profil constant, apres exclusion de la transition.

| Bit, us | Brut, bit/s | Utile mesure par sens, octets/s |
| ---: | ---: | ---: |
| 3000 | 333 | 3,097 |
| 2000 | 500 | 4,347 a 4,348 |
| 1250 | 800 | 6,236 |
| 800 | 1250 | 8,433 |
| 500 | 2000 | 11,020 a 11,022 |
| 300 | 3333 | 13,859 |

Au dernier palier, la mesure porte sur 54 nouveaux ACK par sens en environ
54,55 s. Par rapport aux quelque 2,85 octets/s par sens du TDM 0.3.0,
le gain utile a ce palier est d'environ **4,9 fois**. Le debit brut est
multiplie par dix, mais gardes, calibration, FEC et en-tetes restent presents.
Ce n'est pas une garantie de conserver ce palier lorsque les cartes sont eloignees.

## Adaptation aux erreurs

Un essai exploratoire precedent avec les memes firmwares est conserve dans
`validation/adaptive-degradation-exploration.json`. Il montre les replis
automatiques de 300 vers 500, puis vers 800 us/bit lorsque les pertes
s'accumulent. Sept rendez-vous manques et sept retransmissions sur A ont
ete comptabilises avant les 100 acquittements par sens, sans corruption
detectee dans les donnees verifiables.

Cet essai exploratoire a ete interrompu cote PC pour ajuster la duree
d'attente du banc; son champ `passed: false` ne constitue pas un PASS global.
La validation complete est le journal distinct `adaptive-validation.json`.
Le modem est volontairement prudent apres plusieurs replis : la remontee
peut attendre plusieurs minutes, selon le profil et le nombre d'echecs precedents.

## Instrumentation et limites

Cinq lignes de diagnostic serie incompletes ont ete detectees pendant le
banc complet. Elles sont conservees dans `warnings` et ne remplacent pas
le dernier etat complet. Le nombre de livraisons verifiables dans le journal
ne doit donc pas etre confondu avec tous les messages acceptes par les cartes.
Le banc decrit les erreurs radio via les ACK, pertes et retransmissions,
pas en assimilant une ligne serie manquante a une perte radio.

Un premier passage du banc avait ete arrete par ce probleme de lecture;
le parseur a ete corrige, teste, puis la serie finale a ete executee sans
interruption. Reouvrir un port serie peut reinitialiser ces cartes : aucun
resultat de reprise du banc avec reouverture des ports n'est utilise comme
preuve de continuite. Les ports sont fermes apres le test final et les
demonstrations restent configurees en fonctionnement autonome.

La portee, l'attenuation, la temperature et la coexistence avec d'autres
emetteurs n'ont pas ete caracterisees. Canal, puissance et codes de tonalite
TX restent fixes. Les centres recus F0/F1, le timing et le profil de debit
s'adaptent; le TDM ne garantit pas un lien sans pertes.

## Tests logiciels et construction

- CRC16, Hamming, entrelacement et gestionnaire fiable historique : PASS.
- Protocole TDM : files, ACK, identites de session, doublons, pertes,
  redemarrages, debordement de micros(), absence de chevauchement TX : PASS.
- Negociation des six profils et baisse sur degradation de qualite : PASS.
- Confirmation perdue lors des montees et descentes, donnees en attente : PASS.
- Memes tests TDM avec AddressSanitizer et UndefinedBehaviorSanitizer : PASS.
- Test unitaire du rejet des diagnostics serie tronques : PASS.
- Compilation A et B : ESP8266 Arduino core 3.1.2, D1 mini, CPU 160 MHz, flash 4 Mo.
- RAM statique : 30360 / 80192 octets; heap observe : 48936 octets.
- IRAM, cache inclus : 59823 / 65536 octets; code flash : 250944 octets.

Firmwares testes, copies dans `firmware/`, SHA-256 :

```text
bab03995fd849efe392763dca7db28284194dbcdb0f2680e62e92b1d1fd827c1  TDM_A.ino.bin
64f59bad2c12e64b3fef973c742c1cc172c5b152166df892d4571baab822c884  TDM_B.ino.bin
```

VM : `/home/kaboom/lab_tdm_adaptive`. La version 0.3.0 reste preservee
dans `/home/kaboom/lab_tdm_dynamic` et dans son archive locale.
