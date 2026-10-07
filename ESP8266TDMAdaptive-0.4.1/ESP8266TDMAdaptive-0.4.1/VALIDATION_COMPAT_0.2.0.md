# Validation du modem ESP8266 2-FSK

Essais du 6 octobre 2026 sur les deux Wemos connectes a la VM. Le profil retenu
est **160 MHz, 3000 us par bit, ASK=0, APWR=0**, avec calibration, CRC16, correction
Hamming, entrelacement et ACK adresses. La campagne principale a confirme
60 messages sur 60, avec des retransmissions. Ce resultat de banc ne constitue
pas une certification universelle de fiabilite ou de portee.

## Configuration

| Element | Valeur |
| --- | --- |
| Arduino CLI et core ESP8266 | 1.5.1 et 3.1.2 |
| Carte | Wemos D1 Mini, 4 Mo |
| Carte A | ttyUSB0, MAC 68:c6:3a:d6:0e:e8 |
| Carte B | ttyUSB1, MAC b4:e6:2d:23:0e:f4 |
| Profil Arduino | esp8266:esp8266:d1_mini:xtal=160,eesz=4M |
| Canal | 6, centre nominal 2437 MHz |
| Codes TONE | F0=16, F1=1016 |
| Puissance | ASK=0, APWR=0 |
| Calibration par trame | 40 ms F0 puis 40 ms F1 |
| Synchronisation | 36 bits alternes puis marqueur 12 bits |
| Charge utile | 0 a 32 octets, essais RF sur 1, 8 et 32 |
| Protection | Hamming(7,4), entrelacement de 4 octets, CRC16-CCITT-FALSE |
| ACK | Une trame de 1 octet, destinataire/source/identifiant/drapeau/contenu controles |
| Retries | 5 au maximum, en plus de la premiere tentative |
| Attente ACK | 1100 ms plus une dispersion aleatoire de 0 a 1099 ms |
| Retournement | ACK apres 100 ms puis preparation TX d'environ 80 ms |
| Garde RX apres TX | 120 ms sans bloquer le loop |
| Refroidissement du banc | 1500 ms entre les essais, aucune coupure entre les bits |

Selon le modele empirique TONE des references du projet (pas de 78,125 kHz,
bouclage de 80 MHz), les codes correspondent nominalement a 2438,250 MHz
et 2436,375 MHz. Ces frequences absolues n'ont pas ete mesurees au spectrometre
durant cette campagne.

Les valeurs signees issues d'I/Q sont des estimations internes, pas ces
frequences RF absolues. Par exemple, la campagne principale mesure environ
-99/+17 kHz sur B et -49/+56 kHz sur A pour les memes codes TX.
Les seuils sont appris a chaque reception. L'acquisition I/Q conserve MODE=0,
N=4 et huit acquisitions courtes par estimation; E4 n'est pas le decodeur FSK.

## Resultats des campagnes

Chaque message confirme exige a la fois un ACK accepte par l'emetteur et une
unique reception de la bonne longueur et du bon contenu sur l'autre carte.
Les sens A vers B et B vers A sont alternes. Les echecs sont conserves au
denominateur; un message recu mais non acquitte compte comme echec de confirmation.

| CPU | Bit | Messages confirmes | Retries | Verdict |
| --- | ---: | ---: | ---: | --- |
| 160 MHz | 3000 us | 60/60 | 31 | Profil retenu, campagne principale |
| 160 MHz | 2000 us | 12/12 | 5 | Essai court reussi, profil experimental |
| 80 MHz natif | 3000 us | 6/12 | voir journal | Non valide |
| 80 MHz natif | 4000 us | 9/12 | voir journal | Non valide |

La campagne principale compte 30 messages par sens, 20 par longueur.
41/60 passent sans retransmission. Aucun doublon livre ni reset observe.
Les essais a 80 MHz recompilent et reflashent reellement le firmware avec
xtal=80; ils ne se limitent pas a modifier une option sur le PC.

Ces campagnes precedent uniquement l'ajout final de l'arret explicite par
setMode/sleep. Le format radio et le traitement RX/TX normal sont identiques.
La verification du livrable exact est detaillee plus bas.

### Latence et debit utile a 3000 us

| Charge utile | Confirmes | Sans retry | Temps minimal | Temps moyen | Temps maximal |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 octet | 20/20 | 13/20 | 1465 ms | 3107 ms | 9289 ms |
| 8 octets | 20/20 | 15/20 | 1801 ms | 2704 ms | 9387 ms |
| 32 octets | 20/20 | 13/20 | 2809 ms | 4750 ms | 14278 ms |

Les temps incluent preparation PHY, transmission, ACK et retries eventuels;
ils excluent le refroidissement de 1500 ms. Le debit de modulation est
333,33 bits codes/s, pas 333 bits de charge utile/s.

Pour 32 octets, 256/2,809 donne **91,1 bits utiles/s** sur un echange sans retry,
ACK compris. Le debit calcule sur le temps moyen est **53,9 bits utiles/s**,
ou **41,0 bits/s** en ajoutant la pause de 1,5 s a chaque echange.
Les durees radio calculees sont 602, 938 et 1946 ms pour 1, 8 et 32 octets;
l'ACK prend 602 ms.

A 2000 us, le meilleur echange de 32 octets prend 2015 ms, soit 127,0 bits
utiles/s ACK compris, mais le temps moyen de ces quatre echanges est 5242 ms.
Ce profil court ne demontre donc pas un meilleur debit moyen fiable.
**81,6 bits/s n'est pas un maximum physique etabli.**

## CPU et memoire

Pendant cinq secondes d'ecoute sans trame, le temps comptabilise dans le pilote
RX represente 24,4 a 24,8 % du temps ecoule a 160 MHz, contre 40,7 a 41,8 % a
80 MHz. Il ne s'agit pas de toute la charge CPU : le SDK et le sketch ne sont
pas inclus dans ce compteur. Pendant la reception d'une trame, le rythme
d'acquisition augmente; ces pourcentages de veille ne s'y appliquent pas.

Sur la campagne principale a 160 MHz, un appel RX prend au maximum 628 us,
et le retard maximal d'une transition TX est 61 us. Le banc execute plusieurs
millions de passages dans loop. Le heap libre reste a 48648 octets sur cette
version; le livrable final avec les diagnostics d'arret conserve 48600 octets.

Compilation du banc final : RAM statique 30696/80192 octets; flash programme
251104/1048576 octets. L'IRAM executable utilise 27055/32768 octets.
Le total affiche de 59823/65536 inclut aussi les 32768 octets reserves au cache
d'instructions : il ne signifie pas que 91 % de la RAM de donnees est occupee.

L'emission brute est cooperative apres environ 82 ms de preparation de send().
poll() doit continuer a etre appele. available()/recv() traitent au plus une
estimation I/Q par appel. Pour 3000 us/bit, viser un loop disponible au moins
toutes les 500 us pendant une trame.

sendtoWait(), waitPacketSent() et l'emission automatique des ACK restent
synchrones pour le sketch, avec yield vers le SDK. Le gestionnaire fiable
n'est pas encore entierement evenementiel. Le Wi-Fi normal ne peut pas
partager simultanement ce PHY.

## Corrections livrees

- Restauration complete du passage TX vers RX : gate et horloge TX coupees,
  registres PBUS sauvegardes puis restaures.
- Reception en flux, petites structures fixes, calibration propre a chaque
  reception et synchronisation tenant compte de la polarite.
- Suppression des longs captures/boucles TX provoquant des blocages ou resets.
- Charge utile de 32 octets, CRC16, correction Hamming et entrelacement contre
  les courtes rafales d'erreurs.
- ACK adresses et controles; rejet des simples tonalites comme preuve de livraison.
- Retries bornes, calculs de temps resistants au rebouclage et deduplication.
- Arret RF explicite lors d'une interruption, d'un timeout TX ou d'un retard excessif detecte au prochain appel de poll().

## Verification du livrable final

Apres recompilation et flash des deux cartes, la regression finale confirme
**12/12 messages**, 6 dans chaque sens, sur 1, 8 et 32 octets, avec 3 retries.
Aucun doublon livre, reset observe ou abandon TX involontaire. Les deux gates
sont a zero en fin de test. Ce resultat s'ajoute a la campagne de 60 messages,
sans la presenter comme une campagne unique du binaire final.

Les huit controles materiels passent :

- send() sur 32 octets revient en 82 ms, avant la fin de la trame.
- Cette trame brute de 32 octets est effectivement recue et verifiee.
- Destinataire silencieux : false apres les cinq retries, en 13677 ms.
- Mauvaise adresse : false apres les cinq retries, en 13154 ms.
- Adresse correcte retablie : succes en 1466 ms, sans reset necessaire.
- setMode(Idle) durant TX coupe la gate et signale un envoi non termine.
- sleep() durant TX coupe la gate et retourne false (pas de veille PHY implementee).
- waitPacketSent(1) expire, coupe la gate et retourne false.

Les trois abandons volontaires sont bien comptes par txAborts. Le test normal
suivant revient a zero abandon. Le heap reste a 48600 octets.
Le plus long appel RX mesure dans cette regression est de 640 us.

Les tests natifs passent : vecteur standard CRC16, erreurs Hamming simples,
toutes les rafales contigues de 1 a 8 bits sur 256 blocs entrelaces, rejet des
ACK incorrects, borne de 255 retries, rebouclage temporel, doublons et broadcast.
Les quatre exemples utilisateur ont ete compiles; le banc final est recompile
et flashe apres la protection d'arret.

Les deux cartes sont laissees avec link_bench, a 160 MHz et 3000 us, en mode
QUIET, sans emission spontanee. Les sources sont egalement installees dans
/home/kaboom/Arduino/libraries/ESP8266FSKRadioHeadCompat.

## Test additionnel a tres courte distance

Une serie distincte de 30 echanges a ete faite apres deplacement des cartes. La distance exacte n'a pas ete mesuree. Le profil etait 160 MHz, 3000 us/bit, ASK=0, APWR=0, avec 5 essais dans chaque sens par charge et 1500 ms entre essais.

| Charge | Confirmations | Retries cumules | Echecs apres retries | Latence moyenne | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 octet | 7/10 | 33 | 3 | 9454 ms | 14516 ms |
| 8 octets | 10/10 | 28 | 0 | 9201 ms | 16034 ms |
| 32 octets | 9/10 | 19 | 1 | 9923 ms | 21639 ms |

**Bilan : 26/30 confirmations**, 80 retransmissions, zero doublon ou reset. Dans les quatre echecs, le destinataire avait bien recu une fois le bon payload, mais sendtoWait() a retourne false : l'emetteur n'a pas valide l'ACK. La fiabilite a tres courte distance est donc inferieure a la campagne precedente (60/60). Les frequences estimees restent separees d'environ 82 a 116 kHz. Ces donnees seules ne permettent pas d'isoler la cause entre retournement half-duplex, variabilite PHY et environnement RF.

Les traces integrales sont dans validation/close-range.json et validation/close-range.log.

## Limites de la validation

Ces resultats concernent ces deux cartes, leur emplacement actuel et les
interferences presentes pendant les essais. Il n'y a pas de balayage de
distance, d'attenuation, de temperatures ni de lots de cartes dans cette
campagne. La calibration adaptee aux deux cartes ne prouve pas une
compatibilite universelle. Les registres PHY utilises restent experimentaux.

Le format radio est propre a cette bibliotheque. La ressemblance avec RadioHead
concerne l'API, pas l'interoperabilite radio. La deduplication sur identifiants
8 bits ne garantit pas une transaction unique apres reset/rebouclage.

Un CRC reduit fortement l'acceptation de donnees corrompues sans la rendre
mathematiquement impossible. Les retries ameliorent la livraison; ils ne
suppriment pas les echecs possibles ni les obligations de reactivite du sketch.

## Sources des mesures

Le dossier validation adjacent a la bibliotheque conserve les JSON et journaux :
final160.json, final80.json, slower80.json, fast160.json, negative.json et
release-check.json. Les essais historiques CRC8 sont conserves comme traces
de diagnostic, sans etre additionnes aux resultats du format final.
source-sha256.txt identifie les sources livrees.
